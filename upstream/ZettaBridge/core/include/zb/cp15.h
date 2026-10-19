#pragma once

#include <array>  // dynarmic's coprocessor.h uses std::array without including it
#include <cstdint>
#include <optional>

#include <dynarmic/interface/A32/coprocessor.h>

namespace zb {

// Minimal CP15 for user mode:
//   MRC p15, 0, Rt, c13, c0, 3        -> TPIDRURO (read-only for user mode, set via __ARM_NR_set_tls)
//   MRC/MCR p15, 0, Rt, c13, c0, 2    -> TPIDRURW
//   MCR p15, 0, Rt, c7, c10/c5, 4/5   -> the pre-ARMv7 DSB/DMB/ISB barriers
//   MRRC p15, 1, Rt, Rt2, c14         -> CNTVCT, the generic timer's virtual count
//   MRC p15, 0, Rt, c14, c0, 0        -> CNTFRQ, its frequency
// The generic timer registers are the ones Linux lets 32-bit user code read on ARMv8 hardware
// (OpenSSL's _armv7_tick probe reads CNTVCT from a static constructor; Minecraft PE runs it at
// load time), and they are backed by the host's own counter.
//
// Anything else is a privileged or absent register that would raise SIGILL on hardware. Dynarmic
// cannot raise a guest exception from a coprocessor access (it asserts at JIT time and the whole
// process dies), so those accesses are logged once per encoding and act as no-ops reading 0.
class Cp15 final : public Dynarmic::A32::Coprocessor {
public:
    Cp15(std::uint32_t* tpidruro, std::uint32_t* tpidrurw) : tpidruro_(tpidruro), tpidrurw_(tpidrurw) {}

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRd,
                                                     Dynarmic::A32::CoprocReg CRn, Dynarmic::A32::CoprocReg CRm,
                                                     unsigned opc2) override;
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRn,
                                               Dynarmic::A32::CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, Dynarmic::A32::CoprocReg CRm) override;
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRn,
                                              Dynarmic::A32::CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, Dynarmic::A32::CoprocReg CRm) override;
    std::optional<Callback> CompileLoadWords(bool two, bool long_transfer, Dynarmic::A32::CoprocReg CRd,
                                             std::optional<std::uint8_t> option) override;
    std::optional<Callback> CompileStoreWords(bool two, bool long_transfer, Dynarmic::A32::CoprocReg CRd,
                                              std::optional<std::uint8_t> option) override;

private:
    std::uint32_t* tpidruro_;
    std::uint32_t* tpidrurw_;
};

// Every other coprocessor number (cp14 debug registers, cp0-cp9 and cp12-cp13 that do not exist
// on ARMv7/v8). cp10/cp11 are VFP/ASIMD and never reach a coprocessor object. Same policy as the
// unhandled CP15 accesses: logged once per encoding, no-op, reads 0, instead of a JIT assertion.
class AbsentCoprocessor final : public Dynarmic::A32::Coprocessor {
public:
    explicit AbsentCoprocessor(unsigned number) : number_(number) {}

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRd,
                                                     Dynarmic::A32::CoprocReg CRn, Dynarmic::A32::CoprocReg CRm,
                                                     unsigned opc2) override;
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRn,
                                               Dynarmic::A32::CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, Dynarmic::A32::CoprocReg CRm) override;
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRn,
                                              Dynarmic::A32::CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, Dynarmic::A32::CoprocReg CRm) override;
    std::optional<Callback> CompileLoadWords(bool two, bool long_transfer, Dynarmic::A32::CoprocReg CRd,
                                             std::optional<std::uint8_t> option) override;
    std::optional<Callback> CompileStoreWords(bool two, bool long_transfer, Dynarmic::A32::CoprocReg CRd,
                                              std::optional<std::uint8_t> option) override;

private:
    unsigned number_;
};

}  // namespace zb
