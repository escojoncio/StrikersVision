// StrikersVision launch probe: a SwiftUI app with nothing else in it, built from the same Info.plist
// and bundle id as the real app. The launch-probe IPAs isolate why the app is killed at launch:
// SVProbeMinimal (this file alone), SVProbeGame (this file plus the force-loaded game libraries),
// then StrikersVision itself. Every step is written to Documents/arranque.txt (sv_boot_trace.c).
import SwiftUI

@main
struct SVProbeApp: App {
    init() {
        sv_boot_trace("2 probe: Swift app init")
    }

    var body: some Scene {
        WindowGroup {
            VStack(spacing: 20) {
                Text("StrikersVision").font(.largeTitle)
                Text(SVProbeInfo.title).font(.title2)
                Text("If you can read this, this build opens. Next: install the next test IPA.")
            }
            .padding(40)
            .onAppear { sv_boot_trace("6 probe: window on screen") }
        }
    }
}

enum SVProbeInfo {
    static var title: String {
        (Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String) ?? "Launch test"
    }
}
