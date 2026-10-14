# Bitácora — StrikersVision

## EMPEZAR AQUÍ — 2026-10-14
- Repo inicializado con `LanceSweatpants/Strikers-Quest-MR` @ `b9d27d8` (árbol completo, sin historial).
- Contrato entre piezas: `visionos/include/sv_bridge.h`. Arquitectura: `CLAUDE.md`.
- Trabajo en paralelo (sin build aún):
  - Build/CI: CMake para `xros`, Dawn (Metal) para visionOS, SDL3, juego como `.a`, XcodeGen, workflow.
  - Capa XR: `visionos/xr/` (Compositor Services, ARKit, Sense, modos de vista, celebraciones).
  - App + red: `visionos/app/` (SwiftUI, ajustes, log, SharePlay con Personas), `visionos/net/`.
