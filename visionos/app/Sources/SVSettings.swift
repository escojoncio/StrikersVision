// StrikersVision: the settings screen. Every option the headset needs is a control here (nothing to
// edit in a text file); each one is a STRIKERS_* variable, saved in UserDefaults, put into the
// environment before sv_game_start, and also sent with sv_setting_changed when changed mid-game
// (the XR layer applies the ones it can live and ignores the rest until the next launch).
//
// Names and ranges follow the Quest build (xr_quest.cpp, main.cpp, input.cpp, os.c, overlay.cpp,
// audio_mix.cpp) and visionos/xr/SETTINGS.md for the visionOS-only ones.

import Foundation
import SwiftUI

/// One choice of a picker setting.
struct SVOption: Identifiable {
    let value: String
    let label: LocalizedStringKey
    var id: String { value }
    init(_ value: String, _ label: LocalizedStringKey) { self.value = value; self.label = label }
}

/// One setting: its variable, its control and its default.
struct SVSettingDef: Identifiable {
    enum Kind {
        case choice([SVOption])
        case slider(min: Double, max: Double, step: Double, format: (Double) -> String)
        case toggle(on: String, off: String)
    }

    let key: String              // the STRIKERS_* variable
    let title: LocalizedStringKey
    let help: LocalizedStringKey
    let kind: Kind
    let defaultValue: String
    /// The XR layer can apply it while playing; otherwise it waits for the next launch.
    let live: Bool

    var id: String { key }
}

struct SVSettingGroup: Identifiable {
    let section: SVSettingSection
    let settings: [SVSettingDef]
    var id: SVSettingSection { section }
    init(_ section: SVSettingSection, _ settings: [SVSettingDef]) { self.section = section; self.settings = settings }
}

enum SVSettingSection: CaseIterable, Identifiable {
    case view, picture, performance, controls, sound, language, diagnostics
    var id: Self { self }
    var title: LocalizedStringKey {
        switch self {
        case .view: return "View"
        case .picture: return "Picture"
        case .performance: return "Performance"
        case .controls: return "Controls"
        case .sound: return "Sound"
        case .language: return "Language"
        case .diagnostics: return "Diagnostics"
        }
    }
}

@MainActor
final class SVSettings: ObservableObject {
    static let shared = SVSettings()
    private static let prefix = "sv.setting."

    /// The app-level options (not game variables).
    @Published var allowFullImmersion = UserDefaults.standard.bool(forKey: "sv.allowFullImmersion") {
        didSet { UserDefaults.standard.set(allowFullImmersion, forKey: "sv.allowFullImmersion") }
    }
    @Published var showHands = UserDefaults.standard.object(forKey: "sv.showHands2") as? Bool ?? false {
        didSet { UserDefaults.standard.set(showHands, forKey: "sv.showHands2") }
    }

    @Published private(set) var values: [String: String] = [:]

