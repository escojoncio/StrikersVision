# notas/xr.md — capa XR (visionos/xr/) y app (visionos/app/Sources/)

## Piezas
- `sv_xr.mm`: todos los PortXr*. Hilo `sv-display` (`display_thread_main`, QoS user-interactive):
  `sv_xrhost_frame_begin` sin candado → `display_encode` con `g_lock` (`std::recursive_mutex`).
  Compone el último par TERMINADO (`select_shown_eyes`/`select_shown_screen`, `image_done`, 4 ranuras,
  `pick_eye_slot` nunca la mostrada) con su pose (`shownPose`) y su `anchorId`.
- Hilo del juego: `PortXrBeginFrame` tick 60 Hz por reloj de pared; fuera de tick duerme (`sleep_ns`,
  `mach_wait_until`) hasta el siguiente tick − 1 ms sin candado; `sync_input`/`predict_eyes` solo en tick.
  Lockstep online: si `xrnet::step` rechaza, duerme 0,5 ms.
- `SVXRHost.swift`: ARKit (`queryDeviceAnchor`), anillo de anclas 256 bajo `lock` (NSLock, secciones hoja;
  orden g_lock → lock), Sense/gamepad, manos (`HandTrackingProvider`, pellizco 18/35 mm), hápticos.
- IOSurface↔Dawn `SharedTextureMemory` + `MTLSharedEvent`. Profundidad exportada como RGBA8 empaquetado
  (Dawn rechaza R32Float `4c303066`); MSL desempaqueta con `as_type<float>`; z_view = -nf/(d(f-n)+n).
- `STRIKERS_XR_DEPTH`: 0 = lejos, 1 = real solo <1,2 m (`kNearDepthCutoff`), 2 = real (por defecto).
  0/1 reescriben la profundidad del drawable (`eye_depth_fixup`) → reproyección del sistema incorrecta.
  Con 2 la sala real ocluye el juego si está más cerca.
- `sv_fx.mm`: celebraciones, anillo de 3 buffers + semáforo, dentro del pase "SV view".
- Shaders MSL en `sv_shaders.h`, compilados en runtime (`sv_shader_library`); CI: `scripts/check-msl.sh`.
- Colocación: `place_screen`, `place_command` (`STRIKERS_XR_PLACE`), agarre izquierdo Sense.

## App
- Escenas `WindowGroup`: `launcher` y `match-controls`; `ImmersiveSpace`. Espacio se abre al `game_ready`.
- En partida los paneles están ocultos (`SVGame.panelsVisible=false` al `.opened`); Select (gamepad
  `buttonOptions`, Sense Options/Share) mantenido 3 s → `sv_game_panels_toggle` → callback
  `panels_toggle` (último campo de `SVAppCallbacks`) → main → toggle. Sense: toque <0,4 s = Start
  (pulso 2 muestras); ≥0,4 s nunca Start. Reabrir la app desde Home con espacio abierto muestra paneles.
- `SVLauncherWindows`: contadores open/controlsOpen + flags `launcherOpening`/`controlsOpening`
  (se limpian en onAppear/onDisappear, a los 2 s y en scenePhase .active).
- Audio: SDL3 (`audio_out.cpp` PortAudioStart); `SVGame.anchorAudioToPlayer()` →
  `AVAudioSession.setIntendedSpatialExperience(.headTracked(soundStageSize: .large, anchoringStrategy: .front))`
  en game_ready, al abrir el espacio (+3 s) y en scenePhase .active con espacio abierto.
- Fin del juego: `SVApp` `.onChange(of: game.phase)` `.ended` → `dismissImmersiveSpace` (además de los
  handlers del launcher/controls). Salir → `SVGame.quitApp` → `exit(0)`.
- Ajustes: `SVSettings.swift` → variables `STRIKERS_*` antes de `sv_game_start` o `sv_setting_changed`.

## Aurora (smstrikers-port/extern/aurora)
- Decode-once: clave en `command_processor.cpp` (~716-736) hashea bytes de vértice + puntero/tamaño/stride
  de arrays indexados, no su contenido; se vacía en INVL_VC y fin de frame. Diagnóstico: `STRIKERS_XR_DECODE=0`.
- Copias EFB a mitad de pase de ojo: `resolve_pass_into` (recording.cpp) sella con Store; la continuación
  hereda store ops y `externalTarget`.

## Gotchas
- `-fno-builtin-wcslen` obligatorio (`-fshort-wchar`); CI falla si la app importa `_wcslen`.
- Constructores estáticos llaman `nlMalloc` → `DVDInit` diferido (`PortDVDInitDeferred`).
- Las Personas no se ven con Compositor Services. Hápticos Sense: XPC 4097.
