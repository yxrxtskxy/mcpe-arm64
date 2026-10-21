# Minecraft PE 1.1.5 for arm64-only phones

Newer phones (Snapdragon 8 Elite, Pixel 7 and later, …) can't run 32-bit apps, so the original
Minecraft PE 1.1.5 won't install on them. This project turns your own copy of the game into an
APK that does: the game's 32-bit code is translated to 64-bit while it runs.

## What you need

- A Windows PC with [Git for Windows](https://git-scm.com/) (Git Bash), CMake, Ninja, Python 3,
  curl and unzip on `PATH`.
- WSL with Ubuntu, for the first build only.
- The Minecraft PE 1.1.5 APK (`minecraft-1.1.5.apk`), from your own copy of the game.
- A phone with a 64-bit (arm64) CPU and Android 8 or newer.

## Build

In Git Bash, from the project folder:

```sh
cp /path/to/minecraft-1.1.5.apk game/
sh tools/release.sh
```

The first run downloads and sets up everything else it needs, which takes a while. Later runs only
rebuild. The APK ends up in `out/releases/<version>/`.

The first run also creates a signing key in `~/.mcpe-arm64`. **Back it up.** Updates only install
over a build signed with the same key.

## Install

Copy the APK to the phone and open it, or run `sh tools/release.sh --install` with the phone
connected over USB (USB debugging on). The first launch, and the first after each update, takes a
moment to prepare the game. After that it starts straight away.

To update, install the new APK over the old one. Your worlds are kept.

## Good to know

- **Worlds** are stored inside the app by default, and uninstalling the app deletes them. To keep
  them in `/storage/emulated/0/games/com.mojang`, the folder every Minecraft PE version uses,
  set *File Storage Location* to *External* in the game's settings. Then allow "All files access"
  when Android asks.
- **Custom skins:** *Choose New Skin* opens the file picker. Pick a 64×32 or 64×64 PNG.
- **Short freezes?** Run `adb shell setprop debug.zb.hitch 50` and restart the game. Play until it
  freezes, then run `adb logcat -d -s zbridge`. Each frame of 50 ms or more gets a `hitch:` line
  that says where the time went. The setting lasts until the phone restarts.
- **Problems?** Get the log with `adb logcat -s zbridge zb-launcher mcpe-arm64 MinecraftPE`.
  The *Runtime tools* screen has a report of the last run:
  `adb shell am start -n com.yxrxtskxy.mcpe.arm64/com.zettabridge.launcher.LibraryActivity`.

How it works, what was changed in the translator, and the details of the build are in
[docs/technical.md](docs/technical.md).

## Licences

The translator is [ZettaBridge](https://github.com/ZailoxTT/ZettaBridge), which you may use for
noncommercial purposes only (`upstream/ZettaBridge/LICENSE`). Minecraft belongs to Mojang: build
this only from a copy of the game you own, and don't share the resulting APK.
