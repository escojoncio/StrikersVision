// StrikersVision: SharePlay. One group activity ("StrikersMatch"); its GroupSession carries the same
// lockstep messages the relay carries (xr_net.cpp cannot tell the two apart), plus the shared table's
// pose so everyone in a shared immersive space sees the table in the same place.
//
// Who plays: the first two headsets in the group that start a Friendly (Grudge Match) play each
// other, exactly as in a relay room; xr_net.cpp ignores any third sender while two are playing. Others
// in the group stay in the call and can play their own offline match; a match is not streamed to
// spectators (only inputs travel, and a spectator would need the whole state).
//
// Spatial Personas: the session asks for a group immersive space and a table-shaped spatial template,
// so the system can seat everyone around a common origin. Apple has stated that Personas are not
// shown while an immersive space displays Compositor Services content (the game draws with
// CompositorLayer + Metal), so on visionOS 26 the call shows Personas in the shared windows only, not
// inside the game's space. The template and the table-pose sync are kept because they cost nothing
// and become meaningful the moment the system reports the local participant as spatial.
//
// SideStore and other free-account signing strip com.apple.developer.group-session. Without it the
// framework never produces a session; the app detects that up front (SVSharePlay.availability) and
// disables the button instead of failing later.

import Foundation
import GroupActivities
import SwiftUI

struct StrikersMatch: GroupActivity, Transferable {
    var metadata: GroupActivityMetadata {
        var metadata = GroupActivityMetadata()
        metadata.title = String(localized: "Super Mario Strikers match")
        metadata.subtitle = String(localized: "Play a Friendly together")
        metadata.type = .generic
        return metadata
    }
}

/// Seats for a match on a table: the two players face each other across it (the app content sits at
/// the origin, where the immersive space puts the table), everyone else stands along the sides.
struct StrikersTableTemplate: SpatialTemplate {
    enum Role: String, SpatialTemplateRole {
        case player
    }

    var elements: [any SpatialTemplateElement] {
        let home = SpatialTemplateSeatElement(position: .app.offsetBy(x: 0, z: 1.1),
                                              direction: .lookingAt(.app), role: Role.player)
        let away = SpatialTemplateSeatElement(position: .app.offsetBy(x: 0, z: -1.1),
                                              direction: .lookingAt(.app), role: Role.player)
        let watchers: [any SpatialTemplateElement] = [
            .seat(position: .app.offsetBy(x: 1.2, z: 0.4), direction: .lookingAt(.app)),
            .seat(position: .app.offsetBy(x: -1.2, z: 0.4), direction: .lookingAt(.app)),
            .seat(position: .app.offsetBy(x: 1.2, z: -0.4), direction: .lookingAt(.app)),
            .seat(position: .app.offsetBy(x: -1.2, z: -0.4), direction: .lookingAt(.app)),
        ]
        return [home, away] + watchers
    }
}

/// The SharePlay side of the app. Main-actor state for the UI; the messenger itself is reached from
/// the game's network thread through `SVSharePlayLink`, under a lock.
@MainActor
final class SVSharePlay: ObservableObject {
    static let shared = SVSharePlay()

    /// The room code that names the match inside the group (any 3-8 letters; the group is the room).
    static let groupRoom = "GROUP"

    enum Availability: Equatable {
        case available
        case missingEntitlement   // the signing removed com.apple.developer.group-session
    }

    @Published private(set) var availability: Availability
    @Published private(set) var joined = false
    @Published private(set) var participants = 0
    @Published private(set) var spatial = false
    @Published private(set) var lastError: String?
    /// The system's "a FaceTime call or nearby share is going on" (isEligibleForGroupSession).
    @Published private(set) var eligible = false

    private var session: GroupSession<StrikersMatch>?
    private var tasks: [Task<Void, Never>] = []
    private let stateObserver = GroupStateObserver()
    private var observing = false
    /// Set when this person pressed "Play with SharePlay": they take a player seat in the template.
    private var wantsToPlay = false

    private init() {
        availability = SVEntitlements.hasGroupSession ? .available : .missingEntitlement
    }

    /// Starts listening for sessions (launch time). Does nothing without the entitlement.
    func start() {
        guard !observing else { return }
        observing = true
        guard availability == .available else {
            SVNetLog.log("SharePlay: unavailable (the app was signed without com.apple.developer.group-session)")
            return
        }
        Task { @MainActor [weak self] in
            guard let self else { return }
            for await eligible in self.stateObserver.$isEligibleForGroupSession.values {
                self.eligible = eligible
            }
        }
        Task { @MainActor [weak self] in
            for await session in StrikersMatch.sessions() {
                self?.configure(session)
            }
        }
        SVTablePoseRelay.install()
    }

