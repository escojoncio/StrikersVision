// StrikersVision: the launcher window, the Quest panel's (LobbyActivity.java) counterpart. Play
// offline, host an online match (a four-letter code to tell your friend), join one by its code, or
// play with the people in a SharePlay call; the disc status and the folder picker; settings and log.

import GroupActivities
import SwiftUI
import UniformTypeIdentifiers

struct SVLauncherView: View {
    enum Page: Hashable { case home, host, join }
    enum Sheet: String, Identifiable { case settings, log, help, permissions; var id: String { rawValue } }

    @ObservedObject private var game = SVGame.shared
    @ObservedObject private var sharePlay = SVSharePlay.shared
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.openWindow) private var openWindow

    @State private var page: Page = .home
    @State private var sheet: Sheet?
    @State private var disc: SVDiscImage?
    @State private var pickedFolder: URL?
    @State private var pickingFolder = false
    @State private var problem: String?
    @State private var hostedCode = UserDefaults.standard.string(forKey: "sv.lastHosted") ?? SVLauncherView.newCode()
    @State private var joinCode = UserDefaults.standard.string(forKey: "sv.lastCode") ?? ""
    @State private var opening = false
    /// Play was pressed: the space opens as soon as the game reports it can draw (game_ready).
    @State private var openWhenReady = false
    /// What to do once the permissions sheet closes (the first Play).
    @State private var pendingPlay: (() -> Void)?

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 20) {
                    switch page {
                    case .home: home
                    case .host: host
                    case .join: join
                    }
                }
                .padding(32)
                .frame(maxWidth: 640)
                .frame(maxWidth: .infinity)
            }
            .navigationTitle("Super Mario Strikers")
            .toolbar {
                ToolbarItemGroup(placement: .topBarTrailing) {
                    Button { sheet = .help } label: { Label("Help", systemImage: "questionmark.circle") }
                    Button { sheet = .log } label: { Label("Log", systemImage: "doc.text.magnifyingglass") }
                    Button { sheet = .settings } label: { Label("Settings", systemImage: "gearshape") }
                }
            }
        }
        .sheet(item: $sheet, onDismiss: sheetClosed) { which in
            NavigationStack {
                Group {
                    switch which {
                    case .settings: SVSettingsView(region: disc?.region).navigationTitle("Settings")
                    case .log: SVLogView().navigationTitle("Log")
                    case .help: SVHelpView().navigationTitle("Help")
                    case .permissions: SVPermissionsView { sheet = nil }.navigationTitle("Before you play")
                    }
                }
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Close") { sheet = nil }
                    }
                }
            }
            .frame(minWidth: 640, minHeight: 600)
        }
        .fileImporter(isPresented: $pickingFolder, allowedContentTypes: [.folder]) { result in
            switch result {
            case .success(let url):
                if let why = SVPickedFolder.shared.choose(url) { problem = why } else { refresh() }
            case .failure(let error):
                problem = error.localizedDescription
            }
        }
        .onAppear {
            // Reopened from Home with the game on (no controller to hold Select): the panels come back.
            if game.immersiveOpen { game.panelsVisible = true }
            sharePlay.start()
            refresh()
            if !UserDefaults.standard.bool(forKey: "sv.helpShown") {
                UserDefaults.standard.set(true, forKey: "sv.helpShown")
                sheet = .help
            }
        }
        .onChange(of: game.ready) { _, ready in
            if ready && openWhenReady {
                openWhenReady = false
                Task { await openSpace() }
            }
        }
        .onChange(of: game.phase) { _, phase in
            // The game is gone: the space would only show its last frame. The reason is on this page.
            if case .ended = phase {
                openWhenReady = false
                page = .home
                if game.immersiveOpen { Task { await dismissImmersiveSpace() } }
            }
        }
        .onChange(of: sharePlay.joined) { _, joined in
            // Someone in the call started the match: offer it on the home page (handled there); when
            // the group goes away while this headset plays through it, fall back to offline.
            if !joined && game.roomViaSharePlay { game.setRoom("", viaSharePlay: false) }
        }
    }

    // MARK: pages

    @ViewBuilder private var home: some View {
        statusCard
        if case .ended(let reason) = game.phase {
            note(String(localized: "The game has ended: \(reason)\nClose the app completely and open it again to play another session."),
                 warn: true)
        }
        if let problem { note(problem, warn: true) }

        if game.phase == .running && !game.ready {
            HStack(spacing: 12) {
                ProgressView()
                Text("Loading the game…").font(.headline)
            }
            .padding()
            .frame(maxWidth: .infinity)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 20))
        }
        if game.phase == .running && game.ready {
            playingCard
        }

        VStack(spacing: 12) {
            bigButton("Play offline", systemImage: "gamecontroller.fill", primary: true) { play(room: "", sharePlay: false) }
            bigButton("Host an online match", systemImage: "person.2.wave.2") { page = .host }
            bigButton("Join with a code", systemImage: "keyboard") { page = .join }
            sharePlayButton
        }
        .disabled(!canPlay)

        Text("Online: both players pick Grudge Match in the same room, choose their teams and start.")
            .font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center)
    }

    private var host: some View {
        VStack(spacing: 20) {
            Text("Your room code").font(.title2.bold())
            Text(hostedCode.map { String($0) }.joined(separator: " "))
                .font(.system(size: 64, weight: .bold, design: .monospaced))
                .foregroundStyle(.yellow)
                .accessibilityLabel(hostedCode)
            Text("Tell your friend this code. Both of you: Grudge Match, pick your team, start.")
                .multilineTextAlignment(.center).foregroundStyle(.secondary)
            bigButton("Start", systemImage: "play.fill", primary: true) { play(room: hostedCode, sharePlay: false) }
            bigButton("New code", systemImage: "arrow.clockwise") {
                hostedCode = Self.newCode()
                UserDefaults.standard.set(hostedCode, forKey: "sv.lastHosted")
            }
            bigButton("Back", systemImage: "chevron.left") { page = .home }
        }
        .onAppear { UserDefaults.standard.set(hostedCode, forKey: "sv.lastHosted") }
    }

    private var join: some View {
        VStack(spacing: 20) {
            Text("Join a room").font(.title2.bold())
            Text("Type the code your friend sees.").foregroundStyle(.secondary)
            TextField("CODE", text: $joinCode)
                .font(.system(size: 40, weight: .bold, design: .monospaced))
                .multilineTextAlignment(.center)
                .textInputAutocapitalization(.characters)
                .autocorrectionDisabled()
                .frame(maxWidth: 360)
                .onChange(of: joinCode) { _, new in
                    let clean = Self.clean(new)
                    if clean != new { joinCode = clean }
                }
                .onSubmit(joinNow)
            if let problem { note(problem, warn: true) }
            bigButton("Join", systemImage: "arrow.right.circle.fill", primary: true, action: joinNow)
            bigButton("Back", systemImage: "chevron.left") { page = .home; problem = nil }
        }
    }

    // MARK: pieces

    private var statusCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label {
                if let disc {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(disc.name).font(.headline)
                        if let region = disc.region {
                            Text("\(region.displayName) · \(disc.gameID ?? "")").foregroundStyle(.secondary)
                        } else if disc.isStrikers == false {
                            Text("This disc is \(disc.gameID ?? "?"), not Super Mario Strikers (G4Q…).").foregroundStyle(.orange)
                        }
                        Group {
                            if disc.inPickedFolder {
                                Text("In the folder you picked")
                            } else {
                                Text("In On My Apple Vision Pro › StrikersVision › Strikers")
                            }
                        }
                        .font(.footnote).foregroundStyle(.secondary)
                    }
                } else {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("No game disc found").font(.headline)
                        Text("Put your .iso or .rvz in Files › On My Apple Vision Pro › StrikersVision › Strikers, or pick a folder.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
            } icon: {
                Image(systemName: disc == nil ? "opticaldisc" : "opticaldisc.fill")
                    .foregroundStyle(disc == nil ? .orange : .green)
            }
            HStack {
                Button("Pick a folder…") { pickingFolder = true }
                if pickedFolder != nil {
                    Button("Forget folder") { SVPickedFolder.shared.forget(); refresh() }
                }
                Spacer()
                Button { refresh() } label: { Label("Look again", systemImage: "arrow.clockwise") }
            }
            .buttonStyle(.bordered)
            .disabled(game.phase == .running)
            if let pickedFolder {
                Text("Folder: \(pickedFolder.lastPathComponent)").font(.footnote).foregroundStyle(.secondary)
            }
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 20))
    }

    private var playingCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(game.room.isEmpty ? String(localized: "Playing offline")
                 : game.roomViaSharePlay ? String(localized: "Playing with SharePlay")
                 : String(localized: "Online room \(game.room)"))
                .font(.headline)
            Picker("View", selection: Binding(get: { game.viewMode }, set: { game.setViewMode($0) })) {
                Text("Big screen").tag(Int32(SV_VIEW_JUMBOTRON_ROOM))
                Text("Big screen · stadium").tag(Int32(SV_VIEW_JUMBOTRON_STADIUM))
                Text("Table").tag(Int32(SV_VIEW_TABLETOP_ROOM))
                Text("Table · stadium").tag(Int32(SV_VIEW_TABLETOP_STADIUM))
            }
            .pickerStyle(.segmented)
            HStack {
                if !game.immersiveOpen {
                    Button("Return to the game") { Task { await openSpace() } }.buttonStyle(.borderedProminent)
                } else {
                    Button("Leave the game view") { Task { await dismissImmersiveSpace() } }.buttonStyle(.bordered)
                }
                Spacer()
                Button("Quit game", role: .destructive) {
                    Task { await game.quitApp { await dismissImmersiveSpace() } }
                }
                .buttonStyle(.bordered)
            }
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 20))
    }

    @ViewBuilder private var sharePlayButton: some View {
        switch sharePlay.availability {
        case .missingEntitlement:
            bigButton("Play with SharePlay", systemImage: "shareplay.slash") {}
                .disabled(true)
            Text("SharePlay is not available in this install: the signing removed its permission (free Apple accounts cannot have it). Online rooms with a code work the same.")
                .font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center)
        case .available:
            // Exposes the activity to the window's Share menu too (visionOS 26 nearby sharing).
            ShareLink(item: StrikersMatch(), preview: SharePreview("Super Mario Strikers"))
                .hidden()
            if sharePlay.joined && !game.roomViaSharePlay {
                bigButton("Join the SharePlay match", systemImage: "shareplay", primary: true) {
                    play(room: SVSharePlay.groupRoom, sharePlay: true)
                }
            } else {
                bigButton("Play with SharePlay", systemImage: "shareplay") {
                    Task {
                        if await sharePlay.activate() {
                            play(room: SVSharePlay.groupRoom, sharePlay: true)
                        } else if let error = sharePlay.lastError {
                            problem = error
                        }
                    }
                }
            }
            if sharePlay.joined {
                Text("SharePlay: \(sharePlay.participants) in the group. The first two to start a Grudge Match play each other.")
                    .font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center)
            }
        }
    }

    @ViewBuilder
    private func bigButton(_ title: LocalizedStringKey, systemImage: String, primary: Bool = false,
                           action: @escaping () -> Void) -> some View {
        let button = Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.title3.weight(.semibold))
                .frame(maxWidth: .infinity)
                .padding(.vertical, 6)
        }
        .controlSize(.extraLarge)
        if primary {
            button.buttonStyle(.borderedProminent)
        } else {
            button.buttonStyle(.bordered)
        }
    }

    private func note(_ text: String, warn: Bool) -> some View {
        Text(text)
            .foregroundStyle(warn ? .orange : .secondary)
            .multilineTextAlignment(.center)
            .frame(maxWidth: .infinity)
    }

    // MARK: actions

    private var canPlay: Bool {
        if case .ended = game.phase { return false }
        return !opening
    }

    private func refresh() {
        pickedFolder = SVPickedFolder.shared.url
        disc = SVDisc.find(pickedFolder: pickedFolder)
        if let disc {
            SVLog.log("disc: \(disc.url.path) \(disc.gameID ?? "(id not read)")")
        } else {
            SVLog.log("disc: none in \(SVPaths.home.path)\(pickedFolder.map { " or \($0.path)" } ?? "")")
        }
    }

    private func joinNow() {
        let code = Self.clean(joinCode)
        guard code.count >= 3 else {
            problem = String(localized: "A code is at least 3 letters or numbers.")
            return
        }
        problem = nil
        play(room: code, sharePlay: false)
    }

    private func play(room: String, sharePlay viaSharePlay: Bool) {
        problem = nil
        if !room.isEmpty && !viaSharePlay { UserDefaults.standard.set(room, forKey: "sv.lastCode") }
        if !SVAuthorization.asked {
            pendingPlay = { play(room: room, sharePlay: viaSharePlay) }
            sheet = .permissions
            return
        }
        if game.phase == .idle {
            refresh()
            guard let disc else {
                problem = String(localized: "No game disc yet. Put your .iso or .rvz in the Strikers folder (Files app), or pick a folder.")
                return
            }
            if disc.isStrikers == false {
                problem = String(localized: "\(disc.name) is not Super Mario Strikers. The game's disc code begins with G4Q.")
                return
            }
            game.setRoom(room, viaSharePlay: viaSharePlay)
            guard game.start(disc: disc, pickedFolder: pickedFolder) else { return }
        } else {
            game.setRoom(room, viaSharePlay: viaSharePlay)
        }
        page = .home
        if game.ready {
            Task { await openSpace() }
        } else {
            openWhenReady = true
        }
    }

    private func sheetClosed() {
        if let next = pendingPlay, SVAuthorization.asked {
            pendingPlay = nil
            next()
        } else {
            pendingPlay = nil
        }
    }

    private func openSpace() async {
        guard !game.immersiveOpen, !opening else { return }
        opening = true
        defer { opening = false }
        switch await openImmersiveSpace(id: SVScene.immersive) {
        case .opened:
            game.immersiveOpen = true
            SVLog.log("immersive space opened")
            // After SDL has opened its audio session; again after a pause in case it opens later.
            SVGame.anchorAudioToPlayer()
            Task {
                try? await Task.sleep(nanoseconds: 3_000_000_000)
                if game.immersiveOpen { SVGame.anchorAudioToPlayer() }
            }
            // The panels (this launcher and the match controls) stay hidden in the game; SVApp closes them.
            game.panelsVisible = false
        case .userCancelled:
            SVLog.log("immersive space: cancelled")
        case .error:
            SVLog.log("immersive space: could not open")
            problem = String(localized: "The game view could not open. Close other immersive apps and try again.")
        @unknown default:
            break
        }
    }

    // MARK: codes (LobbyActivity: no I, O, 0 or 1, a code is read out loud)

    private static let codeLetters = Array("ABCDEFGHJKLMNPQRSTUVWXYZ")

    static func newCode() -> String {
        var generator = SystemRandomNumberGenerator()
        return String((0..<4).map { _ in codeLetters.randomElement(using: &generator)! })
    }

    static func clean(_ s: String) -> String {
        var out = ""
        for c in s.uppercased() where out.count < 8 {
            if c.isASCII && (c.isLetter || c.isNumber) { out.append(c) }
        }
        return out
    }
}

