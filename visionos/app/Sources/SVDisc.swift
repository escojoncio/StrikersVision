// StrikersVision: where the game's files live and which disc image it will play.
//
//   <Documents>/Strikers/            the game's home (STRIKERS_HOME): strikers.ini, the logs, saves,
//                                    and the first place a disc image is looked for. Visible in the
//                                    Files app (UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace).
//   a folder the player picked       anywhere the Files app reaches (iCloud Drive, a USB drive...),
//                                    remembered with a security-scoped bookmark.
//
// The rule for picking one image among several is the port's own (src/platform/dvd.c): the name
// that sorts first, ignoring case.

import Foundation
import Security

enum SVPaths {
    static var documents: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }
    static var home: URL { documents.appendingPathComponent("Strikers", isDirectory: true) }
    static var saves: URL { home.appendingPathComponent("Saves", isDirectory: true) }
    /// Dawn's pipeline cache and the like: rebuildable, so out of the Files app and out of backups.
    static var cache: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Strikers", isDirectory: true)
    }

    /// First launch: the folder and a short note telling the player what goes in it.
    static func prepare() {
        let fm = FileManager.default
        for dir in [home, saves, cache] {
            try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        }
        // Once per install: a note the player deleted stays deleted.
        let noteKey = "sv.discNoteWritten"
        if !UserDefaults.standard.bool(forKey: noteKey) {
            UserDefaults.standard.set(true, forKey: noteKey)
            let note = home.appendingPathComponent(String(localized: "PUT YOUR GAME DISC HERE.txt"))
            let text = String(localized: """
            Super Mario Strikers for Apple Vision Pro

            Copy your own disc image of Super Mario Strikers (GameCube) into this folder.
            Accepted: .iso, .gcm, .rvz, .wia, .ciso, .gcz (also NKit .iso).
            Regions: USA (G4QE01), Europe (G4QP01, "Mario Smash Football"), Japan (G4QJ01).

            No game files come with this app. Use a copy of a disc you own.
            Your saves and the game's log (strikers-log.txt) are kept here too.
            """)
            try? Data(text.utf8).write(to: note)
        }
    }
}

/// One disc image the app found.
struct SVDiscImage: Equatable {
    let url: URL
    let gameID: String?        // G4QE01 / G4QP01 / G4QJ01, when the header could be read cheaply
    let inPickedFolder: Bool

    var name: String { url.lastPathComponent }

    var region: SVRegion? { gameID.flatMap(SVRegion.init(gameID:)) }

    /// The right game at all (only known when the header was read).
    var isStrikers: Bool? { gameID.map { $0.hasPrefix("G4Q") } }
}

enum SVRegion: String {
    case usa = "G4QE01", europe = "G4QP01", japan = "G4QJ01"

    init?(gameID: String) {
        switch gameID.prefix(4) {
        case "G4QE": self = .usa
        case "G4QP": self = .europe
        case "G4QJ": self = .japan
        default: return nil
        }
    }

    /// The game languages this disc carries (main.cpp: the US disc is English only; the European one
    /// follows the console language among five; the Japanese one carries every language).
    var languages: [SVGameLanguage] {
        switch self {
        case .usa: return [.english]
        case .europe: return [.english, .german, .french, .spanish, .italian]
        case .japan: return SVGameLanguage.allCases.filter { $0 != .automatic }
        }
    }

    var displayName: String {
        switch self {
        case .usa: return String(localized: "USA")
        case .europe: return String(localized: "Europe")
        case .japan: return String(localized: "Japan")
        }
    }
}

/// STRIKERS_LANGUAGE (src/platform/os.c port_language).
enum SVGameLanguage: String, CaseIterable, Identifiable {
    case automatic, english, german, french, spanish, italian, japanese
    var id: String { rawValue }

    /// The value for STRIKERS_LANGUAGE, nil = unset (the disc's own default).
    var envValue: String? { self == .automatic ? nil : rawValue }

    /// The system language as one of the game's, if it is one.
    static var system: SVGameLanguage? {
        for preferred in Locale.preferredLanguages {
            switch Locale(identifier: preferred).language.languageCode?.identifier {
            case "en": return .english
            case "de": return .german
            case "fr": return .french
            case "es": return .spanish
            case "it": return .italian
            case "ja": return .japanese
            default: continue
            }
        }
        return nil
    }

    /// What to hand the game. A choice is passed as it is (os.c falls back to the disc's own when the
    /// disc lacks it). Automatic: nothing for the US disc (English only); otherwise the headset's
    /// language when the disc carries it (or when the region is unknown), else English, because a
    /// Japanese disc would otherwise start in Japanese.
    static func resolved(_ choice: SVGameLanguage, region: SVRegion?) -> SVGameLanguage? {
        if choice != .automatic { return choice }
        if region == .usa { return nil }
        if let sys = system, region?.languages.contains(sys) ?? true { return sys }
        return .english
    }
}

enum SVDisc {
    static let extensions: Set<String> = ["iso", "gcm", "ciso", "gcz", "rvz", "wia", "nkit"]

    /// The image the game will play: Documents/Strikers first, then the picked folder.
    static func find(pickedFolder: URL?) -> SVDiscImage? {
        if let image = firstImage(in: SVPaths.home, picked: false) { return image }
        if let pickedFolder, let image = firstImage(in: pickedFolder, picked: true) { return image }
        return nil
    }