    // Every option of visionos/xr/SETTINGS.md (the authority for names, ranges, defaults and which
    // ones apply live), plus the game's own ones the app exposes (audio_mix.cpp, os.c, overlay.cpp,
    // the STRIKERS_LOG_* flags).
    static let definitions: [SVSettingGroup] = [
        SVSettingGroup(.view, [
            SVSettingDef(key: "STRIKERS_XR_START_VIEW", title: "Start in",
                         help: "The view the game opens in. Switch any time with the stick double-clicks.",
                         kind: .choice([SVOption("0", "Big screen · your room"), SVOption("1", "Big screen · stadium"),
                                        SVOption("2", "Tabletop · your room"), SVOption("3", "Tabletop · stadium")]),
                         defaultValue: "0", live: false),
            SVSettingDef(key: "STRIKERS_XR_SCREEN_SIZE", title: "Big screen size",
                         help: "Small: 2.4 m wide at 2.5 m. Medium: 3.6 m at 3 m. Large: 6 m at 4 m. A single click of the right stick cycles it too.",
                         kind: .choice([SVOption("0", "Small"), SVOption("1", "Medium"), SVOption("2", "Large")]),
                         defaultValue: "1", live: true),
            SVSettingDef(key: "STRIKERS_XR_SCREEN_CURVE", title: "Big screen curve",
                         help: "0 is flat; 1 curves the screen around you.",
                         kind: .slider(min: 0, max: 1, step: 0.05, format: { String(format: "%.2f", $0) }),
                         defaultValue: "0", live: true),
            SVSettingDef(key: "STRIKERS_XR_HUD_ONLY", title: "Only the scoreboard over the table",
                         help: "Tabletop during play: the flat picture shows only the score bar and the Super Strike meter. Off keeps the whole TV picture.",
                         kind: .toggle(on: "1", off: "0"),
                         defaultValue: "1", live: true),
            SVSettingDef(key: "STRIKERS_XR_CHARSCALE", title: "Player size on the table",
                         help: "Tabletop only: the players are drawn this much bigger so they read at table size.",
                         kind: .slider(min: 1.0, max: 3.0, step: 0.1, format: { String(format: "%.1f×", $0) }),
                         defaultValue: "1.5", live: true),
            SVSettingDef(key: "STRIKERS_XR_TABLE_SCALE", title: "Table size",
                         help: "Tabletop views: the size of the field on the table (1 = a 1.3 m pitch). Holding the left grip, the left stick also resizes it, and both grips together let you stretch it.",
                         kind: .slider(min: 0.25, max: 4.0, step: 0.05, format: { String(format: "%.2f×", $0) }),
                         defaultValue: "1", live: true),
            SVSettingDef(key: "STRIKERS_XR_STADIUM_SCALE", title: "Stadium size (stadium views)",
                         help: "Only in the stadium views: 1 = life size. Smaller makes the stadium a model around you.",
                         kind: .slider(min: 0.1, max: 2.0, step: 0.05, format: { String(format: "%.2f×", $0) }),
                         defaultValue: "1", live: true),
            SVSettingDef(key: "STRIKERS_XR_SKIP_TALL_FIELD", title: "Table only in the stadium",
                         help: "Tabletop · stadium: leave out the life-size goals, benches and fence; the table carries the game.",
                         kind: .toggle(on: "1", off: "0"),
                         defaultValue: "0", live: true),
        ]),
        SVSettingGroup(.picture, [
            SVSettingDef(key: "STRIKERS_XR_RES_SCALE", title: "Game picture resolution",
                         help: "The game's own render size for the big screen (2 = about 1600×900). Higher is sharper and slower.",
                         kind: .slider(min: 1.0, max: 3.0, step: 0.25, format: { String(format: "%.2f×", $0) }),
                         defaultValue: "2", live: false),
            SVSettingDef(key: "STRIKERS_XR_EYE_SCALE", title: "3D view resolution",
                         help: "The size of each eye's picture in the 3D views, against the headset's own recommendation.",
                         kind: .slider(min: 0.3, max: 1.5, step: 0.05, format: { String(format: "%.2f×", $0) }),
                         defaultValue: "1", live: false),
            SVSettingDef(key: "STRIKERS_XR_MSAA", title: "Edge smoothing (MSAA)",
                         help: "Multisampling for the 3D views. Off is faster.",
                         kind: .choice([SVOption("1", "Off"), SVOption("4", "4×")]),
                         defaultValue: "4", live: true),
            SVSettingDef(key: "STRIKERS_XR_MSAA_TABLE", title: "Edge smoothing, table only",
                         help: "Overrides edge smoothing when only the table is drawn.",
                         kind: .choice([SVOption("", "Same as above"), SVOption("1", "Off"), SVOption("4", "4×")]),
                         defaultValue: "", live: true),
            SVSettingDef(key: "STRIKERS_XR_MSAA_STADIUM", title: "Edge smoothing, stadium",
                         help: "Overrides edge smoothing when the life-size stadium is drawn.",
                         kind: .choice([SVOption("", "Same as above"), SVOption("1", "Off"), SVOption("4", "4×")]),
                         defaultValue: "", live: true),
            SVSettingDef(key: "STRIKERS_XR_ANISO", title: "Texture sharpness",
                         help: "Anisotropic filtering: keeps the pitch's grass and lines crisp at a slant.",
                         kind: .choice([SVOption("1", "Off"), SVOption("2", "2×"), SVOption("4", "4×"), SVOption("8", "8×"), SVOption("16", "16×")]),
                         defaultValue: "8", live: true),
        ]),
        SVSettingGroup(.performance, [
            SVSettingDef(key: "STRIKERS_XR_DECODE", title: "Vertex decode once",
                         help: "Decodes the game's geometry once per frame instead of once per eye. Faster; turn it off only to compare.",
                         kind: .choice([SVOption("0", "Off"), SVOption("1", "Players only"), SVOption("2", "Everything")]),
                         defaultValue: "2", live: true),
            SVSettingDef(key: "STRIKERS_XR_LOD", title: "Stadium detail cut-off",
                         help: "Stadium objects smaller than this many pixels are skipped. 0 draws everything.",
                         kind: .slider(min: 0, max: 32, step: 1, format: { "\(Int($0)) px" }),
                         defaultValue: "0", live: true),
            SVSettingDef(key: "STRIKERS_XR_CULL", title: "Skip what each eye cannot see",
                         help: "Per-eye culling of stadium objects. Off only to diagnose missing objects.",
                         kind: .toggle(on: "1", off: "0"),
                         defaultValue: "1", live: false),
            SVSettingDef(key: "STRIKERS_XR_FOVEATION", title: "Foveated rendering",
                         help: "Sharper where you look and coarser at the edges. Experimental: keep it off unless asked.",
                         kind: .toggle(on: "1", off: "0"),
                         defaultValue: "0", live: false),
        ]),
        SVSettingGroup(.controls, [
            SVSettingDef(key: "STRIKERS_XR_HAPTICS", title: "Controller vibration",
                         help: "How strongly the game's rumble reaches the PlayStation VR2 Sense controllers and gamepads. 0 turns it off.",
                         kind: .slider(min: 0, max: 100, step: 5, format: { "\(Int($0)) %" }),
                         defaultValue: "100", live: true),
            SVSettingDef(key: "STRIKERS_XR_DEADZONE", title: "Stick dead zone",
                         help: "How far a stick moves before the game notices. Raise it if a player drifts on his own.",
                         kind: .slider(min: 0, max: 0.4, step: 0.01, format: { String(format: "%.2f", $0) }),
                         defaultValue: "0.12", live: true),
            SVSettingDef(key: "STRIKERS_XR_PS_BUTTON", title: "PS button pauses",
                         help: "When visionOS passes the PS button to the game, it acts as Start. It never leaves the game.",
                         kind: .toggle(on: "1", off: "0"),
                         defaultValue: "0", live: true),
        ]),
        SVSettingGroup(.sound, [
            SVSettingDef(key: "STRIKERS_AUDIO_VOLUME", title: "Game volume",
                         help: "The game's master volume, on top of the headset's.",
                         kind: .slider(min: 0, max: 2, step: 0.05, format: { "\(Int(($0 * 100).rounded())) %" }),
                         defaultValue: "1", live: false),
        ]),
        SVSettingGroup(.language, [
            SVSettingDef(key: "STRIKERS_LANGUAGE", title: "Game language",
                         help: "Automatic uses your headset's language when the disc has it, and English when not. The US disc is English only; the European disc has English, German, French, Spanish and Italian; the Japanese disc has all six.",
                         kind: .choice([SVOption("automatic", "Automatic"), SVOption("english", "English"), SVOption("german", "Deutsch"),
                                        SVOption("french", "Français"), SVOption("spanish", "Español"), SVOption("italian", "Italiano"),
                                        SVOption("japanese", "日本語")]),
                         defaultValue: "automatic", live: false),
        ]),
        SVSettingGroup(.diagnostics, [
            SVSettingDef(key: "STRIKERS_OVERLAY", title: "Performance overlay",
                         help: "The port's heads-up display: frame rate and timings.",
                         kind: .toggle(on: "1", off: "0"),
                         defaultValue: "0", live: false),
            SVSettingDef(key: "SV_LOG_LEVEL", title: "Log detail",
                         help: "How much the game writes to its log. More detail helps when reporting a problem.",
                         kind: .choice([SVOption("0", "Normal"), SVOption("1", "Detailed"), SVOption("2", "Everything")]),
                         defaultValue: "0", live: false),
            SVSettingDef(key: "STRIKERS_XR_STATS_SECONDS", title: "Performance lines in the log",
                         help: "How often the headset layer writes its frame rate line to the log.",
                         kind: .slider(min: 1, max: 60, step: 1, format: { "\(Int($0)) s" }),
                         defaultValue: "5", live: true),
            SVSettingDef(key: "STRIKERS_XR_DEPTH", title: "Depth for the system",
                         help: "What visionOS gets as the picture's depth. Off: none; the whole stadium shows and nothing cuts it, but real objects never hide the game. Near only: depth just for what is within about 1.2 m of you (the table), so close things hold steady without cutting the far stadium. Full real depth: every pixel, so furniture hides the game; for diagnosis.",
                         kind: .choice([SVOption("0", "Off"), SVOption("1", "Near only"), SVOption("2", "Full real depth")]),
                         defaultValue: "0", live: true),
        ]),
    ]

