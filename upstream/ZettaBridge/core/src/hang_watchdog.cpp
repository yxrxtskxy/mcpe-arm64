// Hang watchdog: per-thread last-activity slots (lock-free, allocation-free) plus a background
// sampler that turns "same activity, same counter, two samples running" into a report note. No
// guest semantics change here; a stuck thread is only ever described, never touched.
#include "zb/hang_watchdog.h"

#include <dirent.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <cstdlib>
#include <cstring>
#include <thread>

#include "zb/hitch_trace.h"
#include "zb/host_jni.h"
#include "zb/log.h"
#include "zb/process.h"
#include "zb/runtime_report.h"
#include "zb/syscalls.h"

namespace zb {

namespace {

// Fixed-size, never-grows table of per-thread activity slots. A guest thread claims a slot the
// first time it is seen and keeps it (tids are not reused across a run in a way that matters for
// diagnostics); a slot is identified by `tid` with 0 meaning free.
constexpr std::size_t kMaxSlots = 256;

struct Slot {
    std::atomic<bool> in_progress{false};
    std::atomic<std::int32_t> tid{0};
    std::atomic<std::uint8_t> kind{static_cast<std::uint8_t>(ThreadActivityKind::kNone)};
    std::atomic<std::uint32_t> id{0};
    std::atomic<std::uint64_t> counter{0};
    std::atomic<std::uint32_t> guest_lr{0};
    std::atomic<std::uint32_t> guest_sp{0};
};

Slot g_slots[kMaxSlots];

}  // namespace

namespace {
std::mutex g_profile_mutex;
std::unordered_map<std::int32_t, std::unordered_map<std::uint32_t, std::uint64_t>> g_profile;  // tid -> pc -> samples
std::uint64_t g_profile_total = 0;
}  // namespace

bool profiling_enabled() {
    static const bool enabled = [] {
        if (std::getenv("ZB_PROFILE") != nullptr) return true;
#ifdef __ANDROID__
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.zb.profile", value) > 0) return value[0] == '1';
#endif
        return false;
    }();
    return enabled;
}

void record_profile_sample(std::int32_t tid, std::uint32_t guest_pc) {
    hitch_note_sample(guest_pc);
    std::lock_guard<std::mutex> lock(g_profile_mutex);
    ++g_profile[tid][guest_pc];
    ++g_profile_total;
}

namespace {
// Logs the busiest guest threads since the last report, each with its hottest blocks, and starts
// counting afresh. Percentages are of all samples, so threads compare with each other.
void report_profile() {
    std::unordered_map<std::int32_t, std::unordered_map<std::uint32_t, std::uint64_t>> profile;
    std::uint64_t total;
    {
        std::lock_guard<std::mutex> lock(g_profile_mutex);
        profile.swap(g_profile);
        total = g_profile_total;
        g_profile_total = 0;
    }
    if (total == 0) return;
    std::vector<std::pair<std::int32_t, std::uint64_t>> threads;
    for (const auto& [tid, pcs] : profile) {
        std::uint64_t samples = 0;
        for (const auto& entry : pcs) samples += entry.second;
        threads.emplace_back(tid, samples);
    }
    std::sort(threads.begin(), threads.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    const auto percent = [total](std::uint64_t samples) {
        return 100.0 * static_cast<double>(samples) / static_cast<double>(total);
    };
    log("profile: %llu samples", static_cast<unsigned long long>(total));
    for (std::size_t t = 0; t < threads.size() && t < 8; ++t) {
        const auto [tid, samples] = threads[t];
        log("profile: thread %d %5.1f%%", static_cast<int>(tid), percent(samples));
        std::vector<std::pair<std::uint32_t, std::uint64_t>> hot(profile[tid].begin(), profile[tid].end());
        std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (std::size_t i = 0; i < hot.size() && i < 10; ++i) {
            log("profile:   %5.1f%% 0x%08x %s", percent(hot[i].second), hot[i].first,
                Process::describe_guest_address(hot[i].first).c_str());
        }
    }
}
}  // namespace

namespace {
// This runs on every guest syscall and host call, so the common path must not scan the table:
// each thread remembers its own slot after claiming it once. One thread_local, not two: in this
// dlopened library each access is a call into the dynamic linker's TLS resolver.
struct MySlot {
    Slot* slot = nullptr;
    std::int32_t tid = 0;
};
thread_local MySlot t_mine;

// Only the owning thread writes its slot's counter: a load and a store, not an atomic add.
void count(Slot& slot) {
    slot.counter.store(slot.counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
}  // namespace

void record_thread_activity(std::int32_t tid, ThreadActivityKind kind, std::uint32_t id) {
    if (tid == 0) return;
    MySlot& mine = t_mine;
    if (mine.slot != nullptr && mine.tid == tid) {
        mine.slot->kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
        mine.slot->id.store(id, std::memory_order_relaxed);
        count(*mine.slot);
        mine.slot->in_progress.store(true, std::memory_order_relaxed);
        return;
    }
    // Find an existing slot for this tid, or claim the first free one. A relaxed CAS race
    // between two threads claiming the same free slot for different tids is possible but
    // vanishingly rare and self-heals: the loser just tries the next slot next call.
    Slot* free_slot = nullptr;
    for (Slot& slot : g_slots) {
        const std::int32_t current = slot.tid.load(std::memory_order_relaxed);
        if (current == tid) {
            slot.kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
            slot.id.store(id, std::memory_order_relaxed);
            count(slot);
            slot.in_progress.store(true, std::memory_order_relaxed);
            mine = {&slot, tid};
            return;
        }
        if (current == 0 && free_slot == nullptr) free_slot = &slot;
    }
    if (free_slot == nullptr) return;  // table full; diagnostics only, never blocks the guest
    std::int32_t expected = 0;
    if (!free_slot->tid.compare_exchange_strong(expected, tid, std::memory_order_relaxed)) {
        // Lost the race; the winner's slot is not this tid's, try once more on the next call.
        return;
    }
    free_slot->kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
    free_slot->id.store(id, std::memory_order_relaxed);
    free_slot->counter.store(1, std::memory_order_relaxed);
    mine = {free_slot, tid};
}

void record_thread_activity(std::int32_t tid, ThreadActivityKind kind, std::uint32_t id, std::uint32_t guest_lr,
                            std::uint32_t guest_sp) {
    record_thread_activity(tid, kind, id);
    const MySlot& mine = t_mine;
    if (mine.slot == nullptr || mine.tid != tid) return;
    mine.slot->guest_lr.store(guest_lr, std::memory_order_relaxed);
    mine.slot->guest_sp.store(guest_sp, std::memory_order_relaxed);
}

void record_thread_activity_done(std::int32_t tid) {
    const MySlot& mine = t_mine;
    if (mine.slot != nullptr && mine.tid == tid) {
        mine.slot->in_progress.store(false, std::memory_order_relaxed);
        return;
    }
    for (Slot& slot : g_slots) {
        if (slot.tid.load(std::memory_order_relaxed) == tid) {
            slot.in_progress.store(false, std::memory_order_relaxed);
            return;
        }
    }
}

std::vector<ThreadActivitySample> snapshot_thread_activity() {
    std::vector<ThreadActivitySample> out;
    for (Slot& slot : g_slots) {
        const std::int32_t tid = slot.tid.load(std::memory_order_relaxed);
        if (tid == 0) continue;
        ThreadActivitySample sample;
        sample.tid = tid;
        sample.kind = static_cast<ThreadActivityKind>(slot.kind.load(std::memory_order_relaxed));
        sample.id = slot.id.load(std::memory_order_relaxed);
        sample.counter = slot.counter.load(std::memory_order_relaxed);
        sample.in_progress = slot.in_progress.load(std::memory_order_relaxed);
        sample.guest_lr = slot.guest_lr.load(std::memory_order_relaxed);
        sample.guest_sp = slot.guest_sp.load(std::memory_order_relaxed);
        out.push_back(sample);
    }
    return out;
}

// Ticks of CPU this thread has burned, from /proc/self/task/<tid>/stat, or 0 when unreadable.
// A thread whose activity never changes but whose CPU keeps climbing is spinning in translated
// code; one whose CPU stands still is blocked.
// What the kernel says about a thread: "gone" when it no longer exists, otherwise its scheduler
// state (R running, S sleeping, D uninterruptible), the CPU ticks it burned, and the kernel
// function it is waiting in. Zero CPU alone cannot tell an idle thread from a dead one.
std::string thread_kernel_state(std::int32_t tid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
    std::FILE* file = std::fopen(path, "re");
    if (file == nullptr) return "gone";
    char line[1024];
    const char* read = std::fgets(line, sizeof line, file);
    std::fclose(file);
    if (read == nullptr) return "gone";
    const char* cursor = std::strrchr(line, ')');
    if (cursor == nullptr) return "?";
    char state = '?';
    unsigned long long utime = 0, stime = 0;
    // Fields after "comm": state, ppid, pgrp, session, tty, tpgid, flags, min_flt, cmin_flt,
    // maj_flt, cmaj_flt, utime, stime.
    if (std::sscanf(cursor + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &state, &utime,
                    &stime) != 3) {
        return "?";
    }
    char waiting[64] = {};
    std::snprintf(path, sizeof path, "/proc/self/task/%d/wchan", tid);
    std::FILE* wchan = std::fopen(path, "re");
    if (wchan != nullptr) {
        if (std::fgets(waiting, sizeof waiting, wchan) == nullptr) waiting[0] = '\0';
        std::fclose(wchan);
    }
    char text[128];
    std::snprintf(text, sizeof text, "%c cpu=%llu%s%s", state, utime + stime,
                  waiting[0] != '\0' ? " in=" : "", waiting);
    return text;
}

// Every thread of this process with its name, scheduler state and wait channel, whether or not it
// ever ran guest code. Engine threads carry telling names (Flutter uses "1.ui", "1.raster",
// "1.io"), so this says which part of the guest is missing or waiting, and shows ART's own threads
// next to ours.
// Threads of the runtime itself (binder, ART daemons) crowd out the engine's own, and one report
// line holds only so much, so they are counted rather than listed and the rest goes out in chunks.
bool is_platform_thread(const char* name) {
    static const char* const kNoise[] = {"binder:", "Jit thread pool", "Profile Saver", "ADB-JDWP",
                                         "Signal Catcher", "perfetto", "HeapTaskDaemon",
                                         "ReferenceQueueD", "FinalizerDaemon", "FinalizerWatchd",
                                         "hwuiTask", "RenderThread", "queued-work-loo", "Timer-",
                                         "GPU completion", "SurfaceSyncGrou"};
    for (const char* prefix : kNoise) {
        if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) return true;
    }
    return false;
}

void write_threads_census() {
    DIR* dir = ::opendir("/proc/self/task");
    if (dir == nullptr) {
        runtime_report().note_watch_detail("threads", "(unavailable)");
        return;
    }
    std::string chunk;
    unsigned chunk_index = 0;
    unsigned platform = 0;
    unsigned listed = 0;
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::int32_t tid = std::atoi(entry->d_name);
        if (tid == 0) continue;
        char path[64];
        std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        char name[64] = {};
        if (std::FILE* comm = std::fopen(path, "re")) {
            if (std::fgets(name, sizeof name, comm) != nullptr) {
                char* newline = std::strchr(name, '\n');
                if (newline != nullptr) *newline = '\0';
            }
            std::fclose(comm);
        }
        if (is_platform_thread(name)) {
            ++platform;
            continue;
        }
        if (listed >= 48) break;
        ++listed;
        chunk += (chunk.empty() ? "" : " | ") + std::to_string(tid) + ":" + name + " " +
                 thread_kernel_state(tid);
        if (chunk.size() > 420) {
            runtime_report().note_watch_detail("threads-" + std::to_string(++chunk_index), chunk);
            chunk.clear();
        }
    }
    ::closedir(dir);
    if (!chunk.empty()) runtime_report().note_watch_detail("threads-" + std::to_string(++chunk_index), chunk);
    runtime_report().note_watch_detail("threads-platform", std::to_string(platform) + " runtime threads hidden");
}

// Every guest thread once, with where it is: its last syscall or host call, the guest return
// address of that call and the code addresses on its guest stack (a poor man's backtrace, enough
// to see which wait in which library a stalled thread is in).
void write_thread_dump(const std::vector<ThreadActivitySample>& samples) {
    unsigned written = 0;
    for (const ThreadActivitySample& sample : samples) {
        if (written >= 48) break;
        std::string line = describe_thread_activity(sample) + " " + thread_kernel_state(sample.tid);
        if (sample.guest_lr != 0) line += " lr=" + Process::describe_guest_address(sample.guest_lr);
        if (sample.guest_sp != 0) line += " stack:" + Process::describe_guest_stack(sample.guest_sp, 10);
        runtime_report().note_watch_detail("dump-" + std::to_string(sample.tid), line);
        ++written;
    }
}

std::string describe_thread_activity(const ThreadActivitySample& sample) {
    char text[64];
    switch (sample.kind) {
    case ThreadActivityKind::kSyscall:
        std::snprintf(text, sizeof text, "sys:%s%s", syscall_name(sample.id),
                      sample.in_progress ? "(inside)" : "");
        return text;
    case ThreadActivityKind::kHostCall:
        std::snprintf(text, sizeof text, "host:0x%x%s", sample.id, sample.in_progress ? "(inside)" : "");
        return text;
    case ThreadActivityKind::kNone:
    default:
        return "(none)";
    }
}

HangWatchdog::HangWatchdog(SnapshotFn snapshot) : snapshot_(std::move(snapshot)) {}

bool HangWatchdog::sample(Clock::time_point now) {
    const std::vector<ThreadActivitySample> current = snapshot_();
    // Where every guest thread is, rewritten every five samples whether or not anything counts as
    // stuck: a stall made of timed waits keeps every counter moving.
    static unsigned samples = 0;
    if (++samples % 5 == 0) write_thread_dump(current);

    std::vector<State> next;
    next.reserve(current.size());
    for (const ThreadActivitySample& sample : current) {
        State* prior = nullptr;
        for (State& state : previous_) {
            if (state.tid == sample.tid) {
                prior = &state;
                break;
            }
        }
        State state;
        state.tid = sample.tid;
        state.kind = sample.kind;
        state.id = sample.id;
        state.counter = sample.counter;
        if (prior != nullptr && prior->kind == sample.kind && prior->id == sample.id &&
            prior->counter == sample.counter) {
            state.since = prior->since;
            state.unchanged_samples = prior->unchanged_samples + 1;
        } else {
            state.since = now;
            state.unchanged_samples = 0;
        }
        next.push_back(state);
    }
    previous_ = std::move(next);

    if (notes_written_ >= kMaxNotes) return false;

    std::vector<const State*> stuck;
    for (const State& state : previous_) {
        if (state.unchanged_samples >= kStuckThreshold) stuck.push_back(&state);
    }
    if (stuck.empty()) return false;

    std::string value;
    std::size_t shown = 0;
    for (const State* state : stuck) {
        if (shown >= kMaxThreadsShown) break;
        if (shown > 0) value += " | ";
        ThreadActivitySample sample;
        sample.tid = state->tid;
        sample.kind = state->kind;
        sample.id = state->id;
        sample.counter = state->counter;
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now - state->since).count();
        value += std::to_string(state->tid) + "=" + describe_thread_activity(sample) +
                 " x" + std::to_string(state->counter) + " stuck=" + std::to_string(seconds) +
                 " " + thread_kernel_state(state->tid);
        ++shown;
    }
    value += " | recent-jni: " + jni_recent_calls();
    // Once, with the first report: the whole thread census is long and does not change much.
    static bool census_written = false;
    if (!census_written) {
        census_written = true;
        write_threads_census();
    }


    ++notes_written_;
    runtime_report().note_watch_detail(std::to_string(notes_written_), value);
    return true;
}

void HangWatchdog::run_forever() {
    for (;;) {
        std::this_thread::sleep_for(kInterval);
        sample(Clock::now());
    }
}

void start_hang_watchdog() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::thread([]() {
        HangWatchdog watchdog;
        watchdog.run_forever();
    }).detach();
    if (profiling_enabled()) {
        log("profiler on: sampling every guest thread at 200 Hz");
        std::thread([]() {
            unsigned ticks = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                Process::request_profile_samples();
                if (++ticks % 2000 == 0) report_profile();  // every 10 s
            }
        }).detach();
    }
}

}  // namespace zb
