# Headset layer settings (visionos/xr)

Every setting is an environment variable. The app puts them into the environment before
`sv_game_start()`; the ones marked **live** also take effect mid-game through
`sv_setting_changed(key, value)` (the XR layer also `setenv`s every key it receives, so code that
reads the variable later sees the new value). Unknown keys are ignored; an empty value means
"back to the default". Out-of-range values are clamped.

| Key | Values | Default | Live | What it does |
|---|---|---|---|---|
| `STRIKERS_XR_START_VIEW` | `0`..`3` | `0` | no | View mode at start: 0 Jumbotron · Living Room, 1 Jumbotron · Stadium, 2 Tabletop · Living Room, 3 Tabletop · Stadium (`SV_VIEW_*`). Mid-game use `sv_set_view_mode`. |
| `STRIKERS_XR_SCREEN_SIZE` | `0`, `1`, `2` | `1` | yes | Jumbotron screen preset: 0 = 2.4 m wide at 2.5 m, 1 = 3.6 m at 3.0 m, 2 = 6.0 m at 4.0 m (single click of the right stick cycles it too). |
| `STRIKERS_XR_SCREEN_CURVE` | `0`..`1` | `0` | yes | Jumbotron curvature: 0 flat, 1 = curved around you (radius = viewing distance). Values between bend it partly. |
| `STRIKERS_XR_RES_SCALE` | `1.0`..`3.0` | `2.0` | no | The game's own render size for the flat picture (2 ≈ 1600×900). Read by main.cpp at start. |
| `STRIKERS_XR_EYE_SCALE` | `0.3`..`1.5` | `1.0` | no | Size of each eye image in the 3D modes, relative to the headset's drawable size. |
| `STRIKERS_XR_MSAA` | `1`, `4` | `4` | yes | Multisampling of the 3D eye passes (both layers). |
| `STRIKERS_XR_MSAA_TABLE` | `1`, `4` | = `STRIKERS_XR_MSAA` | yes | Overrides it for frames that draw only the table. |
| `STRIKERS_XR_MSAA_STADIUM` | `1`, `4` | = `STRIKERS_XR_MSAA` | yes | Overrides it for frames that draw the life-size stadium. |
| `STRIKERS_XR_ANISO` | `1`, `2`, `4`, `8`, `16` | `8` | yes | Anisotropic filtering of mipmapped textures (the pitch at a slant). |
| `STRIKERS_XR_DECODE` | `0`, `1`, `2` | `2` | yes | Decode-once vertex path: 0 off, 1 players only, 2 everything eligible. |
| `STRIKERS_XR_HUD_ONLY` | `0`, `1` | `1` | yes | Tabletop during live play: the flat picture draws only the HUD (score bar / Super Strike meter over the table). 0 keeps the full broadcast view. |
| `STRIKERS_XR_CHARSCALE` | `1.0`..`3.0` | `1.5` | yes | Tabletop only: players drawn this much bigger. |
| `STRIKERS_XR_TABLE_SCALE` | `0.25`..`4.0` | `1.0` | yes | Tabletop field size, times the default 0.032 m per game unit (a 1.3 m pitch). Applied when it changes; the hands (both grips) and the left stick with the left grip held also resize the table in play. |
| `STRIKERS_XR_STADIUM_SCALE` | `0.1`..`2.0` | `1.0` | yes | Life-size stadium scale (1 = 1 m per game unit). Stadium views only; in the Living Room views there is no stadium (a log line says so). |
| `STRIKERS_XR_LOD` | `0`..`32` (pixels) | `0` | yes | Stadium economy: stadium objects smaller than this in an eye are skipped (0 = off). |
| `STRIKERS_XR_SKIP_TALL_FIELD` | `0`, `1` | `0` | yes | Tabletop · Stadium: leave out the life-size goals, benches and fence (the table carries the game). |
| `STRIKERS_XR_CULL` | `0`, `1` | `1` | no | Per-view culling of stadium objects (xr_layers.cpp reads it once). |
| `STRIKERS_XR_HAPTICS` | `0`..`100` (%) | `100` | yes | Strength of the game's rumble on the Sense controllers / gamepad. 0 = off. |
| `STRIKERS_XR_DEADZONE` | `0.0`..`0.4` | `0.12` | yes | Stick dead zone (both sticks). |
| `STRIKERS_XR_PS_BUTTON` | `0`, `1` | `0` | yes | PS (home) button, when visionOS lets it through: 0 ignored, 1 = Start (pause). It never leaves the game. |
| `STRIKERS_XR_DEPTH` | `0`, `1`, `2` | `2` | yes | Depth the system gets in the drawable (it hides the game wherever a real object is nearer than the written depth). 0 = off: depth far everywhere (the stadium is never cut by the room). 1 = near only: the real depth where it is closer than 1.2 m from the eye, else far (the table reprojects well). 2 = full real depth (default; the system reprojects best, and real objects hide the game where nearer). Internally the passes always test against the real depth; a small depth-only pass after them rewrites the drawable's depth for 0 and 1. Old configs: `1` used to mean full depth and now means near only. |
| `STRIKERS_XR_STATS_SECONDS` | `1`..`60` | `5` | yes | Interval of the `stats:` lines in the log. |
| `STRIKERS_XR_FOVEATION` | `0`, `1` | `0` | no | Read by `SVLayerConfiguration` (CompositorLayer setup). Kept off: the compositor does not draw through a rasterization rate map. |
| `STRIKERS_XR_FX_MAXIDX` | `10000`..`4000000` | `4000000` | yes | Celebrations: cap on 3D-piece mesh indices drawn per eye (sv_fx.mm). Over the cap the far / landed pieces are dropped first, then the near ones; logged once per celebration. |
| `STRIKERS_XR_FX_NOMESH` | `0`, `1` | `0` | yes | Celebrations, diagnostic: 1 = no 3D-piece meshes (pieces are drawn as flat quads). |
| `STRIKERS_XR_FX_TEST` | `<n>:<commands>` | unset | yes | Celebration self-test (sv_fx.mm). A new `<n>` runs the `;`-separated commands once: `goal <class> [super] [hyper] [own]` (eCharacterClass 0 Birdo .. 11 Yoshi, -1 neutral), `replay`, `end`, `skip`, `live`, `state` (logs what is alive). Goes through `PortXrGoal` / `PortXrCelebrationPhase`, so the buzz and the replay jumbotron follow. The match controls window's "Test the goal celebration" sends it. |