    /// The ones that apply mid-game (the match controls window shows these).
    static var live: [SVSettingDef] { all.filter { $0.live } }

    static var all: [SVSettingDef] { definitions.flatMap { $0.settings } }

    private init() {
        for def in Self.all {
            values[def.key] = UserDefaults.standard.string(forKey: Self.prefix + def.key) ?? def.defaultValue
        }
    }

    func value(_ key: String) -> String {
        values[key] ?? Self.all.first { $0.key == key }?.defaultValue ?? ""
    }

    func set(_ key: String, _ value: String) {
        guard values[key] != value else { return }
        values[key] = value
        UserDefaults.standard.set(value, forKey: Self.prefix + key)
        SVLog.log("setting \(key) = \(value)")
        if sv_game_running() != 0 {
            for (k, v) in expand(key, value) { sv_setting_changed(k, v ?? "") }
        }
    }

    func resetAll() {
        for def in Self.all { set(def.key, def.defaultValue) }
        allowFullImmersion = false
        showHands = false
    }

    /// One setting as the variables it stands for (nil = unset).
    private func expand(_ key: String, _ value: String, region: SVRegion? = nil) -> [(String, String?)] {
        switch key {
        case "SV_LOG_LEVEL":
            let level = Int(value) ?? 0
            return [("STRIKERS_LOG_SCENES", level >= 1 ? "1" : nil),
                    ("STRIKERS_LOG_MATCH", level >= 1 ? "1" : nil),
                    ("STRIKERS_LOG_EVENTS", level >= 2 ? "1" : nil),
                    ("STRIKERS_LOG_AUDIO", level >= 2 ? "1" : nil)]
        case "STRIKERS_LANGUAGE":
            let choice = SVGameLanguage(rawValue: value) ?? .automatic
            return [(key, SVGameLanguage.resolved(choice, region: region)?.envValue)]
        case "STRIKERS_OVERLAY":
            return [(key, value == "1" ? "1" : nil)]
        case "STRIKERS_XR_HAPTICS":
            // The game's own pad rumble switch too, so 0 really means none.
            return [(key, value), ("STRIKERS_PAD_RUMBLE", value == "0" ? "0" : nil)]
        default:
            // Empty = the default (e.g. "Same as above"): unset, so the XR layer falls back.
            return [(key, value.isEmpty ? nil : value)]
        }
    }

