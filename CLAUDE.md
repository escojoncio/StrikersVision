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
