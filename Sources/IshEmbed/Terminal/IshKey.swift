// SPDX-License-Identifier: GPL-3.0-or-later
//
// IshKey + IshKeyEncoder — platform-neutral keyboard input model.
//
// Consumers describe what the user did using `IshKey` values; the
// encoder converts those into the exact byte sequences a Linux pty
// expects, observing the emulator's current mode (DECCKM application
// cursor keys etc.).
//
// This abstraction means a SwiftUI iOS app, a web-based agent
// frontend, and a tcp-shell-proxy all express keystrokes the same way:
//   try term.send(.key(.up))
//   try term.send(.key(.char("a"), modifiers: [.control]))   // -> 0x01
//   try term.send(.key(.f(5)))                                // -> ESC [ 15 ~
//   try term.send(.text("hello"))                             // -> hello
//

import Foundation

/// One logical keystroke.
public struct IshKey: Hashable, Sendable {
    public enum Code: Hashable, Sendable {
        /// A printable Unicode scalar (or composed sequence) the user
        /// typed. Pass the raw character — encoder will UTF-8 it.
        case char(Character)
        /// Multi-character paste (encoder UTF-8s as a whole).
        case text(String)
        case enter           // CR (0x0D)
        case tab
        case backspace       // DEL (0x7F) by default; see `backspaceIsBS` option
        case escape
        case delete          // forward-delete
        case insert
        case home
        case end
        case pageUp
        case pageDown
        case up
        case down
        case left
        case right
        case f(Int)          // F1..F12 (F13+ also handled but rare)
    }

    public struct Modifiers: OptionSet, Hashable, Sendable {
        public let rawValue: UInt8
        public init(rawValue: UInt8) { self.rawValue = rawValue }
        public static let shift   = Modifiers(rawValue: 1 << 0)
        public static let alt     = Modifiers(rawValue: 1 << 1)
        public static let control = Modifiers(rawValue: 1 << 2)
        public static let meta    = Modifiers(rawValue: 1 << 3)
    }

    public let code: Code
    public let modifiers: Modifiers
    public init(_ code: Code, _ modifiers: Modifiers = []) {
        self.code = code
        self.modifiers = modifiers
    }

    // Convenience constructors
    public static let enter     = IshKey(.enter)
    public static let tab       = IshKey(.tab)
    public static let backspace = IshKey(.backspace)
    public static let escape    = IshKey(.escape)
    public static let up        = IshKey(.up)
    public static let down      = IshKey(.down)
    public static let left      = IshKey(.left)
    public static let right     = IshKey(.right)
    public static let home      = IshKey(.home)
    public static let end       = IshKey(.end)
    public static let pageUp    = IshKey(.pageUp)
    public static let pageDown  = IshKey(.pageDown)
    public static let delete    = IshKey(.delete)
    public static let insert    = IshKey(.insert)
    public static func f(_ n: Int) -> IshKey { IshKey(.f(n)) }
    public static func ctrl(_ ch: Character) -> IshKey { IshKey(.char(ch), .control) }
    public static func alt (_ ch: Character) -> IshKey { IshKey(.char(ch), .alt) }
    public static func char(_ ch: Character) -> IshKey { IshKey(.char(ch)) }
    public static func text(_ s: String)     -> IshKey { IshKey(.text(s)) }
}

/// Tunes how `IshKeyEncoder` translates keys to bytes.
public struct IshKeyEncoderOptions: Sendable {
    /// Send 0x08 (BS) instead of 0x7F (DEL) for Backspace. A few tools
    /// configure their terminfo `kbs` to `^H`; flip this when those
    /// misbehave. Default: false (DEL — Linux default).
    public var backspaceIsBS: Bool = false
    public init() {}
}

/// Stateless encoder for IshKey values. Looks at the emulator to find
/// the current cursor-keys mode so arrow keys produce the right
/// sequence (DECCKM application mode = ESC O X, normal = ESC [ X).
public struct IshKeyEncoder: Sendable {
    public var options: IshKeyEncoderOptions
    public init(options: IshKeyEncoderOptions = .init()) { self.options = options }