    /// Before sv_game_start: every setting into the environment. Some variables are presence flags
    /// (the port reads "set at all"), so an unset one is removed rather than set to "0".
    func applyToEnvironment(region: SVRegion?) {
        for def in Self.all {
            for (k, v) in expand(def.key, value(def.key), region: region) {
                if let v { setenv(k, v, 1) } else { unsetenv(k) }
            }
        }
    }
}

// MARK: - The screen

struct SVSettingsView: View {
    @ObservedObject private var settings = SVSettings.shared
    @State private var confirmReset = false
    let region: SVRegion?

    init(region: SVRegion?) { self.region = region }

    var body: some View {
        Form {
            ForEach(SVSettings.definitions) { group in
                let section = group.section
                Section(section.title) {
                    if section == .view {
                        Toggle("Allow full immersion", isOn: $settings.allowFullImmersion)
                        Text("Lets the stadium views hide your room completely (turn the Digital Crown). Off keeps passthrough around you.")
                            .font(.footnote).foregroundStyle(.secondary)
                        Toggle("Show my hands and arms", isOn: $settings.showHands)
                        Text("Your real hands (and what they hold) cut out over the game. Off keeps the game on top of everything; the stadium hides them while you stand in it, whatever this says.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                    ForEach(group.settings) { def in
                        SVSettingRow(def: def, region: region)
                    }
                }
            }
            Section {
                Button("Reset all settings", role: .destructive) { confirmReset = true }
            } footer: {
                if sv_game_running() != 0 {
                    Text("Settings marked “next launch” take effect the next time you open the app.")
                }
            }
        }
        .confirmationDialog("Reset all settings?", isPresented: $confirmReset) {
            Button("Reset", role: .destructive) { settings.resetAll() }
        }
    }
}

struct SVSettingRow: View {
    let def: SVSettingDef
    let region: SVRegion?
    @ObservedObject private var settings = SVSettings.shared

