/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"

namespace Dynarmic {
namespace A32 {

class Jit final {
public:
    explicit Jit(UserConfig conf);
    ~Jit();

    /**
     * Runs the emulated CPU.
     * Cannot be recursively called.
     */
    HaltReason Run();

    /**
     * Steps the emulated CPU.
     * Cannot be recursively called.
     */
    HaltReason Step();

    /**
     * Clears the code cache of all compiled code.
     * Can be called at any time. Halts execution if called within a callback.
     */
    void ClearCache();

    /**
     * Clears the code cache at once and gives back the memory behind it: the code pages and the
     * bookkeeping of every block. Unlike ClearCache it does not wait for the next Run, so it must
     * not be called while Run or Step executes, from any thread.
     */
    void ReleaseCache();

    /**
     * Invalidate the code cache at a range of addresses.
     * @param start_address The starting address of the range to invalidate.
     * @param length The length (in bytes) of the range to invalidate.
     */
    void InvalidateCacheRange(std::uint32_t start_address, std::size_t length);

    /**
     * Reset CPU state to state at startup. Does not clear code cache.
     * Cannot be called from a callback.
     */
    void Reset();

    /**
     * Stops execution in Jit::Run.
     */
    void HaltExecution(HaltReason hr = HaltReason::UserDefined1);

    /**
     * Clears a halt reason from flags.
     * Warning: Only use this if you're sure this won't introduce races.
     */
    void ClearHalt(HaltReason hr = HaltReason::UserDefined1);

    /// View and modify registers.
    std::array<std::uint32_t, 16>& Regs();
    const std::array<std::uint32_t, 16>& Regs() const;
    std::array<std::uint32_t, 64>& ExtRegs();
    const std::array<std::uint32_t, 64>& ExtRegs() const;

    /// View and modify CPSR.
    std::uint32_t Cpsr() const;
    void SetCpsr(std::uint32_t value);

    /// View and modify FPSCR.
    std::uint32_t Fpscr() const;
    void SetFpscr(std::uint32_t value);

    /// Clears exclusive state for this core.
    void ClearExclusiveState();

    /// Translation activity since this Jit was created, for diagnostics.
    struct TranslationStats {
        std::uint64_t blocks = 0;         ///< Basic blocks translated.
        std::uint64_t nanoseconds = 0;    ///< Time spent translating and emitting them.
        std::uint64_t cache_flushes = 0;  ///< Times the whole code cache was dropped.
        std::size_t code_used = 0;        ///< Bytes of the code cache in use.
        std::size_t code_size = 0;        ///< Size of the code cache.
    };

    /**
     * Returns the translation activity of this Jit.
     * Can be called from any thread, at any time.
     */
    TranslationStats GetTranslationStats() const;

    /**
     * Returns true if Jit::Run was called but hasn't returned yet.
     * i.e.: We're in a callback.
     */
    bool IsExecuting() const {
        return is_executing;
    }

    /// Debugging: Dump a disassembly all compiled code to the console.
    void DumpDisassembly() const;

    /**
     * Disassemble the instructions following the current pc and return
     * the resulting instructions as a vector of their string representations.
     */
    std::vector<std::string> Disassemble() const;

private:
    bool is_executing = false;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace A32
}  // namespace Dynarmic
