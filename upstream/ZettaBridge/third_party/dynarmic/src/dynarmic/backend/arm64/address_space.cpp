/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#include <mcl/bit_cast.hpp>

#include "dynarmic/backend/arm64/a64_address_space.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace Dynarmic::Backend::Arm64 {

AddressSpace::AddressSpace(size_t code_cache_size)
        : code_cache_size(code_cache_size)
        , mem(code_cache_size)
        , code(mem.ptr(), mem.ptr())
        , fastmem_manager(exception_handler) {
    ASSERT_MSG(code_cache_size <= 128 * 1024 * 1024, "code_cache_size > 128 MiB not currently supported");

    exception_handler.Register(mem, code_cache_size);
    exception_handler.SetFastmemCallback([this](u64 host_pc) {
        return FastmemCallback(host_pc);
    });
}

AddressSpace::~AddressSpace() {
    if (fast_dispatch_table) {
        munmap(fast_dispatch_table, fast_dispatch_table_size * sizeof(FastDispatchEntry));
    }
}

void AddressSpace::AllocateFastDispatchTable() {
    void* table = mmap(nullptr, fast_dispatch_table_size * sizeof(FastDispatchEntry), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // Without it the dispatcher only loses its shortcut.
    fast_dispatch_table = table == MAP_FAILED ? nullptr : static_cast<FastDispatchEntry*>(table);
}

void AddressSpace::ClearFastDispatchTable() {
    if (!fast_dispatch_table) {
        return;
    }
    // Fresh zero pages in place, which also gives back the ones entries were written to.
    void* table = mmap(fast_dispatch_table, fast_dispatch_table_size * sizeof(FastDispatchEntry),
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (table == MAP_FAILED) {
        std::memset(fast_dispatch_table, 0, fast_dispatch_table_size * sizeof(FastDispatchEntry));
    }
}

void AddressSpace::SetFastDispatchEntry(IR::LocationDescriptor descriptor, CodePtr code_ptr) {
    if (fast_dispatch_table) {
        fast_dispatch_table[FastDispatchIndex(descriptor.Value())] = {~descriptor.Value(), code_ptr};
    }
}

CodePtr AddressSpace::Get(IR::LocationDescriptor descriptor) {
    if (const auto iter = block_entries.find(descriptor); iter != block_entries.end()) {
        return iter->second;
    }
    return nullptr;
}

std::optional<IR::LocationDescriptor> AddressSpace::ReverseGetLocation(CodePtr host_pc) {
    if (auto iter = block_infos.upper_bound(host_pc); iter != block_infos.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->second.location;
    }
    return std::nullopt;
}

CodePtr AddressSpace::ReverseGetEntryPoint(CodePtr host_pc) {
    if (auto iter = block_infos.upper_bound(host_pc); iter != block_infos.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->first;
    }
    return nullptr;
}

CodePtr AddressSpace::GetOrEmit(IR::LocationDescriptor descriptor) {
    if (CodePtr block_entry = Get(descriptor)) {
        return block_entry;
    }

    const auto start = std::chrono::steady_clock::now();
    IR::Block ir_block = GenerateIR(descriptor);
    const EmittedBlockInfo block_info = Emit(std::move(ir_block));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    translation_counters.blocks.fetch_add(1, std::memory_order_relaxed);
    translation_counters.nanoseconds.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()), std::memory_order_relaxed);
    translation_counters.code_used.store(static_cast<size_t>(code.offset()), std::memory_order_relaxed);
    return block_info.entry_point;
}

void AddressSpace::InvalidateBasicBlocks(const tsl::robin_set<IR::LocationDescriptor>& descriptors) {
    UnprotectCodeMemory();

    for (const auto& descriptor : descriptors) {
        if (fast_dispatch_table) {
            FastDispatchEntry& entry = fast_dispatch_table[FastDispatchIndex(descriptor.Value())];
            if (entry.inverted_descriptor == ~descriptor.Value()) {
                entry = {};
            }
        }

        const auto iter = block_entries.find(descriptor);
        if (iter == block_entries.end()) {
            continue;
        }

        // Unlink before removal because InvalidateBasicBlocks can be called within a fastmem callback,
        // and the currently executing block may have references to itself which need to be unlinked.
        RelinkForDescriptor(descriptor, nullptr);

        block_entries.erase(iter);
    }

    ProtectCodeMemory();
}

