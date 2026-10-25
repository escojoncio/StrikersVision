// StrikersVision headset layer, Swift half: Compositor Services frames, ARKit (head and PlayStation
// VR2 Sense controller poses), GameController buttons and haptics. The C++ half (sv_xr.mm) drives it
// from the game thread through the @_cdecl functions below (declared in sv_xr.h); nothing here runs a
// loop of its own except the ARKit anchor stream.
//
// Why Swift for this part: the Compositor Services and ARKit calls below are the ones the Klepton
// visionOS client (same owner) has run on device; their Swift spellings are proven, while several C
// spellings of the same calls are not documented. The frame order, the drawable handling and the
// Sense controller element names follow that client (credit: Klepton, building on ALVR's visionOS
// client), written again here for this game.

import ARKit
import CompositorServices
import CoreHaptics
import Foundation
import GameController
import Metal
import QuartzCore
import simd

/// The C side's log (sv_log_line in sv_game.mm), handed over by sv_xrhost_start: stderr (the game
/// log file) and the app's log viewer.
nonisolated(unsafe) private var svLogSink: (@convention(c) (UnsafePointer<CChar>?) -> Void)?

/// sv_game_panels_toggle (sv_game.mm): not in the bridging header, so bound by name.
@_silgen_name("sv_game_panels_toggle") private func svGamePanelsToggle()

private func xrLog(_ s: String) {
    if let sink = svLogSink { s.withCString { sink($0) } } else { NSLog("%@", s) }
}

// sv_xr.h's SV_XRB_* / SV_GPB_* bits, spelled here so this file does not depend on how Swift
// imports an anonymous C enum.
private enum XRB {
    static let lower: UInt32 = 1 << 0
    static let upper: UInt32 = 1 << 1
    static let menu: UInt32 = 1 << 2
    static let stickClick: UInt32 = 1 << 3
    static let ps: UInt32 = 1 << 4
}
private enum GPB {
    static let south: UInt32 = 1 << 0
    static let east: UInt32 = 1 << 1
    static let west: UInt32 = 1 << 2
    static let north: UInt32 = 1 << 3
    static let start: UInt32 = 1 << 4
    static let rShoulder: UInt32 = 1 << 5
    static let lShoulder: UInt32 = 1 << 6
    static let lStick: UInt32 = 1 << 7
    static let rStick: UInt32 = 1 << 8
    static let up: UInt32 = 1 << 9
    static let down: UInt32 = 1 << 10
    static let left: UInt32 = 1 << 11
    static let right: UInt32 = 1 << 12
    static let home: UInt32 = 1 << 13
}

/// Seconds since the Compositor Services clock's epoch: the CACurrentMediaTime domain ARKit takes.
private func seconds(_ instant: LayerRenderer.Clock.Instant) -> Double {
    let d = LayerRenderer.Clock.Instant.epoch.duration(to: instant)
    let c = d.components
    return Double(c.seconds) + Double(c.attoseconds) * 1e-18
}

private func store(_ m: simd_float4x4, _ out: UnsafeMutablePointer<Float>?) {
    guard let out else { return }
    // simd_float4x4 is four SIMD4<Float> columns: column-major, as sv_xr.mm expects.
    withUnsafeBytes(of: m) { raw in
        let f = raw.bindMemory(to: Float.self)
        for i in 0..<16 { out[i] = f[i] }
    }
}

final class SVXRHost: @unchecked Sendable {
    static let shared = SVXRHost()

    private let lock = NSLock()

    // ARKit
    private let session = ARKitSession()
    private let world = WorldTrackingProvider()
    // Hands (pinch to carry the table / screen), run only when the player allowed hand tracking.
    private var handsProvider = HandTrackingProvider()  // a fresh one each run (guarded by lock)
    private var handsRunning = false
    private var pinching: [Bool] = [false, false]
    private var accessoryProvider: Any?          // AccessoryTrackingProvider (visionOS 26)
    private var accessoryAnchors: [Int: Any] = [:] // AccessoryAnchor by hand (0 left, 1 right)
    private var trackedSpatial = Set<ObjectIdentifier>()
    // Which hand each Sense controller is, from Accessory(device:).inherentChirality: the one map
    // both the buttons and the poses use (vendor-name suffixes are only the fallback).
    private var handOf: [ObjectIdentifier: Int] = [:]
    private var started = false
    private var running = false
    private var runInFlight = false
    private var eventsObserved = false
    private var poseLogged = [false, false]
    private var nilFrames = 0
    private var emptyDrawables = 0
    private var lastControllerCheck: Double = 0
    private var headLogged = false
    private var headMisses = 0

    // The frame in flight (game thread only).
    private var frame: LayerRenderer.Frame?
    private var drawables: [LayerRenderer.Drawable] = []
    private var drawableLogged = false

    // Device anchors handed out to C by id, so a drawable can later be told the exact anchor its
    // pixels were drawn for (eye images are re-shown on display frames without a new game frame).
    // 256 entries (~1.7 s at the ~150 poses a second the two threads ask for): a pair stays on show for
    // far less, so its anchor is still here when the display thread presents it. Guarded by `lock`:
    // head() runs on the game thread and on the display thread, anchor() on the display thread.
    private var anchorRing = [DeviceAnchor?](repeating: nil, count: 256)
    private var anchorNext: Int32 = 0
    private var nilAnchorPresents: UInt64 = 0  // presents whose deviceAnchor was nil (guarded by lock)

