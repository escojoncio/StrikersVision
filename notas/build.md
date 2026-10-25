# notas/build.md — build y CI

- CMake `xros` → `libstrikers.a` + `libstrikers_deps.a` (Dawn prebuilt iOS reorientado con vtool, SDL3,
  Aurora sin ventana). XcodeGen `project.yml` (fuentes listadas una a una; Swift 5, visionOS 26).
- `stubs_generated.c` con `PORT_SYM`/`__USER_LABEL_PREFIX__` (Mach-O).
- CI `.github/workflows/visionos.yml`: push a main con `[build]` en el mensaje o workflow_dispatch
  (`scheme`; por defecto StrikersVision + SVProbeMinimal + SVProbeGame), runner macos-26, cachés Dawn/libs/CMake.
- IPA sin firmar (`com.kdt.livecontainer`) → release prerelease `visionos` (se borra y recrea):
  `StrikersVision.ipa`, `Prueba1-Minima.ipa`, `Prueba2-Juego.ipa`. Log de cada build en rama `ci-logs`
  (`visionos.txt`, `scripts/ci-publish-log.sh`).
- Swift solo ve `sv_bridge.h` y `sv_boot_trace.h` (bridging header); para C de `visionos/xr` usar
  `@_silgen_name` o punteros de función.
- `com.apple.developer.group-session` quitado (cuenta gratuita); SharePlay solo con firma de pago.
- Tests: solo unitarios de Aurora (`extern/aurora/tests`, necesitan abseil/gtest/dawn) y `server/test-room.mjs`.
