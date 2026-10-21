/* libnetd_client.so for the guest: name lookups through netd.
 *
 * Bionic resolves names by asking netd over /dev/socket/dnsproxyd, and it gets the function that
 * opens that socket from this library: libc dlopens libnetd_client.so at startup and takes each
 * netdClientInit* function the library exports. Without the library libc opens no proxy, falls
 * back to resolving by itself against 0.0.0.0:53, and nothing resolves; an arm64-only device has
 * no arm32 copy to load. Minecraft PE then reports every server name as "Invalid IP address!".
 *
 * Only the DNS proxy is provided. The platform library also routes socket(), connect() and the
 * rest through netd's fwmarkd to tag sockets with a network; without those exports libc keeps
 * its plain calls, and sockets stay untagged as they were.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef int (*DnsOpenProxyType)(void);

static int dns_open_proxy(void) {
    /* The platform library's own opt-out, kept for parity. */
    const char* mode = getenv("ANDROID_DNS_MODE");
    if (mode != NULL && strcmp(mode, "local") == 0) {
        errno = ENOSYS;
        return -1;
    }
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s == -1) return -1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, "/dev/socket/dnsproxyd");
    int result;
    do {
        result = connect(s, (const struct sockaddr*)&address, sizeof address);
    } while (result == -1 && errno == EINTR);
    if (result != 0) {
        int error = errno;
        close(s);
        errno = error;
        return -1;
    }
    return s;
}

__attribute__((visibility("default"))) void netdClientInitDnsOpenProxy(DnsOpenProxyType* function) {
    if (function != NULL) *function = dns_open_proxy;
}