void AddressSpace::ClearCache() {
    ClearFastDispatchTable();
    block_entries.clear();
    block_infos.clear();
    block_references.clear();
    code.set_offset(prelude_info.end_of_prelude);
    translation_counters.cache_flushes.fetch_add(1, std::memory_order_relaxed);
    translation_counters.code_used.store(static_cast<size_t>(code.offset()), std::memory_order_relaxed);
}

void AddressSpace::ReleaseCache() {
    const auto used_end = reinterpret_cast<std::uintptr_t>(mem.ptr()) + static_cast<std::uintptr_t>(code.offset());
    ClearCache();
    // clear() keeps the hash maps' buckets; fresh maps do not.
    block_entries = {};
    block_infos = {};
    block_references = {};

    const auto page = static_cast<std::uintptr_t>(getpagesize());
    const auto first_free = (reinterpret_cast<std::uintptr_t>(mem.ptr()) + static_cast<std::uintptr_t>(prelude_info.end_of_prelude) + page - 1) & ~(page - 1);
    if (used_end > first_free) {
        madvise(reinterpret_cast<void*>(first_free), used_end - first_free, MADV_DONTNEED);
    }
}

void AddressSpace::DumpDisassembly() const {
    for (u32* ptr = mem.ptr(); ptr < code.xptr<u32*>(); ptr++) {
        std::printf("%s", Common::DisassembleAArch64(*ptr, mcl::bit_cast<u64>(ptr)).c_str());
    }
}

size_t AddressSpace::GetRemainingSize() {
    return code_cache_size - static_cast<size_t>(code.offset());
}

EmittedBlockInfo AddressSpace::Emit(IR::Block block) {
    if (GetRemainingSize() < 1024 * 1024) {
        ClearCache();
    }

    UnprotectCodeMemory();

    EmittedBlockInfo block_info = EmitArm64(code, std::move(block), GetEmitConfig(), fastmem_manager);

    ASSERT(block_entries.insert({block.Location(), block_info.entry_point}).second);
    ASSERT(block_infos.emplace(block_info.entry_point, MakeBlockInfo(block.Location(), block_info)).second);

    Link(block_info);
    RelinkForDescriptor(block.Location(), block_info.entry_point);

    mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);
    ProtectCodeMemory();

    RegisterNewBasicBlock(block, block_info);

    return block_info;
}

