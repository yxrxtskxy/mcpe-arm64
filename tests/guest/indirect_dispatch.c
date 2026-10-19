/* Indirect branches through Dynarmic's dispatcher and its arm64 fast-dispatch table
 * (patches/dynarmic): calls through function pointers between ARM and Thumb code, two generated
 * stubs that share one table slot, and a stub rewritten in place, whose old translation the
 * table must forget after cacheflush.
 *
 * Build with armv7a clang -O2, then `zbrun indirect_dispatch [rounds]`.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define ARM __attribute__((target("arm"), noinline))
#define THUMB __attribute__((target("thumb"), noinline))

/* Each function and a reference copy of its formula, which the compiler inlines. */
#define F(name, mode, expr)                     \
    mode static uint32_t name(uint32_t x) {     \
        return expr;                            \
    }                                           \
    static inline uint32_t name##_ref(uint32_t x) { \
        return expr;                            \
    }

F(f0, ARM, x * 3u + 1u)
F(f1, THUMB, x ^ 0x5a5a5a5au)
F(f2, ARM, (x << 7) | (x >> 25))
F(f3, THUMB, x + 0x9e3779b9u)
F(f4, ARM, x * 0x01000193u)
F(f5, THUMB, ~x)
F(f6, ARM, x - (x >> 3))
F(f7, THUMB, (x >> 11) ^ (x << 5))

typedef uint32_t (*fn_t)(uint32_t);
static fn_t const table[8] = {f0, f1, f2, f3, f4, f5, f6, f7};

static uint32_t reference(unsigned i, uint32_t x) {
    switch (i) {
    case 0: return f0_ref(x);
    case 1: return f1_ref(x);
    case 2: return f2_ref(x);
    case 3: return f3_ref(x);
    case 4: return f4_ref(x);
    case 5: return f5_ref(x);
    case 6: return f6_ref(x);
    default: return f7_ref(x);
    }
}

/* The table index of a Thumb address; both stubs are Thumb, so the upper descriptor cancels. */
static uint32_t slot(uint32_t pc) {
    return ((pc >> 1) ^ (pc >> 17)) & 0xFFFFu;
}

/* Thumb "movs r0, #value; bx lr". */
static void write_stub(uint16_t* at, uint8_t value) {
    at[0] = (uint16_t)(0x2000u | value);
    at[1] = 0x4770u;
    __builtin___clear_cache((char*)at, (char*)(at + 2));
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    const long rounds = argc > 1 ? atol(argv[1]) : 2000000;
    int failures = 0;

    /* 1. ARM <-> Thumb calls through pointers. */
    uint32_t x = 12345;
    const double start = now();
    for (long n = 0; n < rounds; ++n) {
        x = x * 1664525u + 1013904223u;
        const unsigned i = x >> 29;
        const uint32_t got = table[i](x);
        if (got != reference(i, x)) {
            if (++failures <= 5) printf("call f%u(0x%08x) = 0x%08x, want 0x%08x\n", i, x, got, reference(i, x));
        }
    }
    const double elapsed = now() - start;
    printf("indirect calls: %ld in %.3f s (%.1f ns each)\n", rounds, elapsed, elapsed * 1e9 / (double)rounds);

    /* 2. Two stubs in one table slot, called alternately: every call evicts the other. */
    const size_t size = 0x80000;
    uint8_t* code = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        puts("mmap failed");
        return 2;
    }
    uint16_t* a = (uint16_t*)(code + 0x100);
    uint16_t* b = NULL;
    for (uint8_t* p = code + 0x20000; p + 4 <= code + size; p += 2) {
        if (slot((uint32_t)(uintptr_t)p) == slot((uint32_t)(uintptr_t)a)) {
            b = (uint16_t*)p;
            break;
        }
    }
    if (b == NULL) {
        puts("no colliding address found");
        return 2;
    }
    write_stub(a, 11);
    write_stub(b, 22);
    const fn_t call_a = (fn_t)((uintptr_t)a | 1u);
    const fn_t call_b = (fn_t)((uintptr_t)b | 1u);
    for (int n = 0; n < 10000; ++n) {
        if (call_a(0) != 11 || call_b(0) != 22) {
            if (++failures <= 5) printf("colliding stubs returned %u and %u\n", call_a(0), call_b(0));
            break;
        }
    }

    /* 3. Rewritten code: the table must not keep the old translation. */
    for (int round = 0; round < 50; ++round) {
        const uint8_t value = (uint8_t)(100 + round);
        write_stub(a, value);
        for (int n = 0; n < 100; ++n) {
            const uint32_t got = call_a(0);
            if (got != value) {
                if (++failures <= 5) printf("rewritten stub returned %u, want %u\n", got, value);
                break;
            }
        }
    }

    printf("indirect_dispatch=%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
