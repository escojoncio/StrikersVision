// StrikersVision: the room relay link (server/ in this repository), the visionOS twin of NetWs.java.
//
// Same wire as the Quest: one TLS WebSocket to wss://<host>/room/<CODE>, one binary WebSocket
// message per game message, which the relay hands to everyone in the room, the sender included.
// URLSessionWebSocketTask does the TLS (with the system's certificate checks), the upgrade, the
// masking, fragmentation, ping/pong and Happy Eyeballs between IPv4 and IPv6 (NetWs did that last
// one by hand), so what is left here is the inbox and the connection state.

import Foundation

final class SVRelayLink: SVNetLink, URLSessionWebSocketDelegate, @unchecked Sendable {
    private var session: URLSession?
    private var task: URLSessionWebSocketTask?
    private let label: String

    init?(host: String, port: Int, room: String) {
        var parts = URLComponents()
        parts.scheme = "wss"
        parts.host = host
        if port > 0 && port != 443 { parts.port = port }
        parts.path = "/room/\(room)"
        guard let url = parts.url else { return nil }
        label = "\(host)/room/\(room)"
        super.init()

        let configuration = URLSessionConfiguration.ephemeral
        // xr_net.cpp gives the connection 5 s on its own clock. This is only the idle limit, kept long:
        // both headsets can go quiet for a while (a match loads in one blocking frame), and the game
        // judges liveness itself by its echoes.
        configuration.timeoutIntervalForRequest = 120
        configuration.waitsForConnectivity = false
        configuration.httpAdditionalHeaders = ["User-Agent": "StrikersVision"]
        let queue = OperationQueue()
        queue.maxConcurrentOperationCount = 1
        queue.name = "strikers-ws"
        // The session holds its delegate strongly until invalidated; close() invalidates it.
        let session = URLSession(configuration: configuration, delegate: self, delegateQueue: queue)
        var request = URLRequest(url: url)
        request.timeoutInterval = 120
        let task = session.webSocketTask(with: request)
        // The relay drops anything over 4096 bytes; the game's own limit is xr_net's buffer.
        task.maximumMessageSize = SVNetLink.maxMessage
        self.session = session
        self.task = task
        task.resume()
        receiveNext(task)
    }

    private func receiveNext(_ task: URLSessionWebSocketTask) {
        task.receive { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let message):
                switch message {
                case .data(let data):
                    if data.count > SVNetLink.maxMessage {
                        self.fail("a \(data.count)-byte message, more than the game takes")
                        return
                    }
                    self.deliver(data)
                case .string:
                    break  // the relay never sends text; game messages are binary
                @unknown default:
                    break
                }
                if !self.isClosed { self.receiveNext(task) }
            case .failure(let error):
                self.fail("receive: \(error.localizedDescription)")
            }
        }
    }

    private func fail(_ why: String) {
        markClosed("relay \(label): \(why)")
        task?.cancel(with: .goingAway, reason: nil)
        session?.invalidateAndCancel()
    }

    override func send(_ message: Data) -> Bool {
        guard !isClosed, let task else { return false }
        // Queued in call order; a failure ends the link, and xr_net.cpp re-queues and reconnects.
        task.send(.data(message)) { [weak self] error in
            if let error { self?.fail("send: \(error.localizedDescription)") }
        }
        return true
    }

    override func close() {
        guard !isClosed else { return }
        markClosed(nil)
        task?.cancel(with: .normalClosure, reason: nil)
        session?.finishTasksAndInvalidate()
    }

    // MARK: URLSessionWebSocketDelegate

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask,
                    didOpenWithProtocol protocol: String?) {
        setConnected()
    }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask,
                    didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?) {
        // 4000 = the relay closed the oldest socket of a full room (server/src/index.js).
        markClosed("relay \(label): closed by the server (code \(closeCode.rawValue))")
        session.finishTasksAndInvalidate()
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        markClosed(error.map { "relay \(label): \($0.localizedDescription)" })
    }
}