    // Explicit: the private property would make the memberwise initializer private, and the match
    // controls window (SVImmersiveControls.swift) builds rows too.
    init(def: SVSettingDef, region: SVRegion?) {
        self.def = def
        self.region = region
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            switch def.kind {
            case .choice(let options):
                Picker(def.title, selection: binding) {
                    ForEach(options.filter { allowed($0.value) }) { option in
                        Text(option.label).tag(option.value)
                    }
                }
            case .slider(let min, let max, let step, let format):
                let number = Binding<Double>(
                    get: { Double(settings.value(def.key)) ?? Double(def.defaultValue) ?? min },
                    set: { settings.set(def.key, Self.text($0, step: step)) })
                HStack {
                    Text(def.title)
                    Spacer()
                    Text(format(number.wrappedValue)).monospacedDigit().foregroundStyle(.secondary)
                }
                Slider(value: number, in: min...max, step: step)
            case .toggle(let on, let off):
                Toggle(def.title, isOn: Binding(get: { settings.value(def.key) == on },
                                                set: { settings.set(def.key, $0 ? on : off) }))
            }
            HStack(alignment: .firstTextBaseline) {
                Text(def.help).font(.footnote).foregroundStyle(.secondary)
                if !def.live && sv_game_running() != 0 {
                    Spacer()
                    Text("next launch").font(.caption2).foregroundStyle(.orange)
                }
            }
        }
    }

    private var binding: Binding<String> {
        Binding(get: { settings.value(def.key) }, set: { settings.set(def.key, $0) })
    }

    /// Languages the disc does not carry are hidden (the chosen one stays visible).
    private func allowed(_ value: String) -> Bool {
        guard def.key == "STRIKERS_LANGUAGE", let region,
              let language = SVGameLanguage(rawValue: value), language != .automatic else { return true }
        return region.languages.contains(language) || settings.value(def.key) == value
    }

    private static func text(_ value: Double, step: Double) -> String {
        step >= 1 ? String(Int(value.rounded())) : String(format: "%.2f", value)
    }
}
