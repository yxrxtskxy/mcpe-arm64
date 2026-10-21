# Technical notes: Minecraft PE 1.1.5 for arm64-only phones

Builds one installable APK, `out/MinecraftPE-1.1.5-arm64.apk`, that runs the original 32-bit
Minecraft PE 1.1.5 on phones with no 32-bit CPU mode (e.g. Snapdragon 8 Elite, Pixel 7 and later).

The game's Java code runs unchanged on the phone's normal 64-bit ART. Only its 32-bit native
libraries (`libminecraftpe.so`, `libfmod.so`) are translated to ARM64 at run time by
[ZettaBridge](https://github.com/ZailoxTT/ZettaBridge), which uses Dynarmic's ARM32→ARM64 JIT.
It also runs the real 32-bit Android linker and bionic, and passes GL/EGL/input/assets/JNI
calls through to the phone.

## How the APK is put together

```
MinecraftPE-1.1.5-arm64.apk  (package com.yxrxtskxy.mcpe.arm64, lib/arm64-v8a only)
 ├─ GameLauncherActivity      first launch: imports assets/game/base.apk as a plugin
 ├─ ZettaBridge launcher      plugin host: :guest process, stub activities, plugin Context/ClassLoader
 ├─ lib/arm64-v8a/libzbridge.so   translator + JNI bridge + NativeActivity glue
 └─ assets/
     ├─ game/base.apk         the original game APK, unmodified
     └─ zb/                   32-bit Android sysroot (linker, libc, libm, libc++...), guest stubs
                              (libGLESv2, libEGL, libandroid...), zbhost, the arm64 proxy library
```

At first launch the game APK is copied into the app's private storage, and its two arm32
libraries are extracted and fixed up for the modern linker. After that every launch goes straight
into the game.

**Where worlds live.** By default, the game keeps its worlds in the app's private storage, and
uninstalling the app deletes them. The game's "File Storage Location" setting, set to External,
uses `/storage/emulated/0/games/com.mojang` instead, the folder every Minecraft PE version uses. This
needs "All files access": the first time you switch, the app opens that settings page. Once you
allow it, the switch sticks. Without it the game switches back to Application.
(`StorageAccessActivity`; the app requests `MANAGE_EXTERNAL_STORAGE`, because Android 11+ never
grants the old storage permission to it.)

**Custom skins.** "Choose New Skin" opens the system file picker. The launcher copies the chosen
image into its cache and gives the game that copy's path (`ImagePickActivity`,
`PickedImageProvider`). The game used to ask for storage permission first, because it opened the
picked file by its own path. After each import, `GamePatches.java` changes two instructions in
`libminecraftpe.so`, so the skin picker no longer asks. The change is only applied if the
function's bytes match this build of the game exactly.

## Layout

| Path | What |
|---|---|
| `android/` | The app (Gradle). Java sources of the ZettaBridge launcher are used in place from `upstream/`; `app/src/main` holds this project's manifest, resources, `GameLauncherActivity` and `GamePatches`. |
| `upstream/` | ZettaBridge and its Dynarmic, vendored, with this project's changes (see below) made in place. `upstream/README.md` names the upstream commits and shows how to list the changes. |
| `tools/` | Toolchain setup, fetch, build and signing scripts. |
| `tests/` | Local load test: Minecraft's libraries through the real guest linker on this PC. |
| `game/` | Put `minecraft-1.1.5.apk` here (not committed). |
| `build/`, `out/`, `.deps/` | Generated. |

## Changes to ZettaBridge

In `upstream/ZettaBridge`.

- **`ANativeActivity_*` helpers** (`finish`, `show/hideSoftInput`, `setWindowFlags/Format`): new guest
  stubs (host calls 579-583) forwarded to the framework activity. Minecraft imports
  `ANativeActivity_finish`; without it the library does not load at all.
- **Dropped `DT_NEEDED libOpenSLES.so`** at import (`elf_fixups.cpp`, `fix_guest_lib.py`). The game
  links OpenSL ES without importing anything from it. FMOD's own runtime probe then fails cleanly,
  and FMOD plays through its Java AudioTrack output instead.
- **CP15 generic timer** (CNTVCT/CNTFRQ) and a logged no-op for every other unhandled coprocessor
  access. OpenSSL's CPU probe in the game's static constructors reads CNTVCT, and before this
  change Dynarmic aborted the whole process at JIT time.
- **x86_64 test build** of `zbrun`/`zbfix` (placeholder JNI thunk pool). Two WSL1 fallbacks are
  harmless on phones: `MADV_WIPEONFORK` and `memfd_create`.
