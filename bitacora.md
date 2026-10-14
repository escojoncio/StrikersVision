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
  informe tras quitar `group-session` y la firma ad hoc; causa aún desconocida (visionOS 27.2; ni
  `arranque.txt` se crea → muere antes de cualquier constructor). Sondas en cada build: `SVProbeMinimal`
  (`Prueba1-Minima.ipa`) y `SVProbeGame` (`Prueba2-Juego.ipa`), `visionos/probe/`.
- **Causa del cierre al abrir (resuelta en código, pendiente de probar):** constructores estáticos del juego
  (AudioLoader `Config`, `SkillTweaks`, gcmemcard) llaman a `nlMalloc` → `nlInitMemory` → `DVDInit` al cargar,
  antes de que la app ponga `STRIKERS_HOME`; sin disco → `port_fatal` → `exit(1)` silencioso (Prueba1 abría,
  Prueba2 no). Arreglo: `dvd.c` difiere `DVDInit` en `SV_VISIONOS` sin `STRIKERS_HOME` (`PortDVDInitDeferred`,
  llamado en el hilo del juego antes de `strikers_main`); `fatal.c` → `sv_game_fatal` (sv_game.mm) termina solo
  el hilo del juego y avisa a la app (`game_ended`). `PortConfigLoad` corre ahora en ese DVDInit diferido.
- Primera prueba con disco (visionOS 27.2): la app abre, el juego arranca, XR a 87 fps/juego 60 fps, Sense
  detectados y con pose, modo Tabletop activo. Crash al entrar en la escena 35 (saving_loading.fen):
  SIGBUS en `___chkstk_darwin` desde `nlFont::DrawString` → clang convertía `nlStrLen<unsigned short>` en
  `wcslen` (wchar 16 bits por `-fshort-wchar`, wcslen del sistema cuenta 32 bits) → alloca gigante.
  Arreglo: `-fno-builtin-wcslen` en `port_flags` (`smstrikers-port/CMakeLists.txt`) + la CI falla si la app
  importa `_wcslen`. (`wmemchr` lo importa solo Aurora, con wchar de 32 bits: correcto.)
- Segunda prueba (12:16, log `Strikers_2.zip`): partido completo jugable, celebraciones, 82-84 fps display / 60 juego. Tres fallos
  gráficos reportados: (a) el slider "Stadium size" no hacía nada → el usuario estaba en Tabletop · Living Room, donde no hay
  estadio (ese ajuste solo escala las vistas Stadium). Añadido `STRIKERS_XR_TABLE_SCALE` (en vivo, 0.25..4.0 × 0.032 m/unidad,
  `apply_table_scale_setting` en `take_settings`/`PortXrInit`; slider "Table size" en SVSettings.swift) y aviso en el log cuando
  se cambia el del estadio en una vista de sala. (b) Manos y mandos se veían por encima del 3D → `upperLimbVisibility(.automatic)`
  por "Show my hands" activo por defecto; ahora apagado por defecto (clave nueva `sv.showHands2`, `.hidden`). (c) Texturas y
  modelos cortados: sin pista en el log (culled 0/frame, sin errores de Dawn); pendiente de captura del visor. Sospechas por
  orden: viewport/scissor de Aurora en el pase de ojo (`set_offscreen_uses_native_logical_size`), decode-once (`STRIKERS_XR_DECODE`),
  recorte `uvRect` del HUD sobre la mesa.
- Objetos reales cercanos "delante" del 3D al acercarse (no pasa en Demeo): visionOS compara la profundidad que la app
  escribe con la del entorno; el compuesto escribía una ESTIMACIÓN (plano de la mesa / suelo), así que jugadores, porterías
  y todo lo que sobresale del plano quedaba "detrás" de la mesa real. Arreglo: profundidad real del pase de ojo exportada.
  Aurora: `ExternalPassTarget::depthSnapshot` (recording.cpp: `offscreenDepthSnapshot`, el snapshot R32Float de
  `resolve_pass({.depth=true})` va a esa vista; depth Store en vez de Discard; el depth MSAA lleva TextureBinding).
  sv_xr.mm: `g.eyeDepths[slot][eye]` (IOSurface `OneComponent32Float` → Dawn R32Float, misma vida de accesos/fences que los
  de color; `g.depthExport`, `stop_depth_export` si Dawn lo rechaza → vuelve a la estimación), composite: textura 1 +
  `EyeUniforms.proj` (P22, P23, near 0.05, far 1500) y en `eye_fragment` z_view = -nf/(d(f-n)+n) → depth del drawable.
  Línea de log: `3D modes ready: ... depth exported|estimated`. **Pendiente de probar en el visor.**
- Limitación: las Personas no se ven mientras se dibuja con Compositor Services (Apple, foros 791591/801420).
- Pendiente (por orden):
  1. Texturas/modelos cortados (ver arriba): pedir captura, probar con `STRIKERS_XR_DECODE=0` y `STRIKERS_XR_CULL=0`.
     Verificado en el visor: IOSurface en Dawn, fences, poses, apertura del espacio, Sense. Hápticos fallan
     (gamecontrollerd.haptics XPC 4097): pendiente.
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