    // Controllers (game thread only, apart from the anchor dictionary above).
    private struct HandSample {
        var buttons: UInt32 = 0
        var trigger: Float = 0
        var grip: Float = 0
        var stick = SIMD2<Float>(0, 0)
        var select = false  // Options / Create: Start only as a short tap (see input()), else the panels-toggle hold
    }
    private var selectHeldSince: Double = 0  // 0 = not held
    private var selectFired = false
    private var selectFromSense = false  // the held press began on a Sense (its tap is Start)
    private var lastSelectSample: Double = 0
    private var startPulse = 0  // input samples left of the Start a Sense tap sends
    private var handCache: [ObjectIdentifier: HandSample] = [:]
    private var captured = Set<ObjectIdentifier>()
    private var pressSeen = Set<String>()
    private var engines: [ObjectIdentifier: CHHapticEngine] = [:]
    private var players: [ObjectIdentifier: CHHapticAdvancedPatternPlayer] = [:]
    private var hapticsNoted = Set<String>()

    // MARK: ARKit

    func start() {
        lock.lock()
        if started { lock.unlock(); return }
        started = true
        let observe = !eventsObserved
        eventsObserved = true
        lock.unlock()
        xrLog("[xr] arkit: world tracking supported \(WorldTrackingProvider.isSupported), accessory tracking "
              + "supported \(Self.accessorySupported)")
        if observe { Task { await self.observeEvents() } }
        Task { await self.runSession() }
    }

    /// A new renderer (the space opened again): if the session is not running (a refused or failed
    /// run), try again.
    func retryIfNeeded() {
        lock.lock()
        let again = started && !running && !runInFlight
        lock.unlock()
        if again {
            xrLog("[xr] arkit: session not running; trying again for the new renderer")
            Task { await self.runSession() }
        }
    }

    private static var accessorySupported: Bool {
        if #available(visionOS 26.0, *) { return AccessoryTrackingProvider.isSupported }
        return false
    }

    /// Authorization changes, provider state changes and errors, as they happen.
    private func observeEvents() async {
        for await event in session.events {
            xrLog("[xr] arkit: event \(event)")
        }
    }

    private static func spatialControllers() -> [GCController] {
        guard #available(visionOS 26.0, *) else { return [] }
        return GCController.controllers().filter { $0.productCategory == GCProductCategorySpatialController }
    }