    /// The launcher's "Play with SharePlay": opens the share sheet (visionOS 26 shows it even outside
    /// FaceTime) or joins the session already offered. Returns false when it cannot.
    func activate() async -> Bool {
        guard availability == .available else { return false }
        wantsToPlay = true
        if let session, case .joined = session.state {
            assignSeat()
            return true
        }
        let activity = StrikersMatch()
        switch await activity.prepareForActivation() {
        case .activationPreferred:
            do {
                let started = try await activity.activate()
                if !started { SVNetLog.log("SharePlay: the system did not start the activity") }
                return started
            } catch {
                lastError = error.localizedDescription
                SVNetLog.log("SharePlay: activate failed: \(error.localizedDescription)")
                return false
            }
        case .activationDisabled:
            lastError = String(localized: "SharePlay is turned off for this app.")
            SVNetLog.log("SharePlay: activation disabled by the system")
            return false
        case .cancelled:
            return false
        @unknown default:
            return false
        }
    }

    /// Leaves the group (the launcher's "Leave SharePlay").
    func leave() {
        wantsToPlay = false
        session?.leave()
    }

    private func configure(_ newSession: GroupSession<StrikersMatch>) {
        // A new session replaces any old one.
        session?.leave()
        tasks.forEach { $0.cancel() }
        tasks.removeAll()
        session = newSession
        SVNetLog.log("SharePlay: session offered")

        let messenger = GroupSessionMessenger(session: newSession, deliveryMode: .reliable)
        SVSharePlayLink.setMessenger(messenger, local: newSession.localParticipant)

        tasks.append(Task { @MainActor [weak self] in
            if let coordinator = await newSession.systemCoordinator {
                var configuration = SystemCoordinator.Configuration()
                configuration.supportsGroupImmersiveSpace = true
                configuration.spatialTemplatePreference = .custom(StrikersTableTemplate())
                coordinator.configuration = configuration
                self?.watchCoordinator(coordinator)
                if self?.wantsToPlay == true { coordinator.assignRole(StrikersTableTemplate.Role.player) }
            }
            newSession.join()
        })
        tasks.append(Task { @MainActor [weak self] in
            for await state in newSession.$state.values {
                guard let self else { return }
                switch state {
                case .joined:
                    self.joined = true
                    SVSharePlayLink.setJoined(true)
                    SVNetLog.log("SharePlay: joined")
                case .invalidated(let reason):
                    SVNetLog.log("SharePlay: session ended (\(reason.localizedDescription))")
                    self.ended(newSession)
                case .waiting:
                    break
                @unknown default:
                    break
                }
                self.publishState()
            }
        })
        tasks.append(Task { @MainActor [weak self] in
            for await active in newSession.$activeParticipants.values {
                guard let self else { return }
                self.participants = active.count
                SVNetLog.log("SharePlay: \(active.count) in the group")
                self.publishState()
            }
        })
        tasks.append(Task.detached {
            for await (message, context) in messenger.messages(of: Data.self) {
                SVSharePlayLink.received(message, from: context.source)
            }
        })
    }

    private func watchCoordinator(_ coordinator: SystemCoordinator) {
        spatial = coordinator.localParticipantState.isSpatial
        publishState()
        tasks.append(Task { @MainActor [weak self] in
            for await state in coordinator.localParticipantStates {
                guard let self else { return }
                if self.spatial != state.isSpatial {
                    SVNetLog.log("SharePlay: spatial \(state.isSpatial ? "on" : "off")")
                }
                self.spatial = state.isSpatial
                self.publishState()
            }
        })
        tasks.append(Task { @MainActor in
            for await style in coordinator.groupImmersionStyle {
                SVNetLog.log("SharePlay: the group's immersion style is \(style.map { "\($0)" } ?? "none")")
            }
        })
    }

    private func assignSeat() {
        guard let session else { return }
        Task { @MainActor in
            await session.systemCoordinator?.assignRole(StrikersTableTemplate.Role.player)
        }
    }

    private func ended(_ old: GroupSession<StrikersMatch>) {
        guard session === old else { return }
        session = nil
        joined = false
        participants = 0
        spatial = false
        tasks.forEach { $0.cancel() }
        tasks.removeAll()
        SVSharePlayLink.setJoined(false)
        SVSharePlayLink.setMessenger(nil, local: nil)
        publishState()
    }

    private func publishState() {
        let count = joined ? participants : 0
        sv_shareplay_state(Int32(count), joined && spatial ? 1 : 0)
        SVTablePoseRelay.spatial = joined && spatial
    }
}

// MARK: - The link xr_net.cpp sees (sv_net_open(SV_NET_SHAREPLAY, ...))

/// Messages on the group carry a one-byte kind in front, so the table pose can share the messenger.
enum SVGroupKind: UInt8 {
    case game = 1
    case tablePose = 2
}

final class SVSharePlayLink: SVNetLink, @unchecked Sendable {
    private static let stateLock = NSLock()
    nonisolated(unsafe) private static var messenger: GroupSessionMessenger?
    nonisolated(unsafe) private static var joined = false

    static func setMessenger(_ m: GroupSessionMessenger?, local: Participant?) {
        stateLock.lock(); messenger = m; stateLock.unlock()
    }

    static func setJoined(_ on: Bool) {
        stateLock.lock(); joined = on; stateLock.unlock()
        for link in SVNetRegistry.sharePlayLinks {
            if on { link.setConnected() } else { link.markClosed("SharePlay: the session ended") }
        }
    }

