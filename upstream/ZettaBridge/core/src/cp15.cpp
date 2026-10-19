#include <atomic>
#include <ctime>
#include <mutex>
#include <set>
#include <string>

#include "zb/cp15.h"
#include "zb/log.h"

namespace zb {

using Dynarmic::A32::CoprocReg;

namespace {

// The pre-ARMv7 way to write a memory barrier: a CP15 write rather than the DMB/DSB/ISB
// instructions. Mono still emits it (libmono.so does, in its own memory barrier), and old
// compilers emitted it for anything targeting ARMv6. The other CP15 operations are privileged
// and would fault on real hardware, so only these three are served.
bool is_barrier(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (two || opc1 != 0) return false;
    if (CRn != CoprocReg::C7) return false;
    if (CRm == CoprocReg::C10 && (opc2 == 4 || opc2 == 5)) return true;  // DSB, DMB
    return CRm == CoprocReg::C5 && opc2 == 4;                            // ISB
}

std::uint64_t run_barrier(void*, std::uint32_t, std::uint32_t) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return 0;
}

bool is_thread_id_reg(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm) {
    return !two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0;
}

// The generic timer. On an AArch64 host these are the same registers 32-bit code reads natively
// (CNTVCT_EL0/CNTFRQ_EL0 are EL0-readable on Linux); elsewhere (x86_64 test builds) a monotonic
// nanosecond count at a nominal 1 GHz stands in.
std::uint64_t read_virtual_count(void*, std::uint32_t, std::uint32_t) {
#if defined(__aarch64__)
    std::uint64_t value;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) : : "memory");
    return value;
#else
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(now.tv_nsec);
#endif
}

std::uint64_t read_counter_frequency(void*, std::uint32_t, std::uint32_t) {
#if defined(__aarch64__)
    std::uint64_t value;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return static_cast<std::uint32_t>(value);
#else
    return 1000000000u;
#endif
}

bool is_virtual_count(bool two, unsigned opc, CoprocReg CRm) {
    return !two && opc == 1 && CRm == CoprocReg::C14;
}

bool is_counter_frequency(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    return !two && opc1 == 0 && CRn == CoprocReg::C14 && CRm == CoprocReg::C0 && opc2 == 0;
}

std::uint64_t ignore_access(void*, std::uint32_t, std::uint32_t) {
    return 0;
}

// One log line per distinct encoding, at translation time, so a guest polling an unhandled
// register in a loop does not flood the log.
void note_unhandled(unsigned coproc, const char* kind, bool two, unsigned opc1, unsigned CRn, unsigned CRm,
                    unsigned opc2) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    const std::string key = std::to_string(coproc) + kind + (two ? "2" : "") + "/" + std::to_string(opc1) + "/" +
                            std::to_string(CRn) + "/" + std::to_string(CRm) + "/" + std::to_string(opc2);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!seen.insert(key).second) return;
    }
    log("unhandled coprocessor access p%u %s%s opc1=%u CRn=c%u CRm=c%u opc2=%u: no-op, reads as 0", coproc, kind,
        two ? "2" : "", opc1, CRn, CRm, opc2);
}

unsigned reg_number(CoprocReg reg) {
    return static_cast<unsigned>(reg);
}

constexpr Dynarmic::A32::Coprocessor::Callback kIgnore{&ignore_access, std::nullopt};

}  // namespace

std::optional<Cp15::Callback> Cp15::CompileInternalOperation(bool two, unsigned opc1, CoprocReg CRd, CoprocReg CRn,
                                                             CoprocReg CRm, unsigned opc2) {
    (void)CRd;
    note_unhandled(15, "CDP", two, opc1, reg_number(CRn), reg_number(CRm), opc2);
    return kIgnore;
}

Cp15::CallbackOrAccessOneWord Cp15::CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (is_thread_id_reg(two, opc1, CRn, CRm) && opc2 == 2) return tpidrurw_;
    if (is_barrier(two, opc1, CRn, CRm, opc2)) return Callback{&run_barrier, std::nullopt};
    note_unhandled(15, "MCR", two, opc1, reg_number(CRn), reg_number(CRm), opc2);
    return kIgnore;
}

