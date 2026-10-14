// StrikersVision: the sv_net_* functions of sv_bridge.h, called by smstrikers-port's xr_net.cpp
// from its network thread (the role NetWs.java plays on the Quest).
//
// Handle-based and non-blocking, like NetWs: every call returns at once, messages arrive whole on
// an inbox, and a handle whose link died reports -1 once its inbox is empty, so the C side
// reconnects. Two kinds of link sit behind a handle: the WebSocket relay (SVRelayLink) and the
// SharePlay group (SVSharePlayLink, when the host is SV_NET_SHAREPLAY).

import Foundation

/// What a link has in common: an inbox of whole messages and its connected / closed state.
/// Thread-safe: the C network thread, URLSession's delegate queue and the main actor all touch it.
class SVNetLink: NSObject, @unchecked Sendable {
    /// xr_net.cpp's receive buffer; anything bigger would be cut and read as a different message.
    static let maxMessage = 64 * 1024
    /// xr_net.cpp keeps at most 8192 received messages too; beyond that the oldest go.
    private static let maxInbox = 8192

    let lock = NSLock()
    private var inbox: [Data] = []
    private var inboxHead = 0
    private var connected = false
    private var closed = false

    var isConnected: Bool {
        lock.lock(); defer { lock.unlock() }
        return connected && !closed
    }

    var isClosed: Bool {
        lock.lock(); defer { lock.unlock() }
        return closed
    }

    /// Called with the lock NOT held.
    func setConnected() {
        lock.lock(); defer { lock.unlock() }
        if !closed { connected = true }
    }

    func markClosed(_ why: String?) {
        lock.lock()
        let was = closed
        closed = true
        connected = false
        lock.unlock()
        if !was, let why { SVNetLog.log("net link closed: \(why)") }
    }

    func deliver(_ message: Data) {
        lock.lock(); defer { lock.unlock() }
        if closed { return }
        inbox.append(message)
        if inbox.count - inboxHead > Self.maxInbox { inboxHead += 1 }
        // Compact now and then so the array does not grow without bound.
        if inboxHead > 1024 && inboxHead * 2 > inbox.count {
            inbox.removeFirst(inboxHead)
            inboxHead = 0
        }
    }

    /// sv_net_recv: size copied, 0 nothing pending, -1 gone, -2 buffer too small (message kept).
    func receive(into buffer: UnsafeMutableRawPointer, capacity: Int) -> Int32 {
        lock.lock(); defer { lock.unlock() }
        if inboxHead < inbox.count {
            let message = inbox[inboxHead]
            if message.count > capacity { return -2 }
            message.withUnsafeBytes { raw in
                if let base = raw.baseAddress, message.count > 0 {
                    buffer.copyMemory(from: base, byteCount: message.count)
                }
            }
            inboxHead += 1
            if inboxHead == inbox.count {
                inbox.removeAll(keepingCapacity: true)
                inboxHead = 0
            }
            return Int32(message.count)
        }
        // The last messages are handed out before a dead link reports itself gone (NetWs did the same).
        return closed ? -1 : 0
    }

    /// Subclasses: queue one message. False when the link is gone.
    func send(_ message: Data) -> Bool { false }

    /// Subclasses: end the link (idempotent).
    func close() { markClosed(nil) }
}

/// The handle table.
enum SVNetRegistry {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var links: [Int32: SVNetLink] = [:]
    nonisolated(unsafe) private static var nextHandle: Int32 = 1

    static func add(_ link: SVNetLink) -> Int32 {
        lock.lock(); defer { lock.unlock() }
        let handle = nextHandle
        nextHandle = nextHandle == Int32.max ? 1 : nextHandle + 1
        links[handle] = link
        return handle
    }

    static func link(_ handle: Int32) -> SVNetLink? {
        lock.lock(); defer { lock.unlock() }
        return links[handle]
    }

    static func remove(_ handle: Int32) -> SVNetLink? {
        lock.lock(); defer { lock.unlock() }
        return links.removeValue(forKey: handle)
    }

    /// Every open SharePlay link (the group's messages go to all of them).
    static var sharePlayLinks: [SVSharePlayLink] {
        lock.lock(); defer { lock.unlock() }
        return links.values.compactMap { $0 as? SVSharePlayLink }
    }
}

/// The network side's log lines: into the app log (SVLog lives in visionos/app). A function
/// variable rather than a direct call keeps this folder free of app types.
enum SVNetLog {
    nonisolated(unsafe) static var sink: (@Sendable (String) -> Void)?
    static func log(_ message: String) {
        if let sink { sink(message) } else { NSLog("[net] %@", message) }
    }
}

/// A room code as xr_net.cpp's sanitize_room makes it: 3-8 letters or digits, upper case.
func svSanitizeRoom(_ raw: String) -> String {
    var out = ""
    for c in raw.uppercased() where out.count < 8 {
        if c.isASCII, c.isLetter || c.isNumber { out.append(c) }
    }
    return out.count >= 3 ? out : ""
}

// MARK: - The C entry points (sv_bridge.h)

@_cdecl("sv_net_open")
public func svNetOpen(_ host: UnsafePointer<CChar>?, _ port: Int32, _ room: UnsafePointer<CChar>?,
                      _ flags: Int32) -> Int32 {
    guard let host else { return -1 }
    let hostName = String(cString: host)
    let code = svSanitizeRoom(room.map { String(cString: $0) } ?? "")
    if hostName == SV_NET_SHAREPLAY {
        guard let link = SVSharePlayLink.open() else { return -1 }
        return SVNetRegistry.add(link)
    }
    guard !code.isEmpty, let link = SVRelayLink(host: hostName, port: Int(port), room: code) else { return -1 }
    return SVNetRegistry.add(link)
}

@_cdecl("sv_net_send")
public func svNetSend(_ handle: Int32, _ data: UnsafeRawPointer?, _ size: Int32) -> Int32 {
    guard let link = SVNetRegistry.link(handle), size >= 0 else { return 0 }
    let message = (data != nil && size > 0) ? Data(bytes: data!, count: Int(size)) : Data()
    return link.send(message) ? 1 : 0
}

@_cdecl("sv_net_recv")
public func svNetRecv(_ handle: Int32, _ buffer: UnsafeMutableRawPointer?, _ capacity: Int32) -> Int32 {
    guard let link = SVNetRegistry.link(handle) else { return -1 }
    guard let buffer, capacity > 0 else { return -2 }
    return link.receive(into: buffer, capacity: Int(capacity))
}

@_cdecl("sv_net_close")
public func svNetClose(_ handle: Int32) {
    SVNetRegistry.remove(handle)?.close()
}

@_cdecl("sv_net_connected")
public func svNetConnected(_ handle: Int32) -> Int32 {
    guard let link = SVNetRegistry.link(handle) else { return 0 }
    return link.isConnected ? 1 : 0
}
