# Bitácora — StrikersVision

## EMPEZAR AQUÍ — 2026-10-14
- Estado: CI verde (run 38532720700, `ce71f8f`). IPA sin firmar en la release `visionos`
  (`StrikersVision.ipa`, bundle `com.kdt.livecontainer`). **Nunca ejecutada en el visor.**
- Origen: `LanceSweatpants/Strikers-Quest-MR` @ `b9d27d8`. Contrato: `visionos/include/sv_bridge.h`.
- Build: CMake `xros` → `libstrikers.a` + `libstrikers_deps.a` (Dawn prebuilt iOS reorientado con vtool,
  SDL3, Aurora sin ventana); XcodeGen `project.yml` (fuentes listadas una a una); `stubs_generated.c` con
  `PORT_SYM`/`__USER_LABEL_PREFIX__` (Mach-O). Caché: Dawn, libs del juego, árbol CMake (~3 min con caché).
  Target `StrikersVisionBootstrap` solo bajo demanda (dispatch `scheme=`). Firma ad hoc con entitlements.
- XR (`visionos/xr/`): `sv_xr.mm` (todos los PortXr*; bucle Compositor Services en el hilo del juego;
  IOSurface↔Dawn `SharedTextureMemory` + `MTLSharedEvent`; jumbotron por copia GPU con fallback CPU),
  `sv_xr_pad.cpp` (PAD virtual), `SVXRHost.swift` (ARKit, Sense, hápticos), `sv_fx.mm` (celebraciones),
  shaders MSL en `sv_shaders.h` compilados en runtime (CI los valida con `scripts/check-msl.sh`),
  `SVLayerConfiguration.swift` (bgra8Unorm_srgb, depth32Float, layered). Ajustes: `visionos/xr/SETTINGS.md`.
- App (`visionos/app/`): lanzador, ajustes, log, selector de carpeta, ventana `match-controls` junto al
  espacio inmersivo, es/en. Red (`visionos/net/`): relay WebSocket compatible con Quest + SharePlay
  (mensajero fiable, plantilla espacial de mesa, pose de mesa compartida). `xr_net.cpp`: camino Apple.
- 2026-10-14: la IPA con `com.apple.developer.group-session` se cerraba al abrir (firma de cuenta gratuita sin
  ese permiso en el perfil). Quitado de `visionos/app/StrikersVision.entitlements`; SharePlay queda desactivado
  salvo build firmada con cuenta de pago. Seguía cerrándose sin log: la IPA llevaba firma ad hoc con
  entitlements; ahora sale sin firmar (como VisionQuestPro) y la firma la pone el instalador.
- Diagnóstico de arranque: `visionos/app/Native/sv_boot_trace.c` escribe `Documents/arranque.txt` desde un
  constructor de carga (pasos 1-7, stderr redirigido ahí hasta `open_log_early`). Sigue cerrándose al abrir sin
  informe tras quitar `group-session` y la firma ad hoc; causa aún desconocida.
- Limitación: las Personas no se ven mientras se dibuja con Compositor Services (Apple, foros 791591/801420).
- Pendiente (por orden):
  1. Primera prueba en el visor; leer `Documents/Strikers/strikers-log.txt`. Puntos sin verificar: import de
     IOSurface `32RGBA` como RGBA8Unorm en Dawn, orden fence Dawn↔composite, alineación de ojos/pose,
     profundidad reverse-Z, apertura del espacio tras `game_ready`, poses de Sense (`.grip`).
  2. Modo SharePlay con Personas: segundo ImmersiveSpace con RealityView + jumbotron en `LowLevelTexture`
     (sin modos 3D) mientras la sesión sea espacial.
  3. Vídeos THP (sin decodificador en visionOS), autotests de fichero de Quest (wipe/item/sts/dump),
     máscaras por vista (`PortXrEyeViewAllowed`), foveado/amplificación de vértices en el composite.

## Capa XR — revisión adversarial aplicada (2026-10-14)
- `visionos/xr/SVXRHost.swift`: proveedor de accesorios solo con mandos; si `run` falla, reintento con `[world]`; reintento al llegar un renderer nuevo (`sv_xrhost_session_retry`); `session.events` al log; `consume` tras `run` correcto; mano por `inherentChirality` (un mapa para botones y poses); log de primera pose por mano; logs limitados de `queryNextFrame` nil / sin drawables; `endSubmission` solo si se presentó algo; `sv_xrhost_renderer_device/formats`.
- `visionos/xr/sv_xr.mm`: Begin/EndAccess dentro de scopes de error de Dawn (fallo → CPU o 3D desactivado); librería MSL compilada una vez (`sv_shader_library`, compartida con sv_fx) y pipelines + fx preparados al recibir el renderer; fallback que limpia color+profundidad; evento compartido señalado desde CPU si el command buffer falla; aviso si el renderer sigue en pausa >5 s; `dawn_scoped` con `shared_ptr`.
- Ajustes en vivo sin `setenv` (tabla `sv_setting_override` / `sv_setting_get` en `sv_game.mm`); FOVEATION leído de UserDefaults.
- `sv_game.mm`: log abierto antes del hilo (dup2 a fd 2, `STRIKERS_LOG=0` para que config.c no lo trunque), `NSSetUncaughtExceptionHandler`, QoS user-interactive, callback `game_ready` (añadido al final de `SVAppCallbacks`).
- App: el espacio inmersivo se abre al llegar `game_ready` ("Cargando el juego…"); al terminar el juego se cierra y el motivo queda en el lanzador; sin permiso de hand tracking.
- `crashlog.c`: SIGTRAP. CI: `scripts/check-msl.sh` compila `kSvShadersMsl` con `xcrun -sdk xros metal`.
- Pendiente: probar en el visor (import IOSurface en Dawn, orden de fences, poses de ojos, profundidad reverse-Z).