void AddressSpace::Link(EmittedBlockInfo& block_info) {
    using namespace oaknut;
    using namespace oaknut::util;

    for (auto [ptr_offset, target] : block_info.relocations) {
        CodeGenerator c{mem.ptr(), mem.ptr()};
        c.set_xptr(reinterpret_cast<u32*>(block_info.entry_point + ptr_offset));

        switch (target) {
        case LinkTarget::ReturnToDispatcher:
            c.B(prelude_info.return_to_dispatcher);
            break;
        case LinkTarget::ReturnFromRunCode:
            c.B(prelude_info.return_from_run_code);
            break;
        case LinkTarget::ReadMemory8:
            c.BL(prelude_info.read_memory_8);
            break;
        case LinkTarget::ReadMemory16:
            c.BL(prelude_info.read_memory_16);
            break;
        case LinkTarget::ReadMemory32:
            c.BL(prelude_info.read_memory_32);
            break;
        case LinkTarget::ReadMemory64:
            c.BL(prelude_info.read_memory_64);
            break;
        case LinkTarget::ReadMemory128:
            c.BL(prelude_info.read_memory_128);
            break;
        case LinkTarget::WrappedReadMemory8:
            c.BL(prelude_info.wrapped_read_memory_8);
            break;
        case LinkTarget::WrappedReadMemory16:
            c.BL(prelude_info.wrapped_read_memory_16);
            break;
        case LinkTarget::WrappedReadMemory32:
            c.BL(prelude_info.wrapped_read_memory_32);
            break;
        case LinkTarget::WrappedReadMemory64:
            c.BL(prelude_info.wrapped_read_memory_64);
            break;
        case LinkTarget::WrappedReadMemory128:
            c.BL(prelude_info.wrapped_read_memory_128);
            break;
        case LinkTarget::ExclusiveReadMemory8:
            c.BL(prelude_info.exclusive_read_memory_8);
            break;
        case LinkTarget::ExclusiveReadMemory16:
            c.BL(prelude_info.exclusive_read_memory_16);
            break;
        case LinkTarget::ExclusiveReadMemory32:
            c.BL(prelude_info.exclusive_read_memory_32);
            break;
        case LinkTarget::ExclusiveReadMemory64:
            c.BL(prelude_info.exclusive_read_memory_64);
            break;
        case LinkTarget::ExclusiveReadMemory128:
            c.BL(prelude_info.exclusive_read_memory_128);
            break;
        case LinkTarget::WriteMemory8:
            c.BL(prelude_info.write_memory_8);
            break;
        case LinkTarget::WriteMemory16:
            c.BL(prelude_info.write_memory_16);
            break;
        case LinkTarget::WriteMemory32:
            c.BL(prelude_info.write_memory_32);
            break;
        case LinkTarget::WriteMemory64:
            c.BL(prelude_info.write_memory_64);
            break;
        case LinkTarget::WriteMemory128:
            c.BL(prelude_info.write_memory_128);
            break;
        case LinkTarget::WrappedWriteMemory8:
            c.BL(prelude_info.wrapped_write_memory_8);
            break;
        case LinkTarget::WrappedWriteMemory16:
            c.BL(prelude_info.wrapped_write_memory_16);
            break;
        case LinkTarget::WrappedWriteMemory32:
            c.BL(prelude_info.wrapped_write_memory_32);
            break;
        case LinkTarget::WrappedWriteMemory64:
            c.BL(prelude_info.wrapped_write_memory_64);
            break;
        case LinkTarget::WrappedWriteMemory128:
            c.BL(prelude_info.wrapped_write_memory_128);
            break;
        case LinkTarget::ExclusiveWriteMemory8:
            c.BL(prelude_info.exclusive_write_memory_8);
            break;
        case LinkTarget::ExclusiveWriteMemory16:
            c.BL(prelude_info.exclusive_write_memory_16);
            break;
        case LinkTarget::ExclusiveWriteMemory32:
            c.BL(prelude_info.exclusive_write_memory_32);
            break;
        case LinkTarget::ExclusiveWriteMemory64:
            c.BL(prelude_info.exclusive_write_memory_64);
            break;
        case LinkTarget::ExclusiveWriteMemory128:
            c.BL(prelude_info.exclusive_write_memory_128);
            break;
        case LinkTarget::CallSVC:
            c.BL(prelude_info.call_svc);
            break;
        case LinkTarget::ExceptionRaised:
            c.BL(prelude_info.exception_raised);
            break;
        case LinkTarget::InstructionSynchronizationBarrierRaised:
            c.BL(prelude_info.isb_raised);
            break;
        case LinkTarget::InstructionCacheOperationRaised:
            c.BL(prelude_info.ic_raised);
            break;
        case LinkTarget::DataCacheOperationRaised:
            c.BL(prelude_info.dc_raised);
            break;
        case LinkTarget::GetCNTPCT:
            c.BL(prelude_info.get_cntpct);
            break;
        case LinkTarget::AddTicks:
            c.BL(prelude_info.add_ticks);
            break;
        case LinkTarget::GetTicksRemaining:
            c.BL(prelude_info.get_ticks_remaining);
            break;
        default:
            ASSERT_FALSE("Invalid relocation target");
        }
    }

    for (const auto& [target_descriptor, list] : block_info.block_relocations) {
        block_references[target_descriptor].push_back(block_info.entry_point);
        const CodePtr target_ptr = Get(target_descriptor);
        for (const BlockRelocation relocation : list) {
            LinkBlockLink(block_info.entry_point, target_ptr, relocation);
        }
    }
}