    /// (Re)runs the session with the world tracker and an accessory tracker for whatever Sense
    /// controllers are paired now. The provider list is fixed at run(), so a change of controllers
    /// means running again (not stopping: world tracking carries on).
    private func runSession() async {
        guard claimRun() else { return }
        defer { releaseRun() }

        var providers: [any DataProvider] = [world]
        var names: [String] = []
        var accessoryProvider: Any?
        if #available(visionOS 26.0, *), AccessoryTrackingProvider.isSupported {
            let spatial = Self.spatialControllers()
            var accessories: [Accessory] = []
            var hands: [ObjectIdentifier: Int] = [:]
            for c in spatial {
                do {
                    let a = try await Accessory(device: c)
                    accessories.append(a)
                    let hand = Self.hand(of: a, fallback: c)
                    hands[ObjectIdentifier(c)] = hand
                    xrLog("[xr] input: Sense \"\(c.vendorName ?? "?")\" chirality \(a.inherentChirality) -> "
                          + (hand == 0 ? "left" : "right") + " hand")
                } catch {
                    xrLog("[xr] arkit: accessory for \(c.vendorName ?? "?") failed: \(error)")
                }
            }
            installHands(hands, spatial)
            // A provider with no accessories is not run: it can fail the whole session.
            if !accessories.isEmpty {
                names = accessories.map { $0.name }
                let provider = AccessoryTrackingProvider(accessories: accessories)
                providers.append(provider)
                accessoryProvider = provider
            }
        }
        var withHands = false
        if HandTrackingProvider.isSupported {
            let auth = await session.queryAuthorization(for: [.handTracking])
            if auth[.handTracking] == .allowed {
                let hp = HandTrackingProvider()
                replaceHandsProvider(hp)
                providers.append(hp)
                withHands = true
            } else {
                xrLog("[xr] arkit: hand tracking not allowed (\(String(describing: auth[.handTracking]))); no pinch placing")
            }
        }
        do {
            try await session.run(providers)
            setRunning(accessoryProvider)
            setHands(withHands)
            xrLog("[xr] arkit: session running (world + accessories: \(names)\(withHands ? " + hands" : ""))")
            if #available(visionOS 26.0, *), let p = accessoryProvider as? AccessoryTrackingProvider {
                Task { await self.consume(p) }
            }
        } catch {
            xrLog("[xr] arkit error: session.run with \(providers.count) provider(s) failed: \(error)")
            if providers.count > 1 {
                do {
                    try await session.run([world])
                    setRunning(nil)
                    setHands(false)
                    xrLog("[xr] arkit: session running with world tracking only (no controller poses)")
                    return
                } catch {
                    xrLog("[xr] arkit error: session.run (world only) failed: \(error)")
                }
            }
            // Not running: the next renderer tries again (retryIfNeeded).
            setStopped()
        }
    }

    private func replaceHandsProvider(_ hp: HandTrackingProvider) {
        lock.lock(); defer { lock.unlock() }
        handsRunning = false
        handsProvider = hp
    }

    private func setHands(_ on: Bool) {
        lock.lock(); defer { lock.unlock() }
        handsRunning = on
    }

    /// Per hand (0 left, 1 right): tracked, pinching (thumb and index tips together, with hysteresis),
    /// and the pinch point (between the two tips) in the origin space.
    func hands(_ time: Double, _ valid: UnsafeMutablePointer<Int32>?, _ pinch: UnsafeMutablePointer<Int32>?,
               _ pos: UnsafeMutablePointer<Float>?) {
        lock.lock()
        let provider = handsProvider
        let on = handsRunning
        lock.unlock()
        for h in 0..<2 { valid?[h] = 0; pinch?[h] = 0 }
        guard on, provider.state == .running else { return }
        let anchors = provider.handAnchors(at: time)
        for (h, anchor) in [anchors.leftHand, anchors.rightHand].enumerated() {
            guard let a = anchor, a.isTracked, let skel = a.handSkeleton else { pinching[h] = false; continue }
            let thumb = skel.joint(.thumbTip), index = skel.joint(.indexFingerTip)
            guard thumb.isTracked, index.isTracked else { pinching[h] = false; continue }
            let t = a.originFromAnchorTransform * thumb.anchorFromJointTransform.columns.3
            let i = a.originFromAnchorTransform * index.anchorFromJointTransform.columns.3
            let d = simd_distance(SIMD3(t.x, t.y, t.z), SIMD3(i.x, i.y, i.z))
            pinching[h] = pinching[h] ? d < 0.035 : d < 0.018
            valid?[h] = 1
            pinch?[h] = pinching[h] ? 1 : 0
            pos?[h * 3 + 0] = (t.x + i.x) * 0.5
            pos?[h * 3 + 1] = (t.y + i.y) * 0.5
            pos?[h * 3 + 2] = (t.z + i.z) * 0.5
        }
    }

    // Synchronous helpers: a lock is never taken inside an async function.
    private func claimRun() -> Bool {
        lock.lock(); defer { lock.unlock() }
        if runInFlight { return false }
        runInFlight = true
        return true
    }
    private func releaseRun() { lock.lock(); runInFlight = false; lock.unlock() }
    private func setStopped() { lock.lock(); running = false; lock.unlock() }

    /// 0 left, 1 right. An unspecified chirality falls back to the controller's name, then right.
    @available(visionOS 26.0, *)
    private static func hand(of accessory: Accessory, fallback c: GCController?) -> Int {
        switch accessory.inherentChirality {
        case .left: return 0
        case .right: return 1
        default: return c.map { isLeft($0) ? 0 : 1 } ?? 1
        }
    }

    @available(visionOS 26.0, *)
    private func consume(_ provider: AccessoryTrackingProvider) async {
        for await update in provider.anchorUpdates {
            let anchor = update.anchor
            let hand = Self.hand(of: anchor.accessory, fallback: nil)
            keep(hand: hand, anchor: update.event == .removed ? nil : anchor, from: provider)
        }
    }

    // Synchronous on purpose: a lock must never be held across a suspension point.
    private func installHands(_ hands: [ObjectIdentifier: Int], _ spatial: [GCController]) {
        lock.lock()
        trackedSpatial = Set(spatial.map { ObjectIdentifier($0) })
        handOf = hands
        lock.unlock()
    }

    private func setRunning(_ provider: Any?) {
        lock.lock()
        running = true
        accessoryProvider = provider
        accessoryAnchors.removeAll()
        lock.unlock()
    }

    /// Records the paired Sense controllers (always, so a change seen while the session is not running
    /// is not reported again) and says whether the running session needs a new accessory tracker.
    private func spatialChanged(_ now: Set<ObjectIdentifier>) -> Bool {
        lock.lock(); defer { lock.unlock() }
        let changed = now != trackedSpatial
        trackedSpatial = now
        return changed && running
    }

    private func handFor(_ c: GCController) -> Int {
        lock.lock(); let h = handOf[ObjectIdentifier(c)]; lock.unlock()
        return h ?? (Self.isLeft(c) ? 0 : 1)
    }

    private func keep(hand: Int, anchor: Any?, from provider: AnyObject) {
        lock.lock()
        // A provider replaced by a re-run may still deliver: its anchors are not the current one's.
        if let current = accessoryProvider, (current as AnyObject) === provider {
            accessoryAnchors[hand] = anchor
        }
        lock.unlock()
    }

    // MARK: head

    func head(_ time: Double, _ out: UnsafeMutablePointer<Float>?) -> Int32 {
        // ARKit refuses instants too far ahead; stepping back finds the edge of what it predicts.
        var t = time
        for _ in 0..<8 {
            if let a = world.queryDeviceAnchor(atTimestamp: t) {
                if !headLogged {
                    headLogged = true
                    xrLog(String(format: "[xr] arkit: first head pose, %.1f ms ahead", (time - CACurrentMediaTime()) * 1000))
                }
                store(a.originFromAnchorTransform, out)
                lock.lock()
                let id = anchorNext
                anchorRing[Int(id) % anchorRing.count] = a
                anchorNext = (anchorNext + 1) % 0x4000_0000
                lock.unlock()
                return id
            }
            t -= 0.005
        }
        lock.lock()
        headMisses += 1
        let misses = headMisses
        let isRunning = running
        lock.unlock()
        if misses == 1 || misses % 900 == 0 {
            xrLog("[xr] arkit warn: no head pose (\(misses) misses; session running \(isRunning))")
        }
        return -1
    }

    private func anchor(_ id: Int32) -> DeviceAnchor? {
        guard id >= 0 else { return nil }
        lock.lock(); defer { lock.unlock() }
        // Only while it is still in the ring: an id older than the ring is a stale picture anyway.
        if anchorNext - id > Int32(anchorRing.count) || id >= anchorNext { return nil }
        return anchorRing[Int(id) % anchorRing.count]
    }

    // MARK: frames

    func rendererState(_ r: LayerRenderer) -> Int32 {
        switch r.state {
        case .running: return 1
        case .invalidated: return 2
        default: return 0
        }
    }

    func frameBegin(_ r: LayerRenderer, _ device: UnsafeMutablePointer<UnsafeMutableRawPointer?>?) -> Int32 {
        presented = 0
        guard let f = r.queryNextFrame() else {
            nilFrames += 1
            if nilFrames == 1 || nilFrames % 300 == 0 {
                xrLog("[xr] cp warn: queryNextFrame gave no frame (\(nilFrames) in a row; renderer \(r.state))")
            }
            return 0
        }
        nilFrames = 0
        f.startUpdate()
        f.endUpdate()
        if let timing = f.predictTiming() {
            LayerRenderer.Clock().wait(until: timing.optimalInputTime)
        }
        // All drawables, not the first: visionOS 26 hands out more than one while the render quality
        // changes, and each must be presented.
        let ds = f.queryDrawables()
        // No drawable: the frame is abandoned without endSubmission (ending a submission that
        // presented nothing aborts the process).
        guard !ds.isEmpty else {
            emptyDrawables += 1
            if emptyDrawables == 1 || emptyDrawables % 300 == 0 {
                xrLog("[xr] cp warn: frame without drawables (\(emptyDrawables) in a row; renderer \(r.state))")
            }
            return 0
        }
        emptyDrawables = 0
        f.startSubmission()
        frame = f
        drawables = ds
        device?.pointee = Unmanaged.passUnretained(r.device as AnyObject).toOpaque()
        if !drawableLogged {
            drawableLogged = true
            let d = ds[0]
            var line = "[xr] cp: \(ds.count) drawable(s), \(d.views.count) view(s), \(d.colorTextures.count) colour / "
                + "\(d.depthTextures.count) depth texture(s), layout "
                + (r.configuration.layout == .layered ? "layered" : (r.configuration.layout == .dedicated ? "dedicated" : "shared"))
                + ", colour format \(r.configuration.colorFormat.rawValue), depth range \(d.depthRange)"
            for (i, t) in d.colorTextures.enumerated() {
                line += "; colour[\(i)] \(t.width)x\(t.height) array \(t.arrayLength)"
            }
            for (i, v) in d.views.enumerated() {
                let m = v.textureMap
                line += "; view[\(i)] texture \(m.textureIndex) slice \(m.sliceIndex) viewport "
                    + "\(Int(m.viewport.width))x\(Int(m.viewport.height))+\(Int(m.viewport.originX))+\(Int(m.viewport.originY))"
            }
            xrLog(line)
        }
        return Int32(ds.count)
    }

    func drawableInfo(_ i: Int32, _ time: UnsafeMutablePointer<Double>?, _ range: UnsafeMutablePointer<Float>?,
                      _ views: UnsafeMutablePointer<Int32>?) -> Int32 {
        guard i >= 0, Int(i) < drawables.count else { return 0 }
        let d = drawables[Int(i)]
        time?.pointee = seconds(d.frameTiming.presentationTime)
        if let range {
            range[0] = d.depthRange.x
            range[1] = d.depthRange.y
        }
        views?.pointee = Int32(d.views.count)
        return 1
    }

    func frameView(_ i: Int32, _ v: Int32, _ deviceFromView: UnsafeMutablePointer<Float>?,
                   _ projection: UnsafeMutablePointer<Float>?, _ color: UnsafeMutablePointer<UnsafeMutableRawPointer?>?,
                   _ depth: UnsafeMutablePointer<UnsafeMutableRawPointer?>?, _ slice: UnsafeMutablePointer<Int32>?,
                   _ viewport: UnsafeMutablePointer<Float>?) -> Int32 {
        guard i >= 0, Int(i) < drawables.count else { return 0 }
        let d = drawables[Int(i)]
        guard v >= 0, Int(v) < d.views.count else { return 0 }
        let view = d.views[Int(v)]
        store(view.transform, deviceFromView)
        store(d.computeProjection(viewIndex: Int(v)), projection)
        let map = view.textureMap
        if map.textureIndex < d.colorTextures.count {
            color?.pointee = Unmanaged.passUnretained(d.colorTextures[map.textureIndex] as AnyObject).toOpaque()
        } else {
            color?.pointee = nil
        }
        if map.textureIndex < d.depthTextures.count {
            depth?.pointee = Unmanaged.passUnretained(d.depthTextures[map.textureIndex] as AnyObject).toOpaque()
        } else {
            depth?.pointee = nil
        }
        slice?.pointee = Int32(map.sliceIndex)
        if let viewport {
            viewport[0] = Float(map.viewport.originX)
            viewport[1] = Float(map.viewport.originY)
            viewport[2] = Float(map.viewport.width)
            viewport[3] = Float(map.viewport.height)
        }
        return 1
    }

    private var presented = 0

    func present(_ i: Int32, _ cmd: UnsafeMutableRawPointer?, _ anchorId: Int32) {
        guard i >= 0, Int(i) < drawables.count else { return }
        guard let cmd else {
            xrLog("[xr] cp error: present without a command buffer")
            return
        }
        guard let buffer = Unmanaged<AnyObject>.fromOpaque(cmd).takeUnretainedValue() as? any MTLCommandBuffer else {
            xrLog("[xr] cp error: present without a command buffer")
            return
        }
        let d = drawables[Int(i)]
        d.deviceAnchor = anchor(anchorId)
        if d.deviceAnchor == nil {
            lock.lock(); nilAnchorPresents += 1; lock.unlock()
        }
        d.encodePresent(commandBuffer: buffer)
        presented += 1
    }

    /// Presents without a device anchor since the last call (the stats line).
    func takeNilAnchorPresents() -> UInt64 {
        lock.lock(); defer { lock.unlock() }
        let n = nilAnchorPresents
        nilAnchorPresents = 0
        return n
    }

    func finish() {
        // endSubmission with nothing presented aborts the process: such a frame is abandoned.
        if presented > 0 {
            frame?.endSubmission()
        } else if frame != nil {
            xrLog("[xr] cp error: frame ended with nothing presented; abandoned without endSubmission")
        }
        presented = 0
        frame = nil
        drawables = []
    }

    func device(_ r: LayerRenderer) -> UnsafeMutableRawPointer {
        Unmanaged.passUnretained(r.device as AnyObject).toOpaque()
    }

    func formats(_ r: LayerRenderer, _ color: UnsafeMutablePointer<UInt32>?, _ depth: UnsafeMutablePointer<UInt32>?) {
        color?.pointee = UInt32(r.configuration.colorFormat.rawValue)
        depth?.pointee = UInt32(r.configuration.depthFormat.rawValue)
    }

    // MARK: input

    private func noteFirstPress(_ name: String) {
        if pressSeen.insert(name).inserted { xrLog("[xr] input: first press of \"\(name)\"") }
    }

    /// Keeps the PS / Create / Options buttons from being taken by the system, so the game never
    /// leaves on a press (what the system does with the PS button is its own decision; this asks).
    private func capture(_ c: GCController) {
        guard captured.insert(ObjectIdentifier(c)).inserted else { return }
        var names: [String] = []
        for (name, el) in c.physicalInputProfile.buttons {
            let n = name.lowercased()
            guard n.contains("home") || n.contains("menu") || n.contains("ps") || n.contains("options")
                || n.contains("share") else { continue }
            el.preferredSystemGestureState = .disabled
            names.append("\(name)=\(el.isBoundToSystemGesture ? "system" : "game")")
        }
        if let h = c.extendedGamepad?.buttonHome { h.preferredSystemGestureState = .disabled }
        if let m = c.extendedGamepad?.buttonMenu { m.preferredSystemGestureState = .disabled }
        xrLog("[xr] input: controller \"\(c.vendorName ?? "?")\" (\(c.productCategory)) attached; system buttons \(names.sorted())")
    }

    private static func isLeft(_ c: GCController) -> Bool { (c.vendorName ?? "").hasSuffix("(L)") }

    @available(visionOS 26.0, *)
    private func senseSample(_ c: GCController) -> HandSample {
        let key = ObjectIdentifier(c)
        c.input.inputStateQueueDepth = 1
        // nil = nothing new since the last poll, not "all released".
        guard let s = c.input.nextInputState() else { return handCache[key] ?? HandSample() }
        let b = s.buttons, ax = s.axes, dp = s.dpads
        func pressed(_ n: String) -> Bool { b[n]?.pressedInput.isPressed ?? false }
        func value(_ n: String) -> Float { b[n]?.pressedInput.value ?? 0 }
        func axis(_ n: String) -> Float { ax[n]?.absoluteInput?.value ?? 0 }
        for el in b where el.pressedInput.isPressed {
            for n in el.aliases { noteFirstPress("\(n) [\(Self.isLeft(c) ? "L" : "R")]") }
        }
        var h = HandSample()
        if pressed("Button A") { h.buttons |= XRB.lower }
        if pressed("Button B") { h.buttons |= XRB.upper }
        if pressed("Button Menu") { h.buttons |= XRB.menu }
        h.select = pressed("Button Options") || pressed("Button Share")
        if pressed("Thumbstick Button") { h.buttons |= XRB.stickClick }
        if pressed("Button Home") || pressed("Button PS") || pressed("Button PS Button") {
            h.buttons |= XRB.ps
        }
        h.trigger = value("Trigger")
        h.grip = max(value("Grip"), pressed("Grip") ? 1 : 0)
        // The Sense publishes its stick several ways and not every one carries the analog value:
        // the first one that is deflected wins.
        func live(_ v: SIMD2<Float>) -> Bool { v.x * v.x + v.y * v.y > 0.0004 }
        var stick = SIMD2<Float>(0, 0)
        if let d = dp["Thumbstick"], live(SIMD2(d.xAxis.value, d.yAxis.value)) {
            stick = SIMD2(d.xAxis.value, d.yAxis.value)
        } else if let d = dp[Self.isLeft(c) ? "Left Thumbstick" : "Right Thumbstick"],
                  live(SIMD2(d.xAxis.value, d.yAxis.value)) {
            stick = SIMD2(d.xAxis.value, d.yAxis.value)
        } else if live(SIMD2(axis("Thumbstick X Axis"), axis("Thumbstick Y Axis"))) {
            stick = SIMD2(axis("Thumbstick X Axis"), axis("Thumbstick Y Axis"))
        } else {
            stick = SIMD2(value("Thumbstick Right") - value("Thumbstick Left"),
                          value("Thumbstick Up") - value("Thumbstick Down"))
        }
        h.stick = stick
        handCache[key] = h
        return h
    }

    @available(visionOS 26.0, *)
    private func gripPose(hand: Int, time: Double) -> simd_float4x4? {
        lock.lock()
        let stored = accessoryAnchors[hand] as? AccessoryAnchor
        let provider = accessoryProvider as? AccessoryTrackingProvider
        lock.unlock()
        guard let stored, let provider else { return nil }
        let a = provider.predictAnchor(for: stored, at: time) ?? stored
        guard a.isTracked else { return nil }
        if a.accessory.locations.contains(.grip) {
            return a.coordinateSpace(for: .grip, correction: .rendered).ancestorFromSpaceTransformFloat().matrix
        }
        return a.originFromAnchorTransform
    }

    func input(_ time: Double, _ handConnected: UnsafeMutablePointer<Int32>?, _ handButtons: UnsafeMutablePointer<UInt32>?,
               _ handAnalog: UnsafeMutablePointer<Float>?, _ poseValid: UnsafeMutablePointer<Int32>?,
               _ originFromGrip: UnsafeMutablePointer<Float>?, _ padConnected: UnsafeMutablePointer<Int32>?,
               _ padButtons: UnsafeMutablePointer<UInt32>?, _ padAnalog: UnsafeMutablePointer<Float>?) {
        for h in 0..<2 {
            handConnected?[h] = 0
            handButtons?[h] = 0
            poseValid?[h] = 0
            for k in 0..<4 { handAnalog?[h * 4 + k] = 0 }
        }
        padConnected?.pointee = 0
        padButtons?.pointee = 0
        for k in 0..<6 { padAnalog?[k] = 0 }

        let now = CACurrentMediaTime()
        let controllers = GCController.controllers()
        var spatialNow: [GCController] = []
        var selectDown = false
        var senseSelect = false
        for c in controllers {
            capture(c)
            var spatial = false
            if #available(visionOS 26.0, *) { spatial = c.productCategory == GCProductCategorySpatialController }
            if spatial {
                spatialNow.append(c)
                guard #available(visionOS 26.0, *) else { continue }
                let hand = handFor(c)
                let s = senseSample(c)
                selectDown = selectDown || s.select
                senseSelect = senseSelect || s.select
                handConnected?[hand] = 1
                handButtons?[hand] = s.buttons
                handAnalog?[hand * 4 + 0] = s.trigger
                handAnalog?[hand * 4 + 1] = s.grip
                handAnalog?[hand * 4 + 2] = s.stick.x
                handAnalog?[hand * 4 + 3] = s.stick.y
            } else if let g = c.extendedGamepad, padConnected?.pointee == 0 {
                padConnected?.pointee = 1
                var b: UInt32 = 0
                func bit(_ on: Bool, _ flag: UInt32) { if on { b |= flag } }
                bit(g.buttonA.isPressed, GPB.south)
                bit(g.buttonB.isPressed, GPB.east)
                bit(g.buttonX.isPressed, GPB.west)
                bit(g.buttonY.isPressed, GPB.north)
                bit(g.buttonMenu.isPressed, GPB.start)
                bit(g.rightShoulder.isPressed, GPB.rShoulder)
                bit(g.leftShoulder.isPressed, GPB.lShoulder)
                bit(g.leftThumbstickButton?.isPressed ?? false, GPB.lStick)
                bit(g.rightThumbstickButton?.isPressed ?? false, GPB.rStick)
                bit(g.dpad.up.isPressed, GPB.up)
                bit(g.dpad.down.isPressed, GPB.down)
                bit(g.dpad.left.isPressed, GPB.left)
                bit(g.dpad.right.isPressed, GPB.right)
                bit(g.buttonHome?.isPressed ?? false, GPB.home)
                selectDown = selectDown || (g.buttonOptions?.isPressed ?? false)
                padButtons?.pointee = b
                padAnalog?[0] = g.leftThumbstick.xAxis.value
                padAnalog?[1] = g.leftThumbstick.yAxis.value
                padAnalog?[2] = g.rightThumbstick.xAxis.value
                padAnalog?[3] = g.rightThumbstick.yAxis.value
                padAnalog?[4] = g.leftTrigger.value
                padAnalog?[5] = g.rightTrigger.value
            }
        }
        // A stale hold (no sample for a while: paused, app away) does not count.
        if now - lastSelectSample > 0.25 {
            selectHeldSince = 0
            selectFired = false
        }
        lastSelectSample = now
        // Sense Options / Create: Start only as a tap under 0.4 s, sent on release for two samples.
        if startPulse > 0 {
            startPulse -= 1
            handButtons?[1] |= XRB.menu
        }
        // Select held for 3 s toggles the app's panels, once per press.
        if !selectDown {
            if selectHeldSince != 0 && !selectFired && selectFromSense && now - selectHeldSince < 0.4 {
                startPulse = 2
            }
            selectHeldSince = 0
            selectFired = false
        } else if selectHeldSince == 0 {
            selectHeldSince = now
            selectFromSense = senseSelect
        } else if !selectFired && now - selectHeldSince >= 3.0 {
            selectFired = true
            xrLog("[xr] input: Select held 3 s -> toggling the panels")
            svGamePanelsToggle()
        }
        if #available(visionOS 26.0, *) {
            for h in 0..<2 {
                if let m = gripPose(hand: h, time: time) {
                    poseValid?[h] = 1
                    if !poseLogged[h] {
                        poseLogged[h] = true
                        let t = m.columns.3
                        xrLog(String(format: "[xr] input: first %@ controller pose at (%.2f, %.2f, %.2f)",
                                     h == 0 ? "left" : "right", t.x, t.y, t.z))
                    }
                    if let originFromGrip { store(m, originFromGrip + h * 16) }
                }
            }
        }
        // Pairing changes rebuild the accessory tracker (polled once a second).
        if now - lastControllerCheck > 1.0 {
            lastControllerCheck = now
            if spatialChanged(Set(spatialNow.map { ObjectIdentifier($0) })) {
                xrLog("[xr] input: Sense controllers changed (\(spatialNow.count) now); rebuilding the accessory tracker")
                Task { await self.runSession() }
            }
        }
    }

    // MARK: haptics

    private func engine(_ c: GCController) -> CHHapticEngine? {
        let key = ObjectIdentifier(c)
        if let e = engines[key] { return e }
        guard let haptics = c.haptics else {
            if hapticsNoted.insert("none \(c.vendorName ?? "?")").inserted {
                xrLog("[xr] input: \(c.vendorName ?? "controller") has no haptics")
            }
            return nil
        }
        var made = haptics.createEngine(withLocality: .default)
        if made == nil { made = haptics.createEngine(withLocality: .all) }
        guard let e = made else {
            if hapticsNoted.insert("engine \(c.vendorName ?? "?")").inserted {
                xrLog("[xr] input warn: no haptic engine for \(c.vendorName ?? "?")")
            }
            return nil
        }
        e.resetHandler = { [weak e] in try? e?.start() }
        do { try e.start() } catch {
            xrLog("[xr] input warn: haptic engine would not start: \(error)")
            return nil
        }
        engines[key] = e
        xrLog("[xr] input: haptics ready on \(c.vendorName ?? "?")")
        return e
    }

    func rumble(_ amplitude: Float, _ secs: Float) {
        for c in GCController.controllers() {
            let key = ObjectIdentifier(c)
            if secs <= 0 || amplitude <= 0 {
                if let p = players[key] { try? p.stop(atTime: CHHapticTimeImmediate) }
                players[key] = nil
                continue
            }
            guard let e = engine(c) else { continue }
            do {
                let event = CHHapticEvent(eventType: .hapticContinuous, parameters: [
                    CHHapticEventParameter(parameterID: .hapticIntensity, value: min(1, amplitude)),
                    CHHapticEventParameter(parameterID: .hapticSharpness, value: 0.5),
                ], relativeTime: 0, duration: Double(max(secs, 0.03)))
                let p = try e.makeAdvancedPlayer(with: CHHapticPattern(events: [event], parameters: []))
                if let old = players[key] { try? old.stop(atTime: CHHapticTimeImmediate) }
                try p.start(atTime: CHHapticTimeImmediate)
                players[key] = p
            } catch {
                xrLog("[xr] input warn: rumble failed on \(c.vendorName ?? "?"): \(error); engine dropped")
                engines[key] = nil
                players[key] = nil
            }
        }
    }
}

