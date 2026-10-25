# Bitácora — StrikersVision

## Last done
- 2026-10-25 (sin compilar, pendiente de build): tirones → hilo del juego ya no gira entre ticks
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
- Celebración de gol: causa NO encontrada. Probar en visor: botón "Test the goal celebration" (FX_TEST) vs
  gol real, y `STRIKERS_XR_DECODE=0`. Pedir captura.
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