    /// Every image in a folder (for the launcher's status line when there are several).
    static func images(in folder: URL) -> [URL] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        return names
            .filter { !$0.hasPrefix(".") && extensions.contains(($0 as NSString).pathExtension.lowercased()) }
            .sorted { $0.caseInsensitiveCompare($1) == .orderedAscending }
            .map { folder.appendingPathComponent($0) }
    }

    private static func firstImage(in folder: URL, picked: Bool) -> SVDiscImage? {
        for url in images(in: folder) {
            var isDir: ObjCBool = false
            guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir), !isDir.boolValue else { continue }
            return SVDiscImage(url: url, gameID: readGameID(url), inPickedFolder: picked)
        }
        return nil
    }

    /// The six-byte game ID from the disc header, for the formats where it is in the clear:
    /// a plain image (.iso/.gcm: offset 0, magic 0xC2339F3D at 0x1C) and Dolphin's .rvz/.wia
    /// (the disc's first 0x80 bytes are stored uncompressed at 0x58: header 1 is 0x48 bytes and
    /// header 2 keeps them at its 0x10). CISO/GCZ would need their block maps: name only.
    static func readGameID(_ url: URL) -> String? {
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        guard let head = try? h.read(upToCount: 0x80), head.count >= 0x60 else { return nil }
        let bytes = [UInt8](head)
        var at: Int?
        if bytes.count >= 0x20, bytes[0x1C] == 0xC2, bytes[0x1D] == 0x33, bytes[0x1E] == 0x9F, bytes[0x1F] == 0x3D {
            at = 0
        } else if (bytes[0..<4] == [0x52, 0x56, 0x5A, 0x01] || bytes[0..<4] == [0x57, 0x49, 0x41, 0x01]),
                  bytes.count >= 0x58 + 0x20,
                  bytes[0x58 + 0x1C] == 0xC2, bytes[0x58 + 0x1D] == 0x33 {
            at = 0x58
        }
        guard let at else { return nil }
        let id = bytes[at..<(at + 6)]
        guard id.allSatisfy({ ($0 >= 0x30 && $0 <= 0x39) || ($0 >= 0x41 && $0 <= 0x5A) }) else { return nil }
        return String(decoding: id, as: UTF8.self)
    }
}

/// The folder the player picked with the system picker, remembered across launches (UserDefaults
/// and the keychain, which outlives a reinstall).
final class SVPickedFolder: @unchecked Sendable {
    static let shared = SVPickedFolder()

    private static let defaultsKey = "sv.discFolderBookmark"
    private static let keychainService = "strikersvision.disc"
    private static let keychainAccount = "folder-bookmark"

    private let lock = NSLock()
    private var opened: URL?

    /// The folder, opened for this process; nil when none was picked or it no longer opens.
    var url: URL? {
        lock.lock(); defer { lock.unlock() }
        if let opened { return opened }
        for (source, data) in [("defaults", UserDefaults.standard.data(forKey: Self.defaultsKey)),
                               ("keychain", Self.keychainBookmark())] {
            guard let data else { continue }
            var stale = false
            guard let resolved = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil,
                                          bookmarkDataIsStale: &stale) else {
                SVLog.log("disc folder: the bookmark in \(source) no longer opens")
                continue
            }
            let accessing = resolved.startAccessingSecurityScopedResource()
            guard accessing || FileManager.default.isReadableFile(atPath: resolved.path) else {
                SVLog.log("disc folder: no access to \(resolved.path)")
                continue
            }
            if stale || source != "defaults",
               let fresh = try? resolved.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
                Self.save(fresh)
            }
            opened = resolved
            SVLog.log("disc folder: \(resolved.path)")
            return resolved
        }
        return nil
    }

    /// The folder from fileImporter. Nil, or why it cannot be used.
    func choose(_ picked: URL) -> String? {
        let accessing = picked.startAccessingSecurityScopedResource()
        guard accessing || FileManager.default.isReadableFile(atPath: picked.path) else {
            return String(localized: "That folder could not be opened.")
        }
        guard let data = try? picked.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) else {
            if accessing { picked.stopAccessingSecurityScopedResource() }
            return String(localized: "That folder could not be remembered.")
        }
        Self.save(data)
        lock.lock()
        opened?.stopAccessingSecurityScopedResource()
        opened = picked
        lock.unlock()
        SVLog.log("disc folder picked: \(picked.path)")
        return nil
    }

    func forget() {
        lock.lock()
        opened?.stopAccessingSecurityScopedResource()
        opened = nil
        lock.unlock()
        UserDefaults.standard.removeObject(forKey: Self.defaultsKey)
        SecItemDelete(Self.keychainQuery() as CFDictionary)
        SVLog.log("disc folder forgotten")
    }

    private static func save(_ bookmark: Data) {
        UserDefaults.standard.set(bookmark, forKey: defaultsKey)
        SecItemDelete(keychainQuery() as CFDictionary)
        var item = keychainQuery()
        item[kSecValueData as String] = bookmark
        item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        SecItemAdd(item as CFDictionary, nil)
    }

    private static func keychainBookmark() -> Data? {
        var query = keychainQuery()
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess else { return nil }
        return result as? Data
    }

    private static func keychainQuery() -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: keychainService,
         kSecAttrAccount as String: keychainAccount]
    }
}