- **Atomic `STREX`.** The exclusive-store callbacks compared and then wrote as two plain
  operations. A spinlock release from another thread could land between them and be overwritten,
  leaving the lock set with no owner. World loading hung on FMOD's lock at "Syncing pack with
  server…" because of this. They are now one host compare-exchange.
- **Spin-wait rewrite** at import (`elf_fixups.cpp`, not `fix_guest_lib.py`). The game's 45
  `atomic_flag` test-and-set loops get `svc #0x50|rB` on their contended path. The runtime waits for
  the lock byte to clear (`yield`, then `sched_yield`, then short sleeps) instead of
  translated code spinning against the holder.
- **Android plumbing the game needs:**
  - Looper ident registrations (callback-less `ALooper_addFd`).
  - The `AAssetDir` and 64-bit `AAsset` calls.
  - `AInputQueue_attachLooper` reading its 5th argument from the stack.
  - `fcntl` record locks for LevelDB's `LOCK` file.
  - `SIOCGIF*` ioctls.
  - `/data/data/<game package>` aliased to the plugin's data directory. Paths under it that the
    game hands to Java (`NewStringUTF`) are rewritten to the real directory, because the game
    reads some files back through Java. Its custom skin, `custom.png`, used to come back empty:
    black, and then rejected.
  - DNS through netd. The guest's bionic gets its resolver socket from `libnetd_client.so`, and
    an arm64-only phone has no arm32 copy, so every name lookup fell back to `0.0.0.0:53` and
    failed ("Invalid IP address!" for any server name). A guest `libnetd_client.so`
    (`guest/compat/netd_client.c`) provides only the `/dev/socket/dnsproxyd` connection.
- **Launcher.**
  - `org.apache.http.legacy` and `android.test.base` on the plugin class path, as the platform
    gives apps with an old targetSdk.
  - `RECEIVER_EXPORTED` added for those apps.
  - `Resources.getIdentifier` mapping the host package to the game's.
  - `ANativeActivity_onCreate` finding `ZBridge` from any thread.
  - Storage permission requests (`READ`/`WRITE_EXTERNAL_STORAGE`) are answered through "All files
    access" when the host declares `MANAGE_EXTERNAL_STORAGE`. `StorageAccessActivity` opens its
    settings page and returns the user's answer. The plugin context reports both permissions as
    granted while access is allowed.
  - Image picks (`ACTION_PICK` on images) open the system file picker through `ImagePickActivity`.
    The result is a private copy whose MediaStore `_data` column is a readable path
    (`PickedImageProvider`). Old apps open that path themselves. Under scoped storage the original
    path is often unreadable, and Photos or document picks have no `_data` column at all.
- **GL calls served inside translated code.** A host call used to stop the JIT and enter it again.
  That costs more than most GL calls do, and it wipes the JIT's return predictions, so the
  returns that follow went through the slow dispatcher too. At high view distance the game makes
  about 13,000 GL calls per frame.
  - GL calls now run from the `svc` callback itself (`InlineHostCalls`), with the host's FPCR
    restored for the duration.
  - Six calls still stop the JIT, because they allocate guest memory through the guest's
    `malloc`/`free`: `glGetString*`, and mapping and unmapping buffers.
- **Runtime report off the render thread.**
  - The report counted every GL call under two mutexes, with a `gettid` syscall and a clock read.
  - Once a second, whichever game thread came next rewrote the report file with an `fsync`.
  - Now the GL counter is lock-free, the thread id is cached, and counter-only updates are
    written by a background thread (`zb-report`).
- **Short freezes while moving** (found with the freeze tracer below; numbers under Status).
  - *An exclusive monitor without a lock.* Every `LDREX` and `STREX` of every guest thread took
    Dynarmic's one global spin lock, and every `STREX` scanned all 256 reservations under it. The
    game does that for each `shared_ptr` copy, mutex and allocation. When the kernel preempted
    the holder, the other threads spun on their CPUs, so the render thread stayed busy without
    getting anywhere. ZettaBridge's `STREX` already compare-and-swaps against what `LDREX` read,
    so the monitor is now value-checked and takes no lock (see Changes to Dynarmic).
  - *Render thread priority.* Every guest thread inherits the same priority (nice -10), and FMOD
    raises its own threads to -16 and -19. While seven workers built chunks, the render thread
    waited up to a second at a time for one of the six cores an app gets. On its first
    `eglSwapBuffers` it now goes to -16 (`setprop debug.zb.render_nice <n>` overrides it, 0 leaves
    it alone). Threads it creates afterwards start at its old priority.
