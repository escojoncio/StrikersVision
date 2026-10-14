# Bitácora — StrikersVision

## EMPEZAR AQUÍ — 2026-10-14
- Repo inicializado con `LanceSweatpants/Strikers-Quest-MR` @ `b9d27d8` (árbol completo, sin historial).
- Contrato entre piezas: `visionos/include/sv_bridge.h`. Arquitectura: `CLAUDE.md`.
- Trabajo en paralelo (sin build aún):
  - Build/CI: CMake para `xros`, Dawn (Metal) para visionOS, SDL3, juego como `.a`, XcodeGen, workflow.
  - Capa XR: `visionos/xr/` (Compositor Services, ARKit, Sense, modos de vista, celebraciones).
  - App + red: `visionos/app/` (SwiftUI, ajustes, log, SharePlay con Personas), `visionos/net/`.

## Capa XR — revisión adversarial aplicada (2026-10-14)
- `visionos/xr/SVXRHost.swift`: proveedor de accesorios solo con mandos; si `run` falla, reintento con `[world]`; reintento al llegar un renderer nuevo (`sv_xrhost_session_retry`); `session.events` al log; `consume` tras `run` correcto; mano por `inherentChirality` (un mapa para botones y poses); log de primera pose por mano; logs limitados de `queryNextFrame` nil / sin drawables; `endSubmission` solo si se presentó algo; `sv_xrhost_renderer_device/formats`.
- `visionos/xr/sv_xr.mm`: Begin/EndAccess dentro de scopes de error de Dawn (fallo → CPU o 3D desactivado); librería MSL compilada una vez (`sv_shader_library`, compartida con sv_fx) y pipelines + fx preparados al recibir el renderer; fallback que limpia color+profundidad; evento compartido señalado desde CPU si el command buffer falla; aviso si el renderer sigue en pausa >5 s; `dawn_scoped` con `shared_ptr`.
- Ajustes en vivo sin `setenv` (tabla `sv_setting_override` / `sv_setting_get` en `sv_game.mm`); FOVEATION leído de UserDefaults.
- `sv_game.mm`: log abierto antes del hilo (dup2 a fd 2, `STRIKERS_LOG=0` para que config.c no lo trunque), `NSSetUncaughtExceptionHandler`, QoS user-interactive, callback `game_ready` (añadido al final de `SVAppCallbacks`).
- App: el espacio inmersivo se abre al llegar `game_ready` ("Cargando el juego…"); al terminar el juego se cierra y el motivo queda en el lanzador; sin permiso de hand tracking.
- `crashlog.c`: SIGTRAP. CI: `scripts/check-msl.sh` compila `kSvShadersMsl` con `xcrun -sdk xros metal`.
- Pendiente: probar en el visor (import IOSurface en Dawn, orden de fences, poses de ojos, profundidad reverse-Z).
