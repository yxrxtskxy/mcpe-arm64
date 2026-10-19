// Native method thunk pool for host builds that are not AArch64 (x86_64 test builds of zbrun and
// the guest suite). The real pool (thunks.S) is AArch64 code that ART on the device calls; here
// each 8-byte slot only traps, so the addresses native_thunk_address() hands out stay valid and
// distinct, and anything that really calls one stops at once instead of running garbage.
// The count must match kNativeThunkCount in zb/native_thunks.h.

    .text
    .balign 8

    .globl zb_native_thunk_base
    .hidden zb_native_thunk_base
zb_native_thunk_base:
    .rept 16384
    ud2
    .balign 8, 0xcc
    .endr

    .globl zb_native_thunk_end
    .hidden zb_native_thunk_end
zb_native_thunk_end:

    .section .note.GNU-stack,"",%progbits
