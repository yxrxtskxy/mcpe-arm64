/* Loads Minecraft PE's native libraries through the real arm32 Android linker under zbrun, the
 * way the launcher's library runtime will on the phone: libzbcompat.so first (RTLD_GLOBAL), then
 * libfmod.so and libminecraftpe.so with RTLD_NOW, so every import must bind, and every C++ static
 * constructor of the game runs. Then the entry points the bridge calls are resolved.
 *
 * usage: mcpe_dlopen <dir holding the fixed-up libfmod.so and libminecraftpe.so> <target sdk>
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static int check_symbol(void* handle, const char* name) {
    void* address = handle != NULL ? dlsym(handle, name) : NULL;
    printf("symbol %s=%s\n", name, address != NULL ? "PASS" : "FAIL");
    return address != NULL;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mcpe_dlopen <lib dir> <target sdk>\n");
        return 2;
    }
    const char* dir = argv[1];
    const unsigned target_sdk = (unsigned)strtoul(argv[2], NULL, 10);

    void* dl_android = dlopen("libdl_android.so", RTLD_NOW);
    void (*set_target_sdk)(unsigned) =
        dl_android != NULL ? (void (*)(unsigned))dlsym(dl_android, "android_set_application_target_sdk_version") : NULL;
    printf("target_sdk=%s\n", set_target_sdk != NULL ? "PASS" : "FAIL");
    if (set_target_sdk != NULL) set_target_sdk(target_sdk);

    printf("compat=%s\n", dlopen("libzbcompat.so", RTLD_NOW | RTLD_GLOBAL) != NULL ? "PASS" : "FAIL");

    static const char* const libs[] = {"fmod", "minecraftpe"};
    void* handles[2] = {0};
    int ok = 1;
    for (int i = 0; i < 2; ++i) {
        char path[512];
        snprintf(path, sizeof path, "%s/lib%s.so", dir, libs[i]);
        handles[i] = dlopen(path, RTLD_NOW);
        printf("dlopen %s=%s\n", libs[i], handles[i] != NULL ? "PASS" : "FAIL");
        if (handles[i] == NULL) {
            ok = 0;
            fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
        }
    }

    ok &= check_symbol(handles[1], "JNI_OnLoad");
    ok &= check_symbol(handles[1], "ANativeActivity_onCreate");
    ok &= check_symbol(handles[1], "Java_com_mojang_minecraftpe_MainActivity_nativeRegisterThis");
    ok &= check_symbol(handles[0], "JNI_OnLoad");
    ok &= check_symbol(handles[0], "Java_org_fmod_MediaCodec_fmodGetSize");

    /* FMOD probes OpenSL ES at run time; it must fail here so FMOD uses its AudioTrack output. */
    void* opensl = dlopen("libOpenSLES.so", RTLD_NOW);
    printf("opensl_absent=%s\n", opensl == NULL ? "PASS" : "FAIL");
    ok &= opensl == NULL;
    return ok ? 0 : 1;
}
