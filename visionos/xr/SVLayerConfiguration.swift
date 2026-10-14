// StrikersVision: the CompositorLayer setup of the immersive space (visionos/app SVApp.swift uses
// `CompositorLayer(configuration: SVLayerConfiguration())`). The composite in sv_xr.mm draws into
// what is chosen here:
//   - colour bgra8Unorm_srgb: the composite reads the game's pictures through sRGB views, so the
//     hardware decodes and re-encodes them byte-exact (the effects' pipelines take the format from
//     the drawable, whatever it is);
//   - depth depth32Float, reverse-Z (the drawable's projection), written for the system's
//     reprojection;
//   - layered when offered (one 2-slice texture, a render pass per slice), else dedicated (a texture
//     per view); sv_xr.mm handles shared too, as each view's viewport;
//   - foveation off unless STRIKERS_XR_FOVEATION=1: the composite does not draw through a
//     rasterization rate map, and a pass that ignores a foveated drawable's map shows a distorted
//     picture.
// Every choice is logged into the game log.

import CompositorServices
import Foundation
import Metal
import SwiftUI

struct SVLayerConfiguration: CompositorLayerConfiguration {
    /// The saved setting (SVSettings keeps every STRIKERS_* key in UserDefaults under this prefix).
    /// Not the environment: the game thread may be changing it while this runs.
    private static func setting(_ name: String) -> String? {
        guard let v = UserDefaults.standard.string(forKey: "sv.setting." + name), !v.isEmpty else { return nil }
        return v
    }

    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float

        let wantFoveation = Self.setting("STRIKERS_XR_FOVEATION") == "1"
        let foveate = wantFoveation && capabilities.supportsFoveation
        configuration.isFoveationEnabled = foveate

        // The supported layouts depend on whether foveation is on.
        let layouts = capabilities.supportedLayouts(options: foveate ? [.foveationEnabled] : [])
        if layouts.contains(.layered) {
            configuration.layout = .layered
        } else if layouts.contains(.dedicated) {
            configuration.layout = .dedicated
        } else {
            configuration.layout = .shared
        }

        let layoutName: String
        switch configuration.layout {
        case .layered: layoutName = "layered"
        case .dedicated: layoutName = "dedicated"
        default: layoutName = "shared"
        }
        let line = "[xr] cp: layer configuration: colour bgra8Unorm_srgb (\(MTLPixelFormat.bgra8Unorm_srgb.rawValue)), "
            + "depth depth32Float, layout \(layoutName), foveation \(foveate) "
            + "(asked \(wantFoveation), supported \(capabilities.supportsFoveation))"
        // stderr is the game log (the app's bridging header does not see sv_xr.h's sv_log_line); the
        // log viewer follows that file.
        NSLog("%@", line)
        fputs(line + "\n", stderr)
    }
}
