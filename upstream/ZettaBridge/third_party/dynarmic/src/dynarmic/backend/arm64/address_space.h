/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <atomic>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <mcl/stdint.hpp>
#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

class AddressSpace {
public:
    explicit AddressSpace(size_t code_cache_size);
    virtual ~AddressSpace();

    virtual IR::Block GenerateIR(IR::LocationDescriptor) const = 0;

    CodePtr Get(IR::LocationDescriptor descriptor);

    // Returns "most likely" LocationDescriptor assocated with the emitted code at that location
    std::optional<IR::LocationDescriptor> ReverseGetLocation(CodePtr host_pc);

    // Returns "most likely" entry_point associated with the emitted code at that location
    CodePtr ReverseGetEntryPoint(CodePtr host_pc);

    CodePtr GetOrEmit(IR::LocationDescriptor descriptor);

    void InvalidateBasicBlocks(const tsl::robin_set<IR::LocationDescriptor>& descriptors);

    void ClearCache();
    // ClearCache, and the code pages and the bookkeeping go back to the system.
    void ReleaseCache();

    void DumpDisassembly() const;

    // Diagnostics: written by the thread that runs this address space, readable from any thread.
    struct TranslationCounters {
        std::atomic<u64> blocks{0};
        std::atomic<u64> nanoseconds{0};
        std::atomic<u64> cache_flushes{0};
        std::atomic<size_t> code_used{0};
    };
    const TranslationCounters& GetTranslationCounters() const { return translation_counters; }
    size_t GetCodeCacheSize() const { return code_cache_size; }

protected:
    virtual EmitConfig GetEmitConfig() = 0;
    virtual void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) = 0;

    void ProtectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.protect();
#endif
    }

    void UnprotectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.unprotect();
#endif
    }

    size_t GetRemainingSize();
    EmittedBlockInfo Emit(IR::Block ir_block);
    void Link(EmittedBlockInfo& block);
    void LinkBlockLink(const CodePtr entry_point, const CodePtr target_ptr, BlockRelocation relocation);
    void RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr);

    FakeCall FastmemCallback(u64 host_pc);

    // OptimizationFlag::FastDispatch: a direct-mapped cache in front of block_entries, which the
    // A32 dispatcher probes in a few instructions before it calls GetOrEmit. An entry holds the
    // complement of its location descriptor, so the zero pages of a fresh table are all empty
    // and only the pages entries are written to are ever committed.
    struct FastDispatchEntry {
        u64 inverted_descriptor;
        CodePtr code_ptr;
    };
    static_assert(sizeof(FastDispatchEntry) == 16);
    static constexpr size_t fast_dispatch_table_size = size_t{1} << 16;
    static constexpr u64 FastDispatchIndex(u64 descriptor) {
        return ((descriptor >> 1) ^ (descriptor >> 17) ^ (descriptor >> 32)) & (fast_dispatch_table_size - 1);
    }
    void AllocateFastDispatchTable();
    void ClearFastDispatchTable();
    void SetFastDispatchEntry(IR::LocationDescriptor descriptor, CodePtr code_ptr);
    FastDispatchEntry* fast_dispatch_table = nullptr;  // null without the optimization

    const size_t code_cache_size;
    oaknut::CodeBlock mem;
    oaknut::CodeGenerator code;
    TranslationCounters translation_counters;

    // What an emitted block keeps once it is linked: its links to other blocks, for relinking, and
    // its fastmem accesses, for a fault in one. One per block translated since the last cache
    // clear, so it is kept small: as an EmittedBlockInfo in a hash map, with a hash map of its own
    // for each of these, it took about 2 KiB of host memory per block.
    struct FastmemPatch {
        u32 code_offset;      // of the access, from the entry point
        u32 fallback_offset;  // where the fake call goes, from the entry point
        unsigned instruction;  // the access's half of its DoNotFastmemMarker
        bool recompile;
    };
    struct BlockInfo {
        IR::LocationDescriptor location;
        size_t size;
        std::vector<std::pair<IR::LocationDescriptor, BlockRelocation>> block_relocations;  // by target
        std::vector<FastmemPatch> fastmem_patches;                                           // by code_offset
    };
    static BlockInfo MakeBlockInfo(IR::LocationDescriptor location, const EmittedBlockInfo& emitted);

    // A IR::LocationDescriptor will have one current CodePtr.
    // However, there can be multiple other CodePtrs which are older, previously invalidated blocks.
    tsl::robin_map<IR::LocationDescriptor, CodePtr> block_entries;
    // Every block emitted since the last cache clear, by entry point.
    std::map<CodePtr, BlockInfo> block_infos;
    // The blocks that link to a location. A vector each: a hot function is linked to from thousands
    // of blocks, and an unordered_multimap walks all of a key's entries on every insert.
    tsl::robin_map<IR::LocationDescriptor, std::vector<CodePtr>> block_references;

    ExceptionHandler exception_handler;
    FastmemManager fastmem_manager;

    struct PreludeInfo {
        std::ptrdiff_t end_of_prelude;

        using RunCodeFuncType = HaltReason (*)(CodePtr entry_point, void* jit_state, volatile u32* halt_reason);
        RunCodeFuncType run_code;
        RunCodeFuncType step_code;
        void* return_to_dispatcher;
        void* return_from_run_code;

        void* read_memory_8;
        void* read_memory_16;
        void* read_memory_32;
        void* read_memory_64;
        void* read_memory_128;
        void* wrapped_read_memory_8;
        void* wrapped_read_memory_16;
        void* wrapped_read_memory_32;
        void* wrapped_read_memory_64;
        void* wrapped_read_memory_128;
        void* exclusive_read_memory_8;
        void* exclusive_read_memory_16;
        void* exclusive_read_memory_32;
        void* exclusive_read_memory_64;
        void* exclusive_read_memory_128;
        void* write_memory_8;
        void* write_memory_16;
        void* write_memory_32;
        void* write_memory_64;
        void* write_memory_128;
        void* wrapped_write_memory_8;
        void* wrapped_write_memory_16;
        void* wrapped_write_memory_32;
        void* wrapped_write_memory_64;
        void* wrapped_write_memory_128;
        void* exclusive_write_memory_8;
        void* exclusive_write_memory_16;
        void* exclusive_write_memory_32;
        void* exclusive_write_memory_64;
        void* exclusive_write_memory_128;

        void* call_svc;
        void* exception_raised;
        void* dc_raised;
        void* ic_raised;
        void* isb_raised;

        void* get_cntpct;
        void* add_ticks;
        void* get_ticks_remaining;
    } prelude_info;
};

}  // namespace Dynarmic::Backend::Arm64
