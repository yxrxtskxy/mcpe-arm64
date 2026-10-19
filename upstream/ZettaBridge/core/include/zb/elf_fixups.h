#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zb {

// Dynamic tag written by tools/fix_guest_lib.py in place of DT_TEXTREL. The guest linker
// ignores it (it is in the OS-specific range); zbrun keeps the library's code pages writable
// so the linker's text relocations succeed.
inline constexpr std::int32_t kDtZbTextrel = 0x60005A42;

// Dynamic tag written in place of a DT_NEEDED that names a platform library this runtime has no
// arm32 copy or host-call stub for (kAbsentPlatformLibraries in elf_fixups.cpp). The guest linker
// ignores it like DT_ZB_TEXTREL; d_val keeps the original string offset, so the name stays
// readable. A library that only links one of them (Minecraft PE links libOpenSLES.so and plays
// through FMOD's AudioTrack output) then loads; one that really imports from it fails with the
// missing symbol's name instead of "library not found".
inline constexpr std::int32_t kDtZbDroppedNeeded = 0x60005A43;

// `svc #0x50|rB`, the spin-wait hint written into rewritten test-and-set loops
// (fix_guest_library): rB holds the address of the lock byte. The runtime waits, without
// translated code spinning, until that byte reads 0, and resumes after the svc with every
// register unchanged; the loop then retries its exchange. Linux EABI ignores svc immediates, and
// nothing outside such a loop uses 0x50-0x5f.
inline constexpr std::uint32_t kSpinWaitSvcBase = 0x50;
inline constexpr std::uint32_t kSpinWaitSvcLast = 0x5F;

enum class ElfFixupStatus {
    Unchanged,
    Changed,
    Skipped,
    Error,
};

struct ElfFixupReport {
    ElfFixupStatus status = ElfFixupStatus::Error;
    std::vector<std::string> changes;
    std::string message;
};

// Applies the import-time fixups required by old little-endian ARM ELF32 shared
// libraries. The file is not written until the complete ELF structure has been
// validated and every requested fixup is known to fit.
ElfFixupReport fix_guest_library(const std::string& path);

// True if the ELF32 file behind fd has a DT_ZB_TEXTREL entry in its dynamic section.
bool elf_has_textrel_marker(int fd);

}  // namespace zb
