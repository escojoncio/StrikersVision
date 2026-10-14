# StrikersVision

Super Mario Strikers (GameCube) running natively on **Apple Vision Pro**: the big screen in your
room, a miniature pitch on your table, or the life-size stadium around you. Not an emulator: the
game is built from the community decompilation and compiled for the headset's Apple silicon, the
same source the Meta Quest mixed-reality port
([`LanceSweatpants/Strikers-Quest-MR`](https://github.com/LanceSweatpants/Strikers-Quest-MR)) builds
for Android. This repository carries that port's code and adds a visionOS layer: Compositor Services
and Metal for the 3D views, ARKit for the room, hands and controllers, SwiftUI for the launcher, and
SharePlay.

*You need your own copy of the game. **No game files are included**, in this repository or in the
app.*

### [⬇ Download StrikersVision.ipa](../../releases/download/visionos/StrikersVision.ipa)

## Status

**Alpha, untested on a headset.** The app builds in CI but has not yet run on an Apple Vision Pro;
until the first headset test, expect it not to work at all. This page describes what the port is
built to do.

## What it is

Four ways to watch the same match, as on the Quest, switched any time with the controller sticks or
from the launcher window:

- **Big screen · your room**: a giant flat screen floating in your room (passthrough). The game
  starts here unless you pick another view in Settings.
- **Big screen · stadium**: the same screen, with you standing on the pitch of the life-size
  stadium your match is played in.
- **Tabletop · your room**: the match as a miniature 3D pitch on your real table, bed or floor.
  Walk around it, grab it, turn it and resize it with your hands or the controllers.
- **Tabletop · stadium**: the table inside the life-size stadium.

And around the game:

- **Goal celebrations in 3D** (confetti, streamers, fireworks) and the game's white flashes lighting
  up your view, from the Quest port.
- **Controller vibration** on the PlayStation VR2 Sense controllers.
- **Online 1v1 with room codes**: one player hosts and reads out a four-letter code, the other types
  it in. It uses the same relay and the same protocol as the Quest port, so an Apple Vision Pro and
  a Quest 3 on matching releases can play each other.
- **SharePlay**: in a FaceTime call, start a match with the people in it. The table's position is
  shared, so everyone in a shared space sees it in the same place.
- **Your disc image as it is**: `.iso` (NKit too), Dolphin's `.rvz` and `.wia`, and `.gcm`, `.ciso`,
  `.gcz`. The launcher shows which file it found and its region.
- **Every option inside the app**: view, picture quality, vibration, volume, game language, log
  detail. Nothing to edit by hand.
- **The game's log inside the app**, with copy and share.
- **English and Spanish**, following the headset's language.

## How to install

The release is an **unsigned `.ipa`**. Sign and install it with a sideloading tool such as
[SideStore](https://sidestore.io) or any other tool that installs `.ipa` files on Apple Vision Pro
with your Apple ID.

1. Download [`StrikersVision.ipa`](../../releases/download/visionos/StrikersVision.ipa).
2. Install it with your sideloading tool.
3. Open **StrikersVision** once. It creates its folder in the Files app.

## Your game disc

1. Copy your Super Mario Strikers disc image to the headset (AirDrop, iCloud Drive, a USB drive...).
2. In the **Files** app, move it to **On My Apple Vision Pro → StrikersVision → Strikers**. A
   text file there says what goes in.
3. Or, in StrikersVision, press **Pick a folder…** and choose any folder that holds the image
   (iCloud Drive, a USB drive). The app remembers it.

The launcher shows the file it found and its region (`G4QE01` USA, `G4QP01` Europe, also sold as
*Mario Smash Football*, `G4QJ01` Japan). With several images in one folder, the one whose name sorts
first is used. The US disc is the one tested on the Quest; the others are untested.

**Game language**: Settings → Language. *Automatic* uses the headset's language when the disc has
it, and English otherwise. The US disc is English only; the European disc has English, German,
French, Spanish and Italian; the Japanese disc has all six.

## Controls (PlayStation VR2 Sense controllers)

The Quest port's layout, with each Touch button moved to the Sense button in the same place.

### Playing

| Button | What it does |
| --- | --- |
| **Left stick** | Move |
| **Cross** (✕) | Pass |
| **Circle** (○) | Shoot |
| **Square** (□) | Use your item |
| **Triangle** (△) | Hit |
| **Right grip** | Swap items |
| **Left / right trigger** | The GameCube's L / R |
| **Right stick** | The GameCube's C-stick |
| **Options** | Pause |

A Bluetooth gamepad paired to the headset also works for offline play.

### Switching views

"Click" a stick means pressing it straight down. Switching needs **two quick clicks**, so it can't
happen by accident mid-match.

| Do this | What happens |
| --- | --- |
| **Left stick: click twice fast** | Your room ⇄ the stadium |
| **Right stick: click twice fast** | Big screen ⇄ tabletop |
| **Right stick: click once** | Big screen: change its size |
| **Left stick: click once** | Tabletop: bring the table back in front of you |

The launcher window also has a view picker while the game runs.

### Moving the table (Tabletop)

- **Grab it**: hold the **left grip** and move your hand; let go to drop it there.
- **Resize or turn it**: keep holding the left grip and push the **left stick** up/down to resize
  and left/right to turn.
- **With your hands**: pinch and drag, or use both hands to resize and turn.
- **Lost it?** Click the left stick once.

## Play online with a friend

Both of you need matching releases and your own game files.

1. **One of you hosts**: **Host an online match** shows a 4-letter code. Tell it to your friend and
   press **Start**.
2. **The other joins**: **Join with a code**, type it, **Join**.
3. **Both of you**: pick **Grudge Match**, choose your teams and start. Whoever gets there first
   waits on the loading screen; then you are playing each other.

How it works, as on the Quest: delay-based lockstep, only button presses travel, one headset is
picked as the home side (its stadium and settings), and each player keeps their own view. If the
connection drops for about ten seconds, the online match ends. A room code is not a password:
anyone with the same code lands in the same room, and a third headset is ignored while two play.

## SharePlay

In a FaceTime call with other Apple Vision Pro users, press **Play with SharePlay** (or accept the
activity someone else started). The group becomes the room: the first two people who start a Grudge
Match play each other, exactly as with a code; everyone else stays in the call and can play their
own offline match.

- **Spatial Personas**: the app asks for a shared immersive space with a table-shaped seating
  template (the two players face each other across the table, others stand along the sides), and
  shares the table's position with the group. Apple's current Compositor Services, which the game
  uses to draw its 3D views, do not show Personas inside such a space, so on visionOS 26 Personas
  are visible in the shared windows but not in the game's own space.
- **The permission**: SharePlay needs the `com.apple.developer.group-session` entitlement. Signing
  with a free Apple ID removes it; the app notices, says so, and turns the SharePlay button off.
  Online rooms with a code work without it.

## Known limitations

- **Untested on hardware** (see Status).
- **No intro or attract movies**: the build has no video decoder for the game's THP clips.
  Everything else runs without them.
- **SharePlay** needs the group-session entitlement, which free-account signing strips.
- **Personas don't appear inside the game's immersive space** (see SharePlay).
- **One game per app launch**: after quitting a game, close the app completely and open it again.
- **The shared room server runs on Cloudflare's free plan**. If it hits the daily limit, online play
  won't connect until it resets (00:00 UTC). Offline play is not affected.

To report a problem, open **Log** in the launcher and share it. The same file is
`Strikers/strikers-log.txt` in the Files app.

## Building from source

The app is built by GitHub Actions on a macOS runner with Xcode 26 (the visionOS workflow in
`.github/workflows/`). The workflow builds Dawn for Metal, SDL3 and the game as static libraries
for `xros` arm64, generates the Xcode project, and packages an unsigned `StrikersVision.ipa`. Run it
from the Actions tab, or push a commit whose message contains `[build]`.

Layout:

- `smstrikers-port/`: the game and the port, shared with the Quest build (`android/` is the Quest
  launcher and is not part of the visionOS app).
- `visionos/include/sv_bridge.h`: the C contract between the pieces below.
- `visionos/xr/`: the XR layer (Compositor Services, ARKit, PlayStation VR2 Sense controllers).
- `visionos/app/`: the SwiftUI app (launcher, settings, log, immersive space).
- `visionos/net/`: the online transports (WebSocket relay and SharePlay) behind
  `smstrikers-port/src/platform/xr_net.cpp`.
- `server/`: the room relay, a Cloudflare Worker.

## The room server

Online play goes through a tiny relay: a Cloudflare Worker with one Durable Object per room code,
which passes each message to everyone in the room. Its source is in [`server/`](server/), with
instructions to run your own copy on Cloudflare's free plan. The address is `kServerHost` in
`smstrikers-port/src/platform/xr_net.cpp`; both players need builds that point at the same server.

## Credits

- [Yannick Suter](https://github.com/yannicksuter) and contributors: the decompilation.
- [`new-coke/strikers`](https://github.com/new-coke/strikers): the native port this is built on.
- [`luisxl15/Strikers-Android-Port`](https://github.com/luisxl15/Strikers-Android-Port) (Luis
  Santos): the Android port.
- [LanceSweatpants](https://github.com/LanceSweatpants): the Quest mixed-reality port
  ([`Strikers-Quest-MR`](https://github.com/LanceSweatpants/Strikers-Quest-MR)) this repository
  starts from: the view modes, the tabletop, the celebrations, online play and its room server.
- [encounter/aurora](https://github.com/encounter/aurora): GX/VI/PAD/CARD over WebGPU and SDL3.
- [JoeyAW/aurora-vr](https://github.com/JoeyAW/aurora-vr) and [JoeyAW/TPVR](https://github.com/JoeyAW/TPVR):
  the external render-pass hand-off that the headset path follows.
- [bylaws/libadrenotools](https://github.com/bylaws/libadrenotools): inherited from the Android port
  (Android only).
- [SDL](https://github.com/libsdl-org/SDL), [Dawn](https://dawn.googlesource.com/dawn) and
  [zstd](https://github.com/facebook/zstd) (reads `.rvz`).
- Button prompt artwork by [Kenney](https://www.kenney.nl) (CC0).

This visionOS port was written by Claude (Anthropic's AI) building on the great work of the projects
above, with the repository owner only contributing ideas on how some things could be adapted.

## License

There is no single license for this repository. This is what applies, as far as it can be backed
up:

- **The Quest additions and this repository's visionOS additions** (the XR layers, the
  celebrations, online play and its room server in `server/`, the launchers, everything under
  `visionos/`, and every other change on top of `luisxl15/Strikers-Android-Port`) are free software
  under the **GNU General Public License, version 3 or (at your option) any later version**. The text
  is in [`LICENSE`](LICENSE). The `.rvz` reader in `smstrikers-port/src/platform/disc.c` follows
  Dolphin's code and is GPL-2.0-or-later, used here under GPLv3; GPLv3 is also what lets it sit in
  one binary with the Apache-2.0 and FreeType-licensed libraries the app links.
- **Third-party code compiled into the app** and the notices its licenses ask for are listed in
  [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).
- **The porting code, tools and docs from `new-coke/strikers`** are offered under CC0 1.0, as that
  project's README states. The text is in
  [`smstrikers-port/LICENSE-CC0.txt`](smstrikers-port/LICENSE-CC0.txt).
- **The reconstructed game code** (the decompilation) is an unofficial reconstruction. It may
  remain subject to third-party rights; no ownership of those is claimed, and none is granted here.
- **ODE physics** (`src/ode`, `include/ode`): ODE's BSD-style license
  ([`LICENSE-BSD.TXT`](smstrikers-port/LICENSE-BSD.TXT), and `src/ode/LICENSE` for what it covers).
  ODE's own headers also offer the LGPL 2.1, whose text is in
  [`LICENSE-LGPL-2.1.txt`](smstrikers-port/LICENSE-LGPL-2.1.txt).
- **`LICENSE-GPL-2.0.txt`** came with the upstream tree, which does not say which files it covers.
  zstd offers the GPLv2 as one of its two options.
- **Bundled libraries**, each under its own file: Aurora (MIT, `extern/aurora/LICENSE`), MusyX (MIT,
  `extern/musyx/LICENSE`), zstd (BSD, `extern/zstd/LICENSE`), and Kenney's button prompts (CC0,
  `assets/input-prompts/LICENSE-Kenney.txt`). SDL3 (zlib), Dawn (BSD 3-Clause) and Aurora's other
  dependencies (abseil, fmt, FreeType, Dear ImGui, libpng, SQLite, xxHash) are downloaded at build
  time, each under its own license.
- **The Android port's own code** (`luisxl15/Strikers-Android-Port`) has no license stated by its
  author. It is included as published there, with credit above.

## Disclaimer

An unofficial project, not affiliated with or endorsed by Nintendo, Next Level Games, Apple, Sony or
Meta. Super Mario Strikers is a trademark of Nintendo; Apple Vision Pro, visionOS and SharePlay are
trademarks of Apple; PlayStation is a trademark of Sony Interactive Entertainment. No game files are
included in this repository or in the app; the game runs only from a disc image of your own, legally
obtained copy.
