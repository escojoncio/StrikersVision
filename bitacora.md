# Bitácora — StrikersVision

## Last done
- 2026-10-25: `.claude/agents/coder.md`: maxTurns → 70
- 2026-10-25: `CLAUDE.md` línea 1: regla de cumplir siempre el rol asignado.
- 2026-10-25: `CLAUDE.md` (Sub-agent setup): coder maxTurns 40 → 70.
- 2026-11-10 (pendiente de build y visor): el botón "Probar celebración" (FX_TEST → `PortXrGoal` +
  `PortXrCelebrationPhase`, sin Wiper/ScreenGrab) REPRODUCE los cuadrados → la teoría del wipe es probablemente
  errónea. Nueva hipótesis (unverified): el destello del gol (`add_flash` `sv_fx.mm`, quad 3 m a 0,3 m, shape 16,
  color del equipo: Toad rojo, otros amarillo) falla el test de profundidad contra la profundidad reconstruida
  gruesa (RGBA8, texel más cercano) → bloques con aristas escalonadas; negro = donde no se dibuja. Fix: el flash
  sale de los buckets y se dibuja con `g.always` (sin test ni escritura de profundidad), slot propio
  `inst[count+0]` (tope kMaxParticles-3); ajuste Diagnóstico `STRIKERS_XR_FX_FLASH` (0 = sin destello; si los
  cuadrados persisten con 0, el flash no es la causa); log `fx: flash ...` (máx 20). Otros candidatos con la misma
  debilidad: sparks/cohetes `testNoWrite`, piezas `testWrite`, occluders de mesa `write_occluders`.
  Shape 16 no se tocó (Quest emite `c1*a, a*(1-glow)` a propósito).
- 2026-11-10: build de diagnóstico del wipe (solo afecta a goles REALES, no al botón de prueba): log `glx_XrGrabLog` en `glPlat.cpp` (frame
  del grab + 3 siguientes: hudOnly, wipeActive, layers, eyeSend, vistas de mundo dibujadas, tamaños), log de
  `present_source` en `sv_xr.mm copy_screen_gpu` (`PortXrNoteGrab`), puente `PortXrDiagFlag(0|1)`
  (`xr_quest.h/.cpp`, `sv_xr.mm`, `sv_bootstrap_stubs.c`). Ajustes Diagnóstico: `STRIKERS_XR_NOWIPE`
  (`Wiper.cpp DoWipe` retorna como un cut: bypass del artefacto) y `STRIKERS_XR_GRABNOCLEAR`
  (`glxTarget.cpp` GXCopyTex clear=false). Leer: líneas de grab en strikers-log.txt + probar ambos toggles.
  Pendiente menor: `g_ForceDoubleBallTransition` no se resetea con NOWIPE. Sin leer aún: TransitionSequence.cpp.
- 2026-10-26 (CI verde run 40387474459): fix `xrHudOnly` exige `ScreenTransitionManager::s_pInstance->
  m_pActiveTransition == NULL` (`glPlat.cpp`, include línea 34). No resolvió los cuadros. Cosmético: durante el
  wipe el panel plano sigue recortado a la cinta del marcador (`PortXrHudOnlyNormalView` sigue a 1).
- 2026-10-26: sv_fx descartado como causa (NOMESH=1 y tope 50000 no cambiaron nada). Se quedan `MeshOut`
  reducido, guarda NaN y ajustes Diagnóstico NOMESH/tope; tope por defecto 4000000 (sin límite);
  `MTLMathModeSafe` revertido.
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
- Celebración de gol: ver Last done (wipe + HUD-only).
- Pendiente: hápticos Sense (XPC 4097), SharePlay con Personas, vídeos THP, autotests de Quest.

## Decisions
- La lógica del juego se queda a 60 Hz fijos (no 45/50); sin interpolación de estado.
- `sv_bridge.h` solo se amplía al final de los structs.
- IPA sin firmar; la firma la pone el instalador.

## Files in play
- visionos/xr/sv_xr.mm, visionos/xr/SVXRHost.swift, visionos/xr/sv_game.mm, visionos/xr/sv_xr.h
- visionos/include/sv_bridge.h, visionos/xr/SETTINGS.md
- visionos/app/Sources/{SVApp,SVGame,SVLauncher,SVImmersiveControls,SVSettings}.swift
- smstrikers-port/src/NL/gl/glPlat.cpp (xrHudOnly), visionos/xr/sv_fx.mm

## Module index
- notas/xr.md: capa XR, hilos, profundidad, app/paneles/audio, Aurora decode-once, gotchas.
- notas/build.md: CMake/XcodeGen, CI, release `visionos`, bridging header, tests.