// MARK: - C entry points (sv_xr.h). Game thread.

private func renderer(_ p: UnsafeMutableRawPointer?) -> LayerRenderer? {
    guard let p else { return nil }
    return Unmanaged<LayerRenderer>.fromOpaque(p).takeUnretainedValue()
}

@_cdecl("sv_xrhost_start")
public func sv_xrhost_start(_ log: (@convention(c) (UnsafePointer<CChar>?) -> Void)?) {
    svLogSink = log
    SVXRHost.shared.start()
}

@_cdecl("sv_xrhost_renderer_state")
public func sv_xrhost_renderer_state(_ r: UnsafeMutableRawPointer?) -> Int32 {
    guard let lr = renderer(r) else { return 2 }
    return SVXRHost.shared.rendererState(lr)
}

@_cdecl("sv_xrhost_renderer_device")
public func sv_xrhost_renderer_device(_ r: UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer? {
    guard let lr = renderer(r) else { return nil }
    return SVXRHost.shared.device(lr)
}

@_cdecl("sv_xrhost_renderer_formats")
public func sv_xrhost_renderer_formats(_ r: UnsafeMutableRawPointer?, _ color: UnsafeMutablePointer<UInt32>?,
                                       _ depth: UnsafeMutablePointer<UInt32>?) {
    guard let lr = renderer(r) else { return }
    SVXRHost.shared.formats(lr, color, depth)
}

@_cdecl("sv_xrhost_session_retry")
public func sv_xrhost_session_retry() { SVXRHost.shared.retryIfNeeded() }

@_cdecl("sv_xrhost_frame_begin")
public func sv_xrhost_frame_begin(_ r: UnsafeMutableRawPointer?,
                                  _ device: UnsafeMutablePointer<UnsafeMutableRawPointer?>?) -> Int32 {
    guard let lr = renderer(r) else { return 0 }
    return SVXRHost.shared.frameBegin(lr, device)
}

@_cdecl("sv_xrhost_drawable_info")
public func sv_xrhost_drawable_info(_ d: Int32, _ time: UnsafeMutablePointer<Double>?,
                                    _ range: UnsafeMutablePointer<Float>?, _ views: UnsafeMutablePointer<Int32>?) -> Int32 {
    SVXRHost.shared.drawableInfo(d, time, range, views)
}

@_cdecl("sv_xrhost_frame_view")
public func sv_xrhost_frame_view(_ d: Int32, _ v: Int32, _ deviceFromView: UnsafeMutablePointer<Float>?,
                                 _ projection: UnsafeMutablePointer<Float>?,
                                 _ color: UnsafeMutablePointer<UnsafeMutableRawPointer?>?,
                                 _ depth: UnsafeMutablePointer<UnsafeMutableRawPointer?>?,
                                 _ slice: UnsafeMutablePointer<Int32>?, _ viewport: UnsafeMutablePointer<Float>?) -> Int32 {
    SVXRHost.shared.frameView(d, v, deviceFromView, projection, color, depth, slice, viewport)
}

@_cdecl("sv_xrhost_head")
public func sv_xrhost_head(_ time: Double, _ out: UnsafeMutablePointer<Float>?) -> Int32 {
    SVXRHost.shared.head(time, out)
}

@_cdecl("sv_xrhost_frame_present")
public func sv_xrhost_frame_present(_ d: Int32, _ cmd: UnsafeMutableRawPointer?, _ anchorId: Int32) {
    SVXRHost.shared.present(d, cmd, anchorId)
}

@_cdecl("sv_xrhost_take_nil_anchors")
public func sv_xrhost_take_nil_anchors() -> UInt64 {
    SVXRHost.shared.takeNilAnchorPresents()
}

@_cdecl("sv_xrhost_frame_finish")
public func sv_xrhost_frame_finish() { SVXRHost.shared.finish() }

@_cdecl("sv_xrhost_input")
public func sv_xrhost_input(_ time: Double, _ handConnected: UnsafeMutablePointer<Int32>?,
                            _ handButtons: UnsafeMutablePointer<UInt32>?, _ handAnalog: UnsafeMutablePointer<Float>?,
                            _ poseValid: UnsafeMutablePointer<Int32>?, _ originFromGrip: UnsafeMutablePointer<Float>?,
                            _ padConnected: UnsafeMutablePointer<Int32>?, _ padButtons: UnsafeMutablePointer<UInt32>?,
                            _ padAnalog: UnsafeMutablePointer<Float>?) {
    SVXRHost.shared.input(time, handConnected, handButtons, handAnalog, poseValid, originFromGrip, padConnected,
                          padButtons, padAnalog)
}

@_cdecl("sv_xrhost_hands")
public func sv_xrhost_hands(_ time: Double, _ valid: UnsafeMutablePointer<Int32>?, _ pinch: UnsafeMutablePointer<Int32>?,
                            _ pos: UnsafeMutablePointer<Float>?) {
    SVXRHost.shared.hands(time, valid, pinch, pos)
}

@_cdecl("sv_xrhost_rumble")
public func sv_xrhost_rumble(_ amplitude: Float, _ seconds: Float) {
    SVXRHost.shared.rumble(amplitude, seconds)
}
