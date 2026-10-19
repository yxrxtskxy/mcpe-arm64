/* Record locks through fcntl64 under zbrun: the 32-bit struct flock (F_SETLK/F_GETLK) and the EABI
 * struct flock64 (F_SETLK64), the way LevelDB takes its LOCK file. Each call goes through the raw
 * fcntl64 syscall so the guest layout is exactly the one under test.
 *
 * usage: fcntl_locks <scratch file>
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef F_GETLK64
#define F_GETLK64 12
#define F_SETLK64 13
#endif

struct flock64_eabi {
    short l_type;
    short l_whence;
    long long l_start;
    long long l_len;
    int l_pid;
};

static int check(const char* name, int ok) {
    printf("%s=%s\n", name, ok ? "PASS" : "FAIL");
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    int fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 3;
    int ok = 1;

    struct flock lock;
    memset(&lock, 0, sizeof lock);
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 10;
    lock.l_len = 20;
    ok &= check("setlk32", syscall(221 /* fcntl64 */, fd, F_SETLK, &lock) == 0);

    /* Our own lock never conflicts with ourselves: F_GETLK must say F_UNLCK. */
    struct flock probe;
    memset(&probe, 0, sizeof probe);
    probe.l_type = F_WRLCK;
    probe.l_whence = SEEK_SET;
    probe.l_start = 0;
    probe.l_len = 100;
    ok &= check("getlk32_self", syscall(221, fd, F_GETLK, &probe) == 0 && probe.l_type == F_UNLCK);

    struct flock64_eabi wide;
    memset(&wide, 0, sizeof wide);
    wide.l_type = F_RDLCK;
    wide.l_whence = SEEK_SET;
    wide.l_start = 5000000000LL; /* beyond 32 bits */
    wide.l_len = 10;
    ok &= check("setlk64_high", syscall(221, fd, F_SETLK64, &wide) == 0);

    printf("struct_sizes=%s\n", sizeof(struct flock) == 16 && sizeof(struct flock64_eabi) == 32 ? "PASS" : "FAIL");
    close(fd);
    return ok ? 0 : 1;
}