Cp15::CallbackOrAccessTwoWords Cp15::CompileSendTwoWords(bool two, unsigned opc, CoprocReg CRm) {
    note_unhandled(15, "MCRR", two, opc, 0, reg_number(CRm), 0);
    return kIgnore;
}

Cp15::CallbackOrAccessOneWord Cp15::CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (is_thread_id_reg(two, opc1, CRn, CRm)) {
        if (opc2 == 3) return tpidruro_;
        if (opc2 == 2) return tpidrurw_;
    }
    if (is_counter_frequency(two, opc1, CRn, CRm, opc2)) return Callback{&read_counter_frequency, std::nullopt};
    note_unhandled(15, "MRC", two, opc1, reg_number(CRn), reg_number(CRm), opc2);
    return kIgnore;
}

Cp15::CallbackOrAccessTwoWords Cp15::CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) {
    if (is_virtual_count(two, opc, CRm)) return Callback{&read_virtual_count, std::nullopt};
    note_unhandled(15, "MRRC", two, opc, 0, reg_number(CRm), 0);
    return kIgnore;
}

std::optional<Cp15::Callback> Cp15::CompileLoadWords(bool two, bool, CoprocReg CRd, std::optional<std::uint8_t>) {
    note_unhandled(15, "LDC", two, 0, reg_number(CRd), 0, 0);
    return kIgnore;
}

std::optional<Cp15::Callback> Cp15::CompileStoreWords(bool two, bool, CoprocReg CRd, std::optional<std::uint8_t>) {
    note_unhandled(15, "STC", two, 0, reg_number(CRd), 0, 0);
    return kIgnore;
}

std::optional<AbsentCoprocessor::Callback> AbsentCoprocessor::CompileInternalOperation(
    bool two, unsigned opc1, CoprocReg, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    note_unhandled(number_, "CDP", two, opc1, reg_number(CRn), reg_number(CRm), opc2);
    return kIgnore;
}

AbsentCoprocessor::CallbackOrAccessOneWord AbsentCoprocessor::CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn,
                                                                                 CoprocReg CRm, unsigned opc2) {
    note_unhandled(number_, "MCR", two, opc1, reg_number(CRn), reg_number(CRm), opc2);
    return kIgnore;
}

AbsentCoprocessor::CallbackOrAccessTwoWords AbsentCoprocessor::CompileSendTwoWords(bool two, unsigned opc, CoprocReg CRm) {
    note_unhandled(number_, "MCRR", two, opc, 0, reg_number(CRm), 0);
    return kIgnore;
}

AbsentCoprocessor::CallbackOrAccessOneWord AbsentCoprocessor::CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn,
                                                                                CoprocReg CRm, unsigned opc2) {
    note_unhandled(number_, "MRC", two, opc1, reg_number(CRn), reg_number(CRm), opc2);
    return kIgnore;
}

AbsentCoprocessor::CallbackOrAccessTwoWords AbsentCoprocessor::CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) {
    note_unhandled(number_, "MRRC", two, opc, 0, reg_number(CRm), 0);
    return kIgnore;
}

std::optional<AbsentCoprocessor::Callback> AbsentCoprocessor::CompileLoadWords(bool two, bool, CoprocReg CRd,
                                                                               std::optional<std::uint8_t>) {
    note_unhandled(number_, "LDC", two, 0, reg_number(CRd), 0, 0);
    return kIgnore;
}

std::optional<AbsentCoprocessor::Callback> AbsentCoprocessor::CompileStoreWords(bool two, bool, CoprocReg CRd,
                                                                                std::optional<std::uint8_t>) {
    note_unhandled(number_, "STC", two, 0, reg_number(CRd), 0, 0);
    return kIgnore;
}

}  // namespace zb
