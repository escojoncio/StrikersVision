# Bitácora — StrikersVision

## Last done
- 2026-10-25: `.claude/agents/coder.md`: maxTurns → 70
- 2026-10-25: `CLAUDE.md` línea 1: regla de cumplir siempre el rol asignado.
- 2026-10-25: `CLAUDE.md` (Sub-agent setup): coder maxTurns 40 → 70.
- 2026-10-25 (CI verde run 40233267657, pendiente de visor): cuadros de tile con colores de sv_fx en celebraciones (solo Apple).
  Hipótesis unverified: presión del parameter buffer del tiler por varyings de `MeshOut`. Cambios: `MeshOut`
  sin c1/c2/params (fragment lee `FxInstance` por `inst [[flat]]`, `setFragmentBuffer` índice 1 mismo offset);
  tope de índices por ojo `STRIKERS_XR_FX_MAXIDX` (300000, far primero, log `fx: index budget`);
  `STRIKERS_XR_FX_NOMESH`; posiciones no finitas fuera del clip; `MTLMathModeSafe` en `sv_shader_library`
  (toda nuestra librería). Archivos: `sv_shaders.h`, `sv_fx.mm`, `sv_xr.mm`, `xr/SETTINGS.md`.
  UI: Ajustes > Diagnóstico ("NOMESH" toggle, tope de índices choice; `SVSettings.swift`). Si persiste: NOMESH=1 y MAXIDX=50000; si sigue, no es carga → NaN/depth(any).
- 2026-10-25 (visor: framerate OK): tirones → hilo del juego ya no gira entre ticks
  (`sv_xr.mm` PortXrBeginFrame duerme; `sync_input`/`predict_eyes` solo en tick; anillo de anclas 64→256 con
  candado en `SVXRHost.swift`; contador `no device anchor N` en la línea `present:`).
- 2026-10-25: `STRIKERS_XR_DEPTH` por defecto 2 (`SVSettings.swift`, `sv_xr.mm`, `xr/SETTINGS.md`): 0/1 rompen
  la reproyección; el usuario confirma que solo 2 se ve bien.
- 2026-10-25: audio anclado a la cabeza (`SVGame.anchorAudioToPlayer`); paneles ocultos en partida y toggle
  con Select 3 s (`sv_bridge.h` `panels_toggle`, `sv_game.mm`, `SVXRHost.swift`, `SVApp/SVLauncher/
  SVImmersiveControls.swift`). Detalle en notas/xr.md.
- 2026-10-20: prueba de modos de profundidad y diagnóstico `present:`; 2026-10-18: hilo `sv-display` OK.

## Current state
- Funciona en visor (log 2026-10-22, pre-cambios): partido completo, display 94-99 fps / juego 60,
  `depth exported`, Sense con pose, audio sin underruns.
- Reportado 2026-10-25 en la build previa: celebración de gol mal renderizada; tirones; solo bien con
  profundidad 2; audio saliendo de los paneles; paneles visibles en partida.
- Pendiente de probar en la build siguiente: los cuatro arreglos de arriba (nombres de botón Sense
  `Button Options`/`Button Share` y firma exacta de `headTracked(soundStageSize:anchoringStrategy:)` unverified).
- Celebración de gol: cuadros con colores de sv_fx, solo Apple (Quest bien); ver Last done.
- Pendiente: hápticos Sense (XPC 4097), SharePlay con Personas, vídeos THP, autotests de Quest.

## Decisions
- La lógica del juego se queda a 60 Hz fijos (no 45/50); sin interpolación de estado.
- `sv_bridge.h` solo se amplía al final de los structs.
- IPA sin firmar; la firma la pone el instalador.

## Files in play
- visionos/xr/sv_xr.mm, visionos/xr/SVXRHost.swift, visionos/xr/sv_game.mm, visionos/xr/sv_xr.h
- visionos/include/sv_bridge.h, visionos/xr/SETTINGS.md
- visionos/app/Sources/{SVApp,SVGame,SVLauncher,SVImmersiveControls,SVSettings}.swift
- smstrikers-port/extern/aurora/lib/gx/command_processor.cpp (decode-once, sospecha celebración)

## Module index
- notas/xr.md: capa XR, hilos, profundidad, app/paneles/audio, Aurora decode-once, gotchas.
- notas/build.md: CMake/XcodeGen, CI, release `visionos`, bridging header, tests.