    /// Convert `key` to the bytes a Linux pty expects. If `emulator`
    /// is provided, modes like DECCKM are observed.
    public func encode(_ key: IshKey, emulator: VTEmulator? = nil) -> Data {
        let appCursor = emulator?.applicationCursorKeys ?? false
        var bytes = Data()

        // Alt modifier → prefix ESC (xterm meta-sends-escape).
        if key.modifiers.contains(.alt) { bytes.append(0x1B) }

        switch key.code {
        case .char(let ch):
            // Ctrl+letter → 0x01..0x1A; Ctrl+@ … Ctrl+_ similar mapping.
            if key.modifiers.contains(.control) {
                if let mapped = controlByte(for: ch) {
                    bytes.append(mapped); return bytes
                }
            }
            bytes.append(contentsOf: String(ch).utf8)
        case .text(let s):
            bytes.append(contentsOf: s.utf8)
        case .enter:     bytes.append(0x0D)
        case .tab:
            if key.modifiers.contains(.shift) {
                bytes.append(contentsOf: Array("\u{1B}[Z".utf8))
            } else {
                bytes.append(0x09)
            }
        case .backspace:
            bytes.append(options.backspaceIsBS ? 0x08 : 0x7F)
        case .escape:    bytes.append(0x1B)
        case .delete:    bytes.append(contentsOf: Array("\u{1B}[3~".utf8))
        case .insert:    bytes.append(contentsOf: Array("\u{1B}[2~".utf8))
        case .home:      bytes.append(contentsOf: Array("\u{1B}[H".utf8))
        case .end:       bytes.append(contentsOf: Array("\u{1B}[F".utf8))
        case .pageUp:    bytes.append(contentsOf: Array("\u{1B}[5~".utf8))
        case .pageDown:  bytes.append(contentsOf: Array("\u{1B}[6~".utf8))
        case .up:        bytes.append(contentsOf: Array(appCursor ? "\u{1B}OA".utf8 : "\u{1B}[A".utf8))
        case .down:      bytes.append(contentsOf: Array(appCursor ? "\u{1B}OB".utf8 : "\u{1B}[B".utf8))
        case .right:     bytes.append(contentsOf: Array(appCursor ? "\u{1B}OC".utf8 : "\u{1B}[C".utf8))
        case .left:      bytes.append(contentsOf: Array(appCursor ? "\u{1B}OD".utf8 : "\u{1B}[D".utf8))
        case .f(let n):
            bytes.append(contentsOf: fkey(n))
        }
        return bytes
    }

    /// Encode an iterable of keys back-to-back.
    public func encode(_ keys: [IshKey], emulator: VTEmulator? = nil) -> Data {
        var out = Data()
        for k in keys { out.append(encode(k, emulator: emulator)) }
        return out
    }

    private func controlByte(for ch: Character) -> UInt8? {
        guard let s = ch.asciiValue else { return nil }
        // Ctrl+space → NUL; Ctrl+a..z → 0x01..0x1A; Ctrl+@..Ctrl+_ → 0x00..0x1F
        if s >= 0x40 && s <= 0x5F { return s - 0x40 }          // Ctrl+@..Ctrl+_
        if s >= 0x60 && s <= 0x7A { return s - 0x60 }          // Ctrl+a..Ctrl+z (lowercase)
        if s == 0x20 { return 0x00 }                            // Ctrl+space
        if s == 0x3F { return 0x7F }                            // Ctrl+? -> DEL
        return nil
    }

    private func fkey(_ n: Int) -> [UInt8] {
        switch n {
        case 1:  return Array("\u{1B}OP".utf8)
        case 2:  return Array("\u{1B}OQ".utf8)
        case 3:  return Array("\u{1B}OR".utf8)
        case 4:  return Array("\u{1B}OS".utf8)
        case 5:  return Array("\u{1B}[15~".utf8)
        case 6:  return Array("\u{1B}[17~".utf8)
        case 7:  return Array("\u{1B}[18~".utf8)
        case 8:  return Array("\u{1B}[19~".utf8)
        case 9:  return Array("\u{1B}[20~".utf8)
        case 10: return Array("\u{1B}[21~".utf8)
        case 11: return Array("\u{1B}[23~".utf8)
        case 12: return Array("\u{1B}[24~".utf8)
        default: return []
        }
    }
}
