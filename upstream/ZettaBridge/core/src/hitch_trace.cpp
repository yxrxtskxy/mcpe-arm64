// Freeze tracer (debug.zb.hitch): per-thread wait accounting, and the render thread's account of
// every frame that took too long. Diagnostics only; nothing here changes what the guest sees.
#include "zb/hitch_trace.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include "zb/guest_thread.h"
#include "zb/host_jni.h"
#include "zb/log.h"
#include "zb/process.h"
#include "zb/syscalls.h"

namespace zb {

namespace {

constexpr std::int64_t kSummaryIntervalNs = 30'000'000'000;
constexpr std::size_t kDetailSize = 64;
constexpr std::size_t kWaitKinds = 3;
constexpr std::size_t kMaxSamples = 1024;
constexpr std::size_t kHotSamplesShown = 6;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

long long ms(std::int64_t ns) {
    return static_cast<long long>(ns / 1'000'000);
}

double mib(std::size_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

// Frames at least this long are logged; 0 while the tracer is off.
std::int64_t threshold_ns() {
    static const std::int64_t value = [] {
        const char* text = std::getenv("ZB_HITCH");
#ifdef __ANDROID__
        char property[PROP_VALUE_MAX] = {};
        if (text == nullptr && __system_property_get("debug.zb.hitch", property) > 0) text = property;
#endif
        const long milliseconds = text != nullptr ? std::strtol(text, nullptr, 10) : 0;
        return milliseconds > 0 ? static_cast<std::int64_t>(milliseconds) * 1'000'000 : std::int64_t{0};
    }();
    return value;
}

struct WaitTotals {
    std::int64_t ns = 0;
    std::uint32_t count = 0;
    std::int64_t longest_ns = 0;
    std::uint32_t longest_id = 0;
    char longest_detail[kDetailSize] = {};
};

// Waits of the calling thread since its last frame boundary. Only the render thread has frame
// boundaries; on the other threads these only add up.
struct ThreadWaits {
    WaitTotals totals[kWaitKinds];
    unsigned depth = 0;  // HitchScopes open on this thread
    char detail[kDetailSize] = {};  // hitch_note_detail of the open outermost scope
    // Profiler samples since the last frame boundary; the profiler takes 200 a second.
    std::uint32_t samples[kMaxSamples] = {};
    std::size_t sample_count = 0;
};

thread_local ThreadWaits t_waits;

struct SchedStat {
    std::uint64_t run_ns = 0;   // on a CPU
    std::uint64_t wait_ns = 0;  // runnable, waiting for a CPU
};

// The render thread's previous frame boundary and the running summary.
struct FrameState {
    std::int64_t start_ns = 0;
    Dynarmic::A32::Jit::TranslationStats jit{};
    std::uint64_t all_threads_jit_ns = 0;
    rusage usage{};
    SchedStat sched{};
    bool have_sched = false;
    int schedstat_fd = -2;  // -2: not opened yet; -1: unavailable

    std::int64_t summary_start_ns = 0;
    Dynarmic::A32::Jit::TranslationStats summary_jit{};
    std::uint64_t frames = 0;
    std::uint64_t hitches = 0;
    std::int64_t worst_ns = 0;
};

thread_local FrameState t_frame;

bool read_schedstat(FrameState& frame, std::int32_t tid, SchedStat& out) {
    if (frame.schedstat_fd == -2) {
        char path[64];
        std::snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", static_cast<int>(tid));
        frame.schedstat_fd = ::open(path, O_RDONLY | O_CLOEXEC);
    }
    if (frame.schedstat_fd < 0) return false;
    char text[96];
    const ssize_t n = ::pread(frame.schedstat_fd, text, sizeof text - 1, 0);
    if (n <= 0) return false;
    text[n] = '\0';
    unsigned long long run = 0;
    unsigned long long wait = 0;
    if (std::sscanf(text, "%llu %llu", &run, &wait) != 2) return false;
    out = {run, wait};
    return true;
}

std::string describe_longest(HitchWait kind, const WaitTotals& totals) {
    std::string text;
    switch (kind) {
    case HitchWait::Syscall:
        text = syscall_name(totals.longest_id);
        break;
    case HitchWait::HostCall: {
        const auto [library, name] = host_call_name(totals.longest_id);
        if (std::strcmp(name, "?") != 0) {
            text = std::string(library) + " " + name;
        } else if (const char* jni = jni_host_call_name(totals.longest_id); std::strcmp(jni, "?") != 0) {
            text = std::string("JNI ") + jni;
        } else {
            text = "host call " + std::to_string(totals.longest_id);
        }
        break;
    }
    case HitchWait::SpinWait: {
        char lock[32];
        std::snprintf(lock, sizeof lock, "lock 0x%08x", totals.longest_id);
        text = lock;
        break;
    }
    }
    if (totals.longest_detail[0] != '\0') text += std::string(" (") + totals.longest_detail + ")";
    return text;
}

void append(std::string& line, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void append(std::string& line, const char* fmt, ...) {
    char piece[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(piece, sizeof piece, fmt, args);
    va_end(args);
    line += piece;
}

// " | samples 80, hot 31 libminecraftpe.so 0x12d7072, ..." for the profiler samples of the frame.
void append_samples(std::string& line, const ThreadWaits& waits) {
    if (waits.sample_count == 0) return;
    std::vector<std::uint32_t> pcs(waits.samples, waits.samples + waits.sample_count);
    std::sort(pcs.begin(), pcs.end());
    std::vector<std::pair<std::size_t, std::uint32_t>> counts;  // samples, pc
    for (std::size_t i = 0; i < pcs.size();) {
        std::size_t j = i;
        while (j < pcs.size() && pcs[j] == pcs[i]) ++j;
        counts.emplace_back(j - i, pcs[i]);
        i = j;
    }
    std::sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    append(line, " | samples %zu, hot", waits.sample_count);
    for (std::size_t i = 0; i < counts.size() && i < kHotSamplesShown; ++i) {
        std::string where = Process::describe_guest_address(counts[i].second);
        const std::size_t slash = where.rfind('/');
        if (slash != std::string::npos) where.erase(0, slash + 1);
        append(line, "%s %zu %s", i == 0 ? "" : ",", counts[i].first, where.c_str());
    }
}

void append_waits(std::string& line, const char* label, HitchWait kind, const WaitTotals& totals) {
    append(line, " | %s %lld ms in %u", label, ms(totals.ns), totals.count);
    if (totals.count != 0) {
        append(line, ", longest %lld ms %s", ms(totals.longest_ns), describe_longest(kind, totals).c_str());
    }
}

void log_hitch(const FrameState& frame, const ThreadWaits& waits, std::int32_t tid, std::int64_t frame_ns,
               const Dynarmic::A32::Jit::TranslationStats& jit, std::uint64_t all_threads_jit_ns, const rusage& usage,
               const SchedStat& sched, bool have_sched) {
    const std::int64_t jit_ns = static_cast<std::int64_t>(jit.nanoseconds - frame.jit.nanoseconds);
    // Threads that exited since the last frame took their translation time with them.
    const std::int64_t all_jit_ns =
        all_threads_jit_ns > frame.all_threads_jit_ns ? static_cast<std::int64_t>(all_threads_jit_ns - frame.all_threads_jit_ns) : 0;
    const WaitTotals& syscalls = waits.totals[static_cast<std::size_t>(HitchWait::Syscall)];
    const WaitTotals& host_calls = waits.totals[static_cast<std::size_t>(HitchWait::HostCall)];
    const WaitTotals& spins = waits.totals[static_cast<std::size_t>(HitchWait::SpinWait)];
    // Guest code that a JNI call into Java ran again can be translated inside that call, and is
    // then in both; the rest stays at 0 rather than below.
    const std::int64_t rest = std::max<std::int64_t>(0, frame_ns - jit_ns - syscalls.ns - host_calls.ns - spins.ns);

    std::string line;
    append(line, "hitch: %lld ms frame (tid %d)", ms(frame_ns), static_cast<int>(tid));
    append(line, " | translating %lld ms, %llu blocks", ms(jit_ns),
           static_cast<unsigned long long>(jit.blocks - frame.jit.blocks));
    if (jit.cache_flushes != frame.jit.cache_flushes) {
        append(line, ", %llu FULL CODE CACHE FLUSH", static_cast<unsigned long long>(jit.cache_flushes - frame.jit.cache_flushes));
    }
    append(line, ", cache %.1f of %.0f MiB, all threads %lld ms", mib(jit.code_used), mib(jit.code_size), ms(all_jit_ns));
    append_waits(line, "syscalls", HitchWait::Syscall, syscalls);
    append_waits(line, "host calls", HitchWait::HostCall, host_calls);
    append_waits(line, "spinlocks", HitchWait::SpinWait, spins);
    append(line, " | rest (guest code) %lld ms", ms(rest));
    if (have_sched && frame.have_sched) {
        append(line, " | on cpu %lld ms, waiting for cpu %lld ms",
               ms(static_cast<std::int64_t>(sched.run_ns - frame.sched.run_ns)),
               ms(static_cast<std::int64_t>(sched.wait_ns - frame.sched.wait_ns)));
    }
    append(line, " | page faults %ld (%ld major), switches %ld (%ld involuntary)",
           usage.ru_minflt - frame.usage.ru_minflt + usage.ru_majflt - frame.usage.ru_majflt,
           usage.ru_majflt - frame.usage.ru_majflt, usage.ru_nvcsw - frame.usage.ru_nvcsw + usage.ru_nivcsw - frame.usage.ru_nivcsw,
           usage.ru_nivcsw - frame.usage.ru_nivcsw);
    append_samples(line, waits);
    log("%s", line.c_str());
}

void log_summary(const FrameState& frame, std::int64_t now, const Dynarmic::A32::Jit::TranslationStats& jit) {
    log("hitch summary: %llu frames in %lld s, %llu at %lld ms or more, worst %lld ms | render thread translated "
        "%llu blocks in %lld ms, %llu code cache flushes, cache %.1f of %.0f MiB",
        static_cast<unsigned long long>(frame.frames), static_cast<long long>((now - frame.summary_start_ns) / 1'000'000'000),
        static_cast<unsigned long long>(frame.hitches), ms(threshold_ns()), ms(frame.worst_ns),
        static_cast<unsigned long long>(jit.blocks - frame.summary_jit.blocks),
        ms(static_cast<std::int64_t>(jit.nanoseconds - frame.summary_jit.nanoseconds)),
        static_cast<unsigned long long>(jit.cache_flushes - frame.summary_jit.cache_flushes), mib(jit.code_used),
        mib(jit.code_size));
}

}  // namespace

bool hitch_trace_enabled() {
    return threshold_ns() != 0;
}

void HitchScope::begin(HitchWait kind, std::uint32_t id) {
    ThreadWaits& waits = t_waits;
    if (waits.depth++ != 0) {
        start_ns_ = -1;
        return;
    }
    kind_ = kind;
    id_ = id;
    waits.detail[0] = '\0';
    start_ns_ = now_ns();
}

void HitchScope::end() {
    ThreadWaits& waits = t_waits;
    --waits.depth;
    if (start_ns_ < 0) return;
    const std::int64_t elapsed = now_ns() - start_ns_;
    WaitTotals& totals = waits.totals[static_cast<std::size_t>(kind_)];
    totals.ns += elapsed;
    ++totals.count;
    if (elapsed > totals.longest_ns) {
        totals.longest_ns = elapsed;
        totals.longest_id = id_;
        std::memcpy(totals.longest_detail, waits.detail, kDetailSize);
    }
}

void hitch_note_detail(const char* text) {
    ThreadWaits& waits = t_waits;
    if (waits.depth != 1 || text == nullptr) return;
    std::snprintf(waits.detail, kDetailSize, "%s", text);
}

void hitch_note_sample(std::uint32_t guest_pc) {
    if (!hitch_trace_enabled()) return;
    ThreadWaits& waits = t_waits;
    if (waits.sample_count < kMaxSamples) waits.samples[waits.sample_count++] = guest_pc;
}

void hitch_frame_boundary(GuestThread& thread) {
    if (!hitch_trace_enabled()) return;
    FrameState& frame = t_frame;
    ThreadWaits& waits = t_waits;
    const std::int64_t now = now_ns();
    const Dynarmic::A32::Jit::TranslationStats jit = thread.translation_stats();
    const std::uint64_t all_threads_jit_ns = Process::translation_ns_all_threads();
    rusage usage{};
    ::getrusage(RUSAGE_THREAD, &usage);
    SchedStat sched;
    const bool have_sched = read_schedstat(frame, thread.tid, sched);

    if (frame.start_ns == 0) {
        log("hitch tracer on: frames of the render thread (tid %d) that take %lld ms or more are logged",
            static_cast<int>(thread.tid), ms(threshold_ns()));
        frame.summary_start_ns = now;
        frame.summary_jit = jit;
    } else {
        const std::int64_t frame_ns = now - frame.start_ns;
        ++frame.frames;
        frame.worst_ns = std::max(frame.worst_ns, frame_ns);
        if (frame_ns >= threshold_ns()) {
            ++frame.hitches;
            log_hitch(frame, waits, thread.tid, frame_ns, jit, all_threads_jit_ns, usage, sched, have_sched);
        }
        if (now - frame.summary_start_ns >= kSummaryIntervalNs) {
            log_summary(frame, now, jit);
            frame.summary_start_ns = now;
            frame.summary_jit = jit;
            frame.frames = 0;
            frame.hitches = 0;
            frame.worst_ns = 0;
        }
    }

    // The next frame starts here. The eglSwapBuffers that called us is still open and counts
    // toward it when it returns.
    frame.start_ns = now;
    frame.jit = jit;
    frame.all_threads_jit_ns = all_threads_jit_ns;
    frame.usage = usage;
    frame.sched = sched;
    frame.have_sched = have_sched;
    for (WaitTotals& totals : waits.totals) totals = WaitTotals{};
    waits.sample_count = 0;
}

}  // namespace zb