/// The explanation shown before the system's permission prompts.
struct SVPermissionsView: View {
    let done: () -> Void
    @State private var asking = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                Text("The game will ask for three permissions. Each one is used only while you play:")
                row("move.3d", "Surroundings", "To place the big screen and the table in your room and keep them still while you move.")
                row("hand.raised", "Hands", "To grab, move and resize the table with your hands.")
                row("gamecontroller", "Controllers", "To track the PlayStation VR2 Sense controllers in space (grabbing the table, the pointer over the menus).")
                Text("Nothing leaves the headset. You can change these later in Settings › Privacy & Security.")
                    .font(.footnote).foregroundStyle(.secondary)
                Button {
                    asking = true
                    Task {
                        await SVAuthorization.request()
                        asking = false
                        done()
                    }
                } label: {
                    Text("Continue").frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(asking)
            }
            .padding(32)
        }
    }

    private func row(_ icon: String, _ title: LocalizedStringKey, _ text: LocalizedStringKey) -> some View {
        Label {
            VStack(alignment: .leading) {
                Text(title).font(.headline)
                Text(text).foregroundStyle(.secondary)
            }
        } icon: {
            Image(systemName: icon).font(.title2)
        }
    }
}

/// First launch and the toolbar's help: where the disc goes, the controls, the views, online.
struct SVHelpView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                section("Your game disc",
                        "Copy your own Super Mario Strikers (GameCube) disc image — .iso, .rvz, .wia, .gcm, .ciso or .gcz — with the Files app to On My Apple Vision Pro › StrikersVision › Strikers. Or press Pick a folder… to use a folder somewhere else (iCloud Drive, a USB drive). No game files come with this app.")
                section("Controllers",
                        "PlayStation VR2 Sense controllers: left stick moves, Cross passes, Circle shoots, Square uses your item, Triangle hits, right grip swaps items, the triggers are L and R, the right stick is the C-stick, Options pauses. A Bluetooth gamepad also works offline.")
                section("Views",
                        "Left stick, click twice fast: your room ⇄ the stadium. Right stick, click twice fast: big screen ⇄ table. Right stick once: big screen size. Left stick once: bring the table back in front of you. Hold the left grip to grab the table; the left stick then resizes and turns it.")
                section("Online",
                        "One player presses Host an online match and reads out the code; the other presses Join with a code. Then both pick Grudge Match, choose teams and start. A Quest 3 on the same release can play against you.")
                section("SharePlay",
                        "In a FaceTime call with Apple Vision Pro users, press Play with SharePlay: the first two to start a Grudge Match play each other.")
                section("Something wrong?",
                        "Open Log (top right) and share it. The game's log is also in the Strikers folder as strikers-log.txt.")
            }
            .padding(32)
        }
    }

    private func section(_ title: LocalizedStringKey, _ text: LocalizedStringKey) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.headline)
            Text(text).foregroundStyle(.secondary)
        }
    }
}
