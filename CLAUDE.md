# CLAUDE.md

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.

# StrikersVision

Port de **Strikers-Quest-MR** (Super Mario Strikers de GameCube, decompilación compilada de forma
nativa) al Apple Vision Pro. No es emulación: el mismo código fuente se compila para visionOS arm64.

Origen: `LanceSweatpants/Strikers-Quest-MR` @ `b9d27d88632d92a32580798b254e55507650c410`
(fork de `luisxl15/Strikers-Android-Port`, basado en `new-coke/strikers`).

## Arquitectura
| Pieza | Quest (origen) | Vision Pro (este repo) |
|---|---|---|
| Juego | `smstrikers-port/` (C/C++) para Android arm64 | el mismo, biblioteca estática para `xros` arm64 |
| GX/VI/PAD | Aurora sobre WebGPU (Dawn → Vulkan) | Aurora sobre WebGPU (Dawn → **Metal**) |
| SDL3 | ventana/audio/mandos Android | audio y mandos (sin ventana: Aurora sin superficie) |
| Capa XR | `src/platform/xr_quest.cpp` (OpenXR + Vulkan) | `visionos/xr/` (Compositor Services + ARKit + Metal) |
| Imagen 3D | VkImage opaque-fd importada en Dawn + SyncFD | IOSurface importada en Dawn (`SharedTextureMemory`) + `MTLSharedEvent` |
| Mandos | Touch | PlayStation VR2 Sense (GameController + `AccessoryTrackingProvider`) |
| Panel | `LobbyActivity.java` | `visionos/app/` (SwiftUI) |
| Online | `NetWs.java` (WebSocket al relay de `server/`) | `visionos/net/` (WebSocket + SharePlay) |

Contrato entre piezas: `visionos/include/sv_bridge.h` (solo se amplía, nunca se cambia una firma).

## Datos prácticos
- Ajustes: pantalla de ajustes de la app; llegan al juego como variables `STRIKERS_*` antes de
  `sv_game_start`, o en vivo con `sv_setting_changed`.
- Log: `<Documents>/Strikers/strikers-log.txt`, visible y compartible desde la app.
- Disco del juego: `.iso/.gcm/.ciso/.gcz/.rvz` en `Documents/Strikers` (app Archivos) o en una carpeta
  elegida con el selector del sistema (marcador con ámbito de seguridad).
- CI: `.github/workflows/visionos.yml`; log de cada build en la rama `ci-logs`.
- Estado y pendientes: `bitacora.md`.
