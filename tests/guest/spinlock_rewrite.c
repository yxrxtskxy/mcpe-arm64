/* The byte spinlock GCC emits for std::atomic_flag, written out instruction for instruction so
 * that zbfix's rewrite (core/src/elf_fixups.cpp) finds and replaces it. Four threads increment a
 * counter under it; after the rewrite the contended path waits through svc #0x50|rB instead of
 * spinning, and the count must still be exact.
 *
 * Build with -mthumb; run `zbfix` on the binary, then `zbrun [rounds]`.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define THREADS 4
static int rounds = 200000;

static volatile unsigned char lock_byte;
static volatile unsigned counter;

static void lock(volatile unsigned char* l) {
    unsigned old, status;
    __asm__ volatile(
        "1: ldrexb %[old], [%[l]]\n"
        "   strexb %[status], %[one], [%[l]]\n"
        "   cmp %[status], #0\n"
        "   bne 1b\n"
        "   dmb ish\n"
        "   cmp %[old], #0\n"
        "   bne 1b\n"
        : [old] "=&l"(old), [status] "=&l"(status)
        : [l] "r"(l), [one] "r"(1u)
        : "cc", "memory");
}

static void unlock(volatile unsigned char* l) {
    __asm__ volatile("dmb ish" ::: "memory");
    *l = 0;
}

static void* worker(void* arg) {
    (void)arg;
    for (int i = 0; i < rounds; ++i) {
        lock(&lock_byte);
        unsigned v = counter;
        counter = v + 1;  /* not atomic: only the lock keeps this exact */
        unlock(&lock_byte);
    }
    return NULL;
}

int main(int argc, char** argv) {
    if (argc > 1) rounds = atoi(argv[1]);
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i) pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    const unsigned expected = (unsigned)(THREADS * rounds);
    const int ok = counter == expected;
    printf("count=%u expected=%u spinlock=%s\n", counter, expected, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
