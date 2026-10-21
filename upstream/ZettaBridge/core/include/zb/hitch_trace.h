#pragma once

#include <cstdint>

namespace zb {

class GuestThread;

// Freeze tracer. With the system property debug.zb.hitch (or $ZB_HITCH) set to a number of
// milliseconds, every frame of the render thread (one eglSwapBuffers to the next) that takes at
// least that long is logged with where its time went: translating guest code, waiting in
// syscalls, in host calls (GL, EGL, JNI into Java...) and in rewritten spinlocks, each with its
// longest single wait. What is left was spent running translated code or not running at all, and
// the line has the scheduler's numbers for the thread too. Every 30 s a summary line follows.
// Off, it costs one test per syscall and host call.
bool hitch_trace_enabled();

enum class HitchWait : std::uint8_t { Syscall, HostCall, SpinWait };

// Times one syscall, host call or spinlock wait of the calling thread into its current frame.
// Only the outermost one counts: a JNI call into Java that runs guest code again already holds
// the syscalls that code makes.
class HitchScope {
public:
    HitchScope(HitchWait kind, std::uint32_t id) {
        if (hitch_trace_enabled()) begin(kind, id);
    }
    ~HitchScope() {
        if (start_ns_ != 0) end();
    }
    HitchScope(const HitchScope&) = delete;
    HitchScope& operator=(const HitchScope&) = delete;

private:
    void begin(HitchWait kind, std::uint32_t id);
    void end();

    std::int64_t start_ns_ = 0;  // 0: not tracing; negative: nested in another scope
    HitchWait kind_ = HitchWait::Syscall;
    std::uint32_t id_ = 0;
};

// What the calling thread's current host call does, in a few words (the Java method a JNI call
// runs). Kept with the call if it turns out to be the longest of its frame.
void hitch_note_detail(const char* text);

// A profiler sample (debug.zb.profile) of the calling thread. The render thread keeps those of its
// current frame, and a frame that is logged names its hottest guest addresses.
void hitch_note_sample(std::uint32_t guest_pc);

// The render thread, at the start of each eglSwapBuffers: ends one frame and starts the next.
void hitch_frame_boundary(GuestThread& thread);

}  // namespace zb