- Diagnostics: per-thread activity dumps, `setprop debug.zb.profile 1` (the busiest guest threads
  with their hottest blocks in logcat every 10 s) and `setprop debug.zb.strace net|all`.
- **Freeze tracer**, `setprop debug.zb.hitch <ms>` (`hitch_trace.cpp`), read when the game starts.
  - Every frame of the render thread (one `eglSwapBuffers` to the next) that takes at least that
    long gets one `hitch:` line in logcat. It splits the frame into translating new guest code,
    syscalls, host calls and spinlock waits, each with its longest single wait. A JNI call names
    its Java method.
  - What is left is guest code. The line also has the thread's time on and off the CPU, its page
    faults and context switches.
  - A `hitch summary:` line follows every 30 s, with how full the render thread's code cache is.
  - With the profiler on as well, each logged frame names its hottest guest addresses.
  - Off, it costs one test per syscall and host call.
- `PluginStore.importApk(Context, InputStream)` for the bundled APK, and a Windows path fix in
  `check_launcher_bundle.py`.

## Changes to Dynarmic

In `upstream/ZettaBridge/third_party/dynarmic`, on top of ZettaBridge's own two Dynarmic patches.

- **Fast dispatch for the arm64 backend.** The x86_64 backend has a fast-dispatch cache; the arm64
  backend left it as a TODO. Without it, every indirect branch the return predictions miss
  (virtual calls, function pointers, jump tables) did a C++ hash-map lookup. In the profile that
  was about 16% of the game's render thread.
  - The dispatcher now first probes a 64K-entry direct-mapped table in a few instructions.
  - A miss fills the table from the C++ lookup.
  - Code-cache flushes and block invalidation clear the affected entries.
  - The table is anonymous memory, committed only where entries are written.
- **Translation statistics.** `A32::Jit::GetTranslationStats()` reports, readable from any thread:
  - blocks translated and the time spent translating them;
  - full code-cache flushes;
  - code cache use.

  Both backends implement it. The freeze tracer reads it.
- **Value-checked exclusive monitor**, `ExclusiveMonitor(processor_count, value_checked)`.
  - It is for exclusive write callbacks that compare-and-swap against the value the exclusive read
    returned, as ZettaBridge's do.
  - Each processor then keeps its reservation without the global lock. A store leaves the other
    reservations alone, and their own compare-and-swap fails instead.
  - The one difference from real hardware: a change that writes the read value back goes
    unnoticed, as with any compare-and-swap.
  - The default stays as it was. It cannot be combined with `fastmem_exclusive_access`, whose
    inline x86_64 code takes the lock.

## Building (Windows + WSL)

One command, in Git Bash:

```sh
cp /path/to/minecraft-1.1.5.apk game/
sh tools/release.sh               # add --install to put it on the connected phone
```

The result is in `out/releases/<versionName>-arm64.<versionCode>/`:
- `MinecraftPE-<versionName>-arm64.<versionCode>.apk`
- `SHA256SUMS`
- `BUILD-INFO.txt`, which records the game APK, the commit it was built from, the upstream
  ZettaBridge and Dynarmic commits, the signing certificate and the toolchain.

Any missing piece is set up first, and pieces already present are skipped:

| Piece | Comes from |
|---|---|
| Android SDK 35, NDK r29, JDK 17, Gradle 8.11.1 | `tools/setup_toolchain_windows.sh`, checksum-verified |
| arm32 sysroot | `extract_sysroot.sh` in WSL: an AOSP GSI, ~1.2 GB download, needs `debugfs` |
| Boost headers | WSL's `libboost-dev`, copied into `.deps/boost-headers` |
| Signing key | `tools/make_signing_key.sh`, written to `~/.mcpe-arm64` |

Git, curl, unzip, CMake, Ninja and Python 3 must be on `PATH`. WSL is used only while the sysroot or
the Boost headers are missing.

**Versions.** The app's `versionName` is the game's own, read from the game APK at build time
(`1.1.5.1`). For a new release, bump `versionCode` in `android/app/build.gradle.kts`. Re-running
with the same `versionCode` replaces that release.

**Uncommitted changes.** `release.sh` builds the files as they are, committed or not. If some
changes are uncommitted, `BUILD-INFO.txt` says so, because the commit alone would not rebuild that
release. For a quick test build without a release folder, `sh tools/build_apk.sh` builds
`out/MinecraftPE-1.1.5-arm64.apk`. `BUILD_TYPE=debug` makes that build debuggable.

