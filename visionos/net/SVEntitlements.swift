// StrikersVision: which entitlements this process was really signed with.
//
// The owner installs with a free Apple ID (SideStore and similar), and that signing keeps only the
// entitlements a free account may have: com.apple.developer.group-session is not one of them. The
// framework does not say so; a missing entitlement just means no session ever arrives. So the app
// reads its own code signature: the entitlements blob of LC_CODE_SIGNATURE in the main executable.
// There is no public API for this on visionOS (SecTaskCopyValueForEntitlement is macOS-only).

import Foundation

enum SVEntitlements {
    /// The signed entitlements, or nil when they could not be read (then features are tried anyway).
    static let signed: [String: Any]? = readSigned()

    static var hasGroupSession: Bool {
        guard let signed else { return true }  // unknown: try, and let SharePlay fail softly
        return (signed["com.apple.developer.group-session"] as? Bool) == true
    }

    private static func readSigned() -> [String: Any]? {
        guard let url = Bundle.main.executableURL,
              let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        guard let head = try? handle.read(upToCount: 32), head.count == 32 else { return nil }
        let magic = head.loadLE32(0)
        guard magic == 0xfeedfacf else { return nil }  // MH_MAGIC_64: a visionOS executable is thin arm64
        let ncmds = Int(head.loadLE32(16))
        let sizeofcmds = Int(head.loadLE32(20))
        guard sizeofcmds > 0, sizeofcmds < 16 << 20,
              let cmds = try? handle.read(upToCount: sizeofcmds), cmds.count == sizeofcmds else { return nil }
        var at = 0
        for _ in 0..<ncmds {
            guard at + 8 <= cmds.count else { return nil }
            let cmd = cmds.loadLE32(at)
            let size = Int(cmds.loadLE32(at + 4))
            if cmd == 0x1d /* LC_CODE_SIGNATURE */, at + 16 <= cmds.count {
                let offset = UInt64(cmds.loadLE32(at + 8))
                let length = Int(cmds.loadLE32(at + 12))
                return entitlements(handle, offset: offset, length: length)
            }
            guard size >= 8 else { return nil }
            at += size
        }
        return nil
    }

    /// The code signature SuperBlob is big-endian: magic 0xfade0cc0, length, count, then
    /// (type, offset) pairs; slot 5 holds the XML entitlements blob (magic 0xfade7171).
    private static func entitlements(_ handle: FileHandle, offset: UInt64, length: Int) -> [String: Any]? {
        guard length > 12, length < 64 << 20,
              (try? handle.seek(toOffset: offset)) != nil,
              let blob = try? handle.read(upToCount: length), blob.count == length,
              blob.loadBE32(0) == 0xfade0cc0 else { return nil }
        let count = Int(blob.loadBE32(8))
        for i in 0..<count {
            let entry = 12 + i * 8
            guard entry + 8 <= blob.count else { return nil }
            let type = blob.loadBE32(entry)
            let at = Int(blob.loadBE32(entry + 4))
            guard type == 5, at + 8 <= blob.count, blob.loadBE32(at) == 0xfade7171 else { continue }
            let size = Int(blob.loadBE32(at + 4))
            guard size > 8, at + size <= blob.count else { return nil }
            let xml = blob.subdata(in: (at + 8)..<(at + size))
            return (try? PropertyListSerialization.propertyList(from: xml, format: nil)) as? [String: Any]
        }
        // No XML entitlements slot: either none at all or only the DER form. Unknown, not "none".
        return nil
    }
}

private extension Data {
    func loadLE32(_ at: Int) -> UInt32 {
        var v: UInt32 = 0
        for i in 0..<4 { v |= UInt32(self[self.startIndex + at + i]) << (8 * i) }
        return v
    }

    func loadBE32(_ at: Int) -> UInt32 {
        var v: UInt32 = 0
        for i in 0..<4 { v = (v << 8) | UInt32(self[self.startIndex + at + i]) }
        return v
    }
}
