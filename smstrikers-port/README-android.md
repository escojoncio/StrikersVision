# Android

> **This fork is the Meta Quest version.** Start with the [README at the top of the repo](../README.md).
> This page is the phone port's own documentation (upstream), kept for the build details; its
> `apk/` folder and some of its notes do not apply to the Quest build.

Super Mario Strikers, natively, on an arm64 Android device. Same game code, same Aurora, same
renderer; what is Android-specific is confined to four files and a Gradle project.

Requires game data from your own copy of Super Mario Strikers. No game assets are included.

A built APK is in [`apk/`](../apk/), with its checksum and a note on what its signature does and
does not prove.

## What it needs

- **A 64-bit ARM device on Android 9 (API 28) or later, with Vulkan.** Dawn has exactly one backend
  on Android and it is Vulkan; there is no GL fallback worth having. 32-bit and x86 Android are not
  built: the prebuilt Dawn package exists for `android-aarch64` alone, and a 32-bit address space is
  not a comfortable place for this game's heap.
- **Your own disc.** A disc image (`.iso`, `.gcm`, `.ciso` or `.gcz`) or the extracted disc's
  `files` folder, US, European or Japanese.

## Building

Two steps, deliberately separate. The native build is CMake and the NDK, the same build every other
platform gets; Gradle only packages what it produced, so Gradle's own idea of which CMake and which
NDK to use never gets a vote.

```sh
# 1. the game
tools/build-android.sh                  # -> build-android/jniLibs/arm64-v8a/

# 2. the app
cd android && ./gradlew assembleDebug   # -> app/build/outputs/apk/debug/
```

The first one wants `ANDROID_SDK_ROOT` (or `ANDROID_HOME`) pointing at an SDK with NDK
**29.0.14206865** installed, which is the NDK Aurora's prebuilt Dawn was compiled with. A different
NDK is a different libc++, and `libwebgpu_dawn.a` is a static archive that has to meet the one it
was built against.

```sh
sdkmanager "ndk;29.0.14206865" "platforms;android-35" "build-tools;35.0.1"
```

`tools/build-android.sh` runs the same three steps as `tools/rebuild.sh` -- compile, generate stubs,
link -- because the stub set cannot be known until every translation unit has compiled, and then
stages `libmain.so` and `libc++_shared.so` where the Gradle project expects them. It rewrites
`src/platform/stubs_generated.c`, which is generated output that happens to be checked in and is
per-object-format: the copy in the tree carries Mach-O's leading underscore on every symbol and
satisfies nothing on ELF.

## Where the game data goes

Copy the disc image into the app's own folder on shared storage, over USB from a PC or with a file
manager:

```
Android/data/io.github.newcoke.strikers/files/
```

Nothing needs a permission there. The app's first screen prints the exact path and copies it to the
clipboard.

If you would rather keep the image somewhere you can find it -- `/sdcard/Strikers`, say -- the same
screen offers **All files access**, and with it the game looks in `Strikers`, `Games/Strikers`,
`ROMs/GameCube` and `Download` as well.

Either form works: a whole disc image, or the extracted disc's `files` folder (1416 files, about
618 MB) put at `.../files/files`.

## Settings

The desktop port's settings application is Qt, and Qt is not here. Instead the app writes a
`strikers.ini` into that same folder on first run, with the keys that matter on a handheld and the
defaults this port picked for one; it is never overwritten, so it is yours to edit.
`strikers.ini.example` documents the full set.

It sets three keys and no more, because only three of them differ on Android: `backend = vulkan`,
which is the only backend Dawn has here; `fps_limit = 60`, because a 120 Hz panel would otherwise
have the limiter follow it and spend twice the GPU for a simulation that still steps 60 times a
second; and `vsync = 1`. Everything else is left at the port's own default, with the keys worth
reaching for -- `res_scale`, `msaa`, `aniso` -- written out commented so they are easy to find.

## Controls

A Bluetooth or USB controller is a controller: SDL sees it, Aurora maps it, and the button prompts
follow it, exactly as on the desktop.

Without one, the game draws an on-screen GameCube pad -- stick, A/B/X/Y, L, R, Z and Start. It is
not a special input path: it drives an SDL virtual joystick declared as a gamepad, so PAD sees an
ordinary controller arriving and everything downstream of that is the code a real controller uses.
It disappears the moment a real controller connects and comes back when it leaves. `touch = 0` in
`strikers.ini` turns it off for good.

## What is missing

- **THP movies.** The build wants libavcodec and there is no Android one in the tree yet, so the
  attract-mode and cutscene videos are skipped and the game runs without them. Point
  `-DSTRIKERS_FFMPEG_ROOT` at an Android build of ffmpeg to get them back.
- **The settings app.** See above.
- **Online multiplayer**, which the port does not have on any platform.

## Reading a crash

The port installs no signal handler on Android: `debuggerd` already writes a tombstone with
symbolised frames, the registers and the other threads, which is strictly more than the desktop
handler prints.

```sh
adb logcat -s strikers SDL DEBUG AndroidRuntime
adb pull /sdcard/Android/data/io.github.newcoke.strikers/files/strikers-log.txt
```

The second is the port's own log: `STRIKERS_LOG` is pointed at it before the game starts, because
stderr on Android goes nowhere anyone can read.