**Signing key.** Keep `~/.mcpe-arm64/`, which holds the key and its passwords. An update only
installs over a build signed with the same key.

Local check without a phone (WSL; x86_64 `zbrun` runs the guest through Dynarmic's x64 backend):

```sh
sh tools/wsl_build_host_x64.sh && sh tests/run_mcpe_dlopen.sh
```

## Status

- Built and statically verified: arm64-v8a only, 16 KiB-aligned, v2/v3-signed, targetSdk 35.
- Verified on this PC: the game and FMOD load through the real 32-bit Android linker with every
  import bound, and the game's static constructors run under translation. ZettaBridge's own
  guest suite passes on the x86_64 build, apart from file-syscall cases WSL1 does not implement.
- **Runs on an arm64-only phone.** Tested on an ASUS ROG Phone 8 (Snapdragon 8 Gen 3, Android 16,
  `ro.product.cpu.abilist` = arm64-v8a).
  - Install, launch and the main menu work, and so do touch input and FMOD audio.
  - Creating a world and opening it into play works.
  - Frame rate in one world, standing still, debug builds, display at 120 Hz:

    | View distance | Before the speed-ups | 1.1.5-arm64.2 |
    |---|---|---|
    | 224 blocks | 108–112 fps | 120 fps (the display's limit) |
    | 448 blocks | 39 fps | 57–58 fps |

    The game is CPU-bound on its render thread. Translated game code is the largest share,
    followed by the GL driver.
  - Flying through new terrain in a creative world for 40 s, with the freeze tracer on (two runs
    each):

    | | 1.1.5-arm64.2 | 1.1.5-arm64.3 |
    |---|---|---|
    | Frames of 50 ms or more | 59–73 | 3 |
    | Time spent in them | 10.4–12.8 s | 0.2 s |
    | Longest frame | 680–868 ms | 74–79 ms |
    | Frame rate while moving | 52–57 fps | 95–105 fps |

    The lock-free exclusive monitor makes nearly all of the difference. The render thread's
    priority halves what is left. A 2.5-minute flight afterwards had 3 frames over 100 ms, the
    longest 134 ms.
- First launch after each install or update spends a few seconds importing the bundled game; the
  game's data directory (worlds, options) is kept.
- Untested: Xbox Live sign-in, multiplayer and Realms, the Store, long sessions. ZettaBridge itself is
  early-stage software.
- Local check: `tests/guest/spinlock_rewrite.c` (4 threads on one byte lock, before and after
  `zbfix`). It used to hang under `zbrun` before the `STREX` fix and now counts exactly at 20M
  increments.
- Phone check of the arm64 fast dispatch: `tests/guest/indirect_dispatch.c`, run under an arm64
  `zbrun` (`ninja -C upstream/ZettaBridge/build/android-arm64 zbrun`, pushed to
  `/data/local/tmp` with the sysroot).
  - It covers ARM and Thumb calls through function pointers, two generated stubs that share one
    table slot, and code rewritten in place followed by `cacheflush`.
  - It passes, and the indirect-call loop runs in 29 ns per iteration instead of 39.5 ns.
- ZettaBridge's host tests (`tests/host`, built with `ZB_BUILD_TESTS=ON` in WSL): the same 21
  tests fail under WSL1 with and without this project's changes. The causes are environmental:
  Xbyak "code is too big" and symlink loops. The other 32 pass, including the new inline host
  call test and the updated runtime report test.

Troubleshooting on the phone: **Runtime tools** → the game → *Last run report*. Runtime tools has
no icon or shortcut. It opens from the "could not be started" screen, or with
`adb shell am start -n com.yxrxtskxy.mcpe.arm64/com.zettabridge.launcher.LibraryActivity`. Then
`adb logcat -s zbridge zb-launcher mcpe-arm64 MinecraftPE`. With a debug build
(`BUILD_TYPE=debug sh tools/build_apk.sh`), `adb shell run-as com.yxrxtskxy.mcpe.arm64 cat
files/zb-runtime-report.txt` reads the report directly.

## Licences

ZettaBridge is source-available under PolyForm Noncommercial 1.0.0 + PolyForm Perimeter 1.0.1:
noncommercial use only, and no competing product. Its licence text is in
`upstream/ZettaBridge/LICENSE`, and covers that directory, this project's changes in it included.
Dynarmic is 0BSD. The AOSP runtime libraries are
Apache-2.0. Minecraft is Mojang's: build this only from a copy of the game you own, and do not
distribute the resulting APK.