## Game-side variables the app also sets

Not read by the XR layer; listed so the names stay in one place. `STRIKERS_AUDIO_VOLUME` (`0`..`2`,
read once), `STRIKERS_LANGUAGE` (`english` … `japanese` or `0`..`5`, read at boot),
`STRIKERS_OVERLAY` (presence flag), `STRIKERS_LOG_SCENES` / `_MATCH` / `_EVENTS` / `_AUDIO` (presence
flags), `STRIKERS_PAD_RUMBLE=0` (with haptics at 0), and the paths `STRIKERS_HOME`, `STRIKERS_LOG`,
`STRIKERS_USER_DIR`, `STRIKERS_CACHE_DIR`, `STRIKERS_DATA`, plus `STRIKERS_NO_MESSAGEBOX=1` and
`STRIKERS_NO_MOVIES=1`. None of these apply mid-game.

## View modes

`sv_set_view_mode(SV_VIEW_*)` (any thread) and the stick double-clicks switch modes; every change is
reported through `SVAppCallbacks.view_mode_changed`. Stadium modes call
`set_upper_limbs_visible(0)`; Living Room modes call `set_upper_limbs_visible(1)`.

## Log lines worth knowing

- `[xr] SV_XR build=...` first line of the headset layer.
- `[xr] stats: display ... fps, game ... fps, ...` every `STRIKERS_XR_STATS_SECONDS`.
- `[xr] cp: ...` Compositor Services; `[xr] arkit: ...`; `[xr] dawn: ...`; `[xr] input: ...`; `[xr] fx: ...`.
