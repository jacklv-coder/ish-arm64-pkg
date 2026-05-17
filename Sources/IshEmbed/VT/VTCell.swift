// SPDX-License-Identifier: GPL-3.0-or-later
//
// VTCell — one screen cell. Pure value type, no platform deps.
//

import Foundation

/// One screen cell in a VT grid. ~21 bytes; fits 24×120 in ~60 KiB.
public struct VTCell: Equatable, Sendable {
    /// Unicode scalar value. 0x20 = empty.
    public var scalar: UInt32 = 0x20
    public var fg: VTColor = .default
    public var bg: VTColor = .default
    public var bold: Bool = false
    public var dim: Bool = false
    public var italic: Bool = false
    public var underline: Bool = false
    public var inverse: Bool = false
    public var strike: Bool = false

    public init() {}

    public static let empty = VTCell()

    /// True if every visual attribute except `scalar` matches.
    public func sameAttrs(as other: VTCell) -> Bool {
        return fg == other.fg && bg == other.bg && bold == other.bold &&
               dim == other.dim && italic == other.italic &&
               underline == other.underline && inverse == other.inverse &&
               strike == other.strike
    }

    /// Convenience: the printable Character this cell represents.
    /// Returns " " if the scalar is a control char or invalid.
    public var character: Character {
        if scalar < 0x20 { return " " }
        if let u = Unicode.Scalar(scalar) { return Character(u) }
        return " "
    }
}

/// Color slot inside a cell. `.default` means "use the renderer's
/// foreground/background default" so the consumer (SwiftUI / TUI
/// dumper / web frontend) decides the actual pixel value.
public enum VTColor: Equatable, Sendable {
    case `default`
    case indexed(UInt8)          // 0..15 standard, 16..255 xterm-256
    case rgb(UInt8, UInt8, UInt8)

    /// Resolve to a stable (r,g,b) tuple using a default xterm-256
    /// palette. `.default` is returned as `nil` so callers can decide
    /// what the term "default" means in their renderer.
    public func resolveRGB() -> (UInt8, UInt8, UInt8)? {
        switch self {
        case .default:          return nil
        case .rgb(let r, let g, let b): return (r, g, b)
        case .indexed(let i):   return VTColor.xterm256Palette[Int(i)]
        }
    }

    /// 256-entry xterm palette as raw 0..255 RGB tuples.
    public static let xterm256Palette: [(UInt8, UInt8, UInt8)] = {
        var out: [(UInt8, UInt8, UInt8)] = []
        // 0..15: standard ANSI (low-intensity + high-intensity).
        let base: [(UInt8, UInt8, UInt8)] = [
            (0,0,0), (128,0,0), (0,128,0), (128,128,0),
            (0,0,128), (128,0,128), (0,128,128), (192,192,192),
            (128,128,128), (255,0,0), (0,255,0), (255,255,0),
            (76,76,255), (255,0,255), (0,255,255), (255,255,255),
        ]
        out.append(contentsOf: base)
        // 16..231: 6×6×6 cube
        let levels: [UInt8] = [0, 95, 135, 175, 215, 255]
        for r in 0..<6 { for g in 0..<6 { for b in 0..<6 {
            out.append((levels[r], levels[g], levels[b]))
        }}}
        // 232..255: grayscale ramp
        for i in 0..<24 {
            let v = UInt8(min(255, 8 + i * 10))
            out.append((v, v, v))
        }
        return out
    }()
}
