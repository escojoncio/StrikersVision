// StrikersVision: TEMPORARY. The CompositorLayer configuration the app's immersive space uses, until
// visionos/xr/SVLayerConfiguration.swift exists: Compositor Services' defaults (layered, colour
// bgra8Unorm_srgb, depth depth32Float).
import CompositorServices

struct SVLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
        configuration.isFoveationEnabled = false
    }
}
