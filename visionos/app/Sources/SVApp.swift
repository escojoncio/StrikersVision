// StrikersVision: the app. One launcher window (SVLauncher.swift) and one immersive space whose only
// content is a CompositorLayer: the game draws into it with Metal from its own thread (visionos/xr).

import ARKit
import CompositorServices
import SwiftUI

enum SVScene {
    static let launcher = "launcher"
    static let immersive = "game"
}

@main
struct StrikersVisionApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject private var game = SVGame.shared
    @ObservedObject private var settings = SVSettings.shared
    @State private var immersionStyle: any ImmersionStyle = MixedImmersionStyle.mixed

    init() {
        SVPaths.prepare()
        SVLog.start()
        SVNetLog.sink = { SVLog.log($0) }
        SVLog.log("Documents/Strikers: \(SVPaths.home.path)")
        if SVSettings.shared.allowFullImmersion { _immersionStyle = State(initialValue: FullImmersionStyle.full) }
        if let signed = SVEntitlements.signed {
            SVLog.log("signed entitlements: \(signed.keys.sorted().joined(separator: ", "))")
        } else {
            SVLog.log("signed entitlements: could not be read")
        }
    }

    var body: some Scene {
        WindowGroup(id: SVScene.launcher) {
            SVLauncherView()
        }
        .defaultSize(width: 900, height: 700)
        // On the app's phase, not one window's: closing the launcher while the space is open is not
        // going to the background.
        .onChange(of: scenePhase) { _, phase in
            SVLog.log("app phase: \(String(describing: phase))")
            if game.phase == .running { sv_set_paused(phase == .background ? 1 : 0) }
        }
        .onChange(of: settings.allowFullImmersion) { _, allow in
            if allow { immersionStyle = FullImmersionStyle.full } else { immersionStyle = MixedImmersionStyle.mixed }
        }

        ImmersiveSpace(id: SVScene.immersive) {
            CompositorLayer(configuration: SVLayerConfiguration()) { layerRenderer in
                SVRendererHandOff.attach(layerRenderer)
            }
        }
        // Mixed by default: the room stays visible around the big screen and the table. With "Allow
        // full immersion" on, the space opens fully immersive (the stadium views draw everything).
        .immersionStyle(selection: $immersionStyle, in: .mixed, .full)
        // The system's own hands over the game, unless the XR layer asks them hidden (standing in the
        // stadium) or the player turned them off. A Scene modifier: a CompositorLayer has no views.
        .upperLimbVisibility(game.limbsWanted && settings.showHands ? .automatic : .hidden)
    }
}

/// The permissions the XR layer needs, asked up front from the launcher with an explanation, so the
/// system prompts never appear in the middle of the game.
enum SVAuthorization {
    static let askedKey = "sv.authorizationAsked"

    static var asked: Bool { UserDefaults.standard.bool(forKey: askedKey) }

    /// World sensing (head pose, planes for the table), hands, and the PlayStation VR2 Sense
    /// controllers' tracking. A refusal is logged and the game still runs (with what it has).
    static func request() async {
        UserDefaults.standard.set(true, forKey: askedKey)
        var types: [ARKitSession.AuthorizationType] = [.worldSensing]
        if HandTrackingProvider.isSupported { types.append(.handTracking) }
        if AccessoryTrackingProvider.isSupported { types.append(.accessoryTracking) }
        let session = ARKitSession()
        let result = await session.requestAuthorization(for: types)
        for (type, status) in result {
            SVLog.log("authorization \(String(describing: type)): \(String(describing: status))")
        }
    }
}