    private static var currentMessenger: GroupSessionMessenger? {
        stateLock.lock(); defer { stateLock.unlock() }
        return joined ? messenger : nil
    }

    /// A handle for xr_net.cpp. Connected while the session is joined; it waits (unconnected) for
    /// one otherwise, and xr_net retries.
    static func open() -> SVSharePlayLink? {
        let link = SVSharePlayLink()
        stateLock.lock(); let up = joined; stateLock.unlock()
        if up { link.setConnected() }
        return link
    }

    static func received(_ message: Data, from source: Participant) {
        guard let first = message.first, let kind = SVGroupKind(rawValue: first) else { return }
        let body = message.dropFirst()
        switch kind {
        case .game:
            guard body.count <= SVNetLink.maxMessage else { return }
            let payload = Data(body)
            for link in SVNetRegistry.sharePlayLinks { link.deliver(payload) }
        case .tablePose:
            SVTablePoseRelay.received(Data(body))
        }
    }

    /// Sends one framed message to everyone else in the group.
    static func broadcast(_ kind: SVGroupKind, _ body: Data, completion: (@Sendable (Bool) -> Void)? = nil) -> Bool {
        guard let messenger = currentMessenger else { return false }
        var framed = Data([kind.rawValue])
        framed.append(body)
        messenger.send(framed, to: .all) { error in
            if let error { SVSharePlayLink.logSendFailure(error) }
            completion?(error == nil)
        }
        return true
    }

    nonisolated(unsafe) private static var sendFailures = 0
    private static func logSendFailure(_ error: Error) {
        stateLock.lock()
        sendFailures += 1
        let n = sendFailures
        stateLock.unlock()
        if n <= 3 || n % 100 == 0 { SVNetLog.log("SharePlay: send failed (\(n)): \(error.localizedDescription)") }
    }

    override func send(_ message: Data) -> Bool {
        guard !isClosed else { return false }
        // The relay echoes every message to its sender and xr_net.cpp uses that echo as its liveness
        // check (no echo for 5 s = dead link). The messenger never hands a sender its own messages,
        // so the echo is made here once the send completes, failed or not: a failed send (nobody else
        // in the group yet, a hiccup) is a lost message, which the lockstep already resends, not a
        // dead link. The link dies only with the session (setJoined(false)).
        let ok = Self.broadcast(.game, message) { [weak self] _ in
            self?.deliver(message)
        }
        if !ok { markClosed("SharePlay: no session") }
        return ok
    }
}

// MARK: - The shared table's pose

/// Local table moves go to the group (rate-limited); remote ones go to the XR layer, only while the
/// system has put everyone in one spatial frame (otherwise the numbers mean different places).
enum SVTablePoseRelay {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var lastSent: UInt64 = 0
    nonisolated(unsafe) private static var pending: Data?
    nonisolated(unsafe) private static var spatialNow = false

    /// Everyone shares one spatial frame (set from the main actor, read on the game thread).
    static var spatial: Bool {
        get { lock.lock(); defer { lock.unlock() }; return spatialNow }
        set { lock.lock(); spatialNow = newValue; lock.unlock() }
    }

    static func install() {
        sv_register_table_pose_callback(svTablePoseChanged)
    }

    /// Game thread. At most ~15 updates a second; the last pose of a burst always goes out.
    static func local(_ matrix: UnsafePointer<Float>, _ scale: Float) {
        var body = Data(count: 17 * 4)
        body.withUnsafeMutableBytes { raw in
            let f = raw.bindMemory(to: Float.self)
            for i in 0..<16 { f[i] = matrix[i] }
            f[16] = scale
        }
        let now = DispatchTime.now().uptimeNanoseconds / 1_000_000
        lock.lock()
        let due = now - lastSent >= 66
        if due { lastSent = now; pending = nil } else { pending = body }
        let wasSpatial = spatialNow
        lock.unlock()
        guard wasSpatial else { return }
        if due {
            _ = SVSharePlayLink.broadcast(.tablePose, body)
        } else {
            DispatchQueue.global().asyncAfter(deadline: .now() + .milliseconds(70)) { SVTablePoseRelay.flush() }
        }
    }

    private static func flush() {
        lock.lock()
        let body = pending
        pending = nil
        if body != nil { lastSent = DispatchTime.now().uptimeNanoseconds / 1_000_000 }
        lock.unlock()
        if let body { _ = SVSharePlayLink.broadcast(.tablePose, body) }
    }

    static func received(_ body: Data) {
        guard body.count == 17 * 4 else { return }
        guard spatial else { return }
        var values = [Float](repeating: 0, count: 17)
        _ = values.withUnsafeMutableBytes { body.copyBytes(to: $0) }
        guard values.allSatisfy({ $0.isFinite }), values[16] > 0 else { return }
        values.withUnsafeBufferPointer { buf in
            sv_apply_remote_table_pose(buf.baseAddress!, buf[16])
        }
    }
}

/// The C callback (SVTablePoseCallback). A plain function: C function pointers cannot capture.
private func svTablePoseChanged(_ matrix: UnsafePointer<Float>?, _ scale: Float) {
    guard let matrix else { return }
    SVTablePoseRelay.local(matrix, scale)
}