AddressSpace::BlockInfo AddressSpace::MakeBlockInfo(IR::LocationDescriptor location, const EmittedBlockInfo& emitted) {
    BlockInfo info{location, emitted.size, {}, {}};

    info.block_relocations.reserve([&] {
        size_t count = 0;
        for (const auto& entry : emitted.block_relocations) {
            count += entry.second.size();
        }
        return count;
    }());
    for (const auto& [target, list] : emitted.block_relocations) {
        for (const BlockRelocation relocation : list) {
            info.block_relocations.emplace_back(target, relocation);
        }
    }
    std::sort(info.block_relocations.begin(), info.block_relocations.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    info.fastmem_patches.reserve(emitted.fastmem_patch_info.size());
    for (const auto& [code_offset, patch] : emitted.fastmem_patch_info) {
        ASSERT(std::get<0>(patch.marker) == location);
        info.fastmem_patches.push_back(FastmemPatch{
            .code_offset = static_cast<u32>(code_offset),
            .fallback_offset = static_cast<u32>(mcl::bit_cast<CodePtr>(patch.fc.call_pc) - emitted.entry_point),
            .instruction = std::get<1>(patch.marker),
            .recompile = patch.recompile,
        });
    }
    std::sort(info.fastmem_patches.begin(), info.fastmem_patches.end(),
              [](const FastmemPatch& a, const FastmemPatch& b) { return a.code_offset < b.code_offset; });

    return info;
}

void AddressSpace::LinkBlockLink(const CodePtr entry_point, const CodePtr target_ptr, BlockRelocation relocation) {
    using namespace oaknut;
    using namespace oaknut::util;

    CodeGenerator c{mem.ptr(), mem.ptr()};
    c.set_xptr(reinterpret_cast<u32*>(entry_point + relocation.code_offset));

    switch (relocation.type) {
    case BlockRelocationType::Branch:
        if (target_ptr) {
            c.B((void*)target_ptr);
        } else {
            c.NOP();
        }
        break;
    case BlockRelocationType::MoveToScratch1:
        if (target_ptr) {
            c.ADRL(Xscratch1, (void*)target_ptr);
        } else {
            c.ADRL(Xscratch1, prelude_info.return_to_dispatcher);
        }
        break;
    default:
        ASSERT_FALSE("Invalid BlockRelocationType");
    }
}

void AddressSpace::RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr) {
    const auto references = block_references.find(target_descriptor);
    if (references == block_references.end()) {
        return;
    }
    for (const CodePtr entry_point : references->second) {
        if (auto block_iter = block_infos.find(entry_point); block_iter != block_infos.end()) {
            const BlockInfo& block_info = block_iter->second;

            const auto by_target = [](const auto& entry, IR::LocationDescriptor target) { return entry.first < target; };
            for (auto relocation = std::lower_bound(block_info.block_relocations.begin(), block_info.block_relocations.end(), target_descriptor, by_target);
                 relocation != block_info.block_relocations.end() && relocation->first == target_descriptor;
                 ++relocation) {
                LinkBlockLink(entry_point, target_ptr, relocation->second);
            }

            mem.invalidate(reinterpret_cast<u32*>(entry_point), block_info.size);
        }
    }
}

FakeCall AddressSpace::FastmemCallback(u64 host_pc) {
    {
        const auto host_ptr = mcl::bit_cast<CodePtr>(host_pc);

        auto block_info = block_infos.upper_bound(host_ptr);
        if (block_info == block_infos.begin()) {
            goto fail;
        }
        --block_info;
        const CodePtr entry_point = block_info->first;
        const BlockInfo& info = block_info->second;

        const auto code_offset = static_cast<u32>(host_ptr - entry_point);
        const auto patch_entry = std::lower_bound(info.fastmem_patches.begin(), info.fastmem_patches.end(), code_offset,
                                                  [](const FastmemPatch& patch, u32 offset) { return patch.code_offset < offset; });
        if (patch_entry == info.fastmem_patches.end() || patch_entry->code_offset != code_offset) {
            goto fail;
        }

        const FakeCall fc{
            .call_pc = mcl::bit_cast<u64>(entry_point + patch_entry->fallback_offset),
        };

        if (patch_entry->recompile) {
            fastmem_manager.MarkDoNotFastmem(std::make_tuple(info.location, patch_entry->instruction));
            InvalidateBasicBlocks({info.location});
        }

        return fc;
    }

fail:
    fmt::print("dynarmic: Segfault happened within JITted code at host_pc = {:016x}\n", host_pc);
    fmt::print("Segfault wasn't at a fastmem patch location!\n");
    ASSERT_FALSE("segfault");
}

}  // namespace Dynarmic::Backend::Arm64
