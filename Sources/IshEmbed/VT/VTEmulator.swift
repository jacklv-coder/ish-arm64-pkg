// SPDX-License-Identifier: GPL-3.0-or-later
//
// VTEmulator — minimal but functional xterm-style VT terminal driver.
// Pure logic, no platform deps. Feed bytes in, read the cell grid out.
//
// Implements:
//  - VT100/VT102 cursor positioning, erase, scroll
//  - Alt-screen + main buffer with optional main-buffer scrollback
//  - SGR (CSI m) including 16-color, xterm-256 and truecolor (38;2;...)
//  - DEC private modes (?1 DECCKM, ?7 DECAWM, ?25 cursor, ?47/?1049 alt)
//  - DSR cursor position report + primary DA
//  - OSC (consumed and discarded — title etc. — we don't surface those)
//
// What it deliberately doesn't implement:
//  - DCS / SOS / PM
//  - Mouse reporting modes
//  - Sixel / Kitty graphics
//  - Tab stop manipulation
//
// All bytes the emulator wants to send back to the guest (cursor
// position reports, DA replies) accumulate in `pendingReply`. The
// owner of the emulator should drain it after every `feed(_:)` and
// pipe the bytes to the pty.
//

import Foundation

public final class VTEmulator: @unchecked Sendable {
    public enum BufferKind: Sendable { case main, alt }

    // MARK: state

    public private(set) var rows: Int
    public private(set) var cols: Int
    public let main: VTBuffer
    public let alt: VTBuffer
    public private(set) var active: BufferKind = .main
    public var activeBuffer: VTBuffer { active == .main ? main : alt }

    /// Cursor row (0-indexed).
    public private(set) var row: Int = 0
    /// Cursor column (0-indexed).
    public private(set) var col: Int = 0
    public private(set) var cursorVisible: Bool = true

    /// Active pen (SGR state) applied to newly-printed cells.
    private var pen = VTCell()

    /// Scroll region (DECSTBM), 0-indexed inclusive.
    private var scrollTop: Int = 0
    private var scrollBottom: Int = 0

    /// True if the previous `putChar` overflowed the rightmost column
    /// and a subsequent printable char must wrap.
    private var wrapPending: Bool = false

    // Modes
    public private(set) var autoWrap: Bool = true
    public private(set) var bracketedPaste: Bool = false
    public private(set) var applicationCursorKeys: Bool = false

    // Parser state machine
    private enum Parser {
        case ground
        case esc
        case csi
        case osc
        case oscEsc
        case charset
    }
    private var parser: Parser = .ground
    private var csiPriv: Character? = nil
    private var csiParams: [Int] = []
    private var csiCurrentParam: Int? = nil
    private var oscBuffer: String = ""

    /// Bytes the emulator wants to write to the pty (CPR, DA, …).
    /// Owner drains via `drainReply()`.
    public private(set) var pendingReply = Data()

    /// Monotonic generation counter. Bumped on every `feed(_:)` that
    /// produces visible state changes. Useful as a SwiftUI / KVO trigger.
    public private(set) var generation: UInt64 = 0

    // MARK: init / lifecycle

    public init(rows: Int = 24, cols: Int = 80) {
        self.rows = rows
        self.cols = cols
        self.main = VTBuffer(rows: rows, cols: cols)
        self.alt = VTBuffer(rows: rows, cols: cols)
        self.scrollBottom = rows - 1
        // Only main keeps scrollback; alt-screen consumers (less, vim,
        // htop and other full-screen tools) own their own.
        self.main.keepScrollback = true
    }

    public func resize(rows newRows: Int, cols newCols: Int) {
        let r = max(1, newRows)
        let c = max(1, newCols)
        guard r != rows || c != cols else { return }
        rows = r
        cols = c
        main.resize(rows: r, cols: c)
        alt.resize(rows: r, cols: c)
        scrollTop = 0
        scrollBottom = r - 1
        if row >= rows { row = rows - 1 }
        if col >= cols { col = cols - 1 }
        wrapPending = false
        generation &+= 1
    }

    /// Drain pending bytes the emulator wants to send to the guest.
    public func drainReply() -> Data {
        let out = pendingReply
        pendingReply.removeAll(keepingCapacity: true)
        return out
    }

    // MARK: feed

    /// Feed UTF-8 bytes from the guest. Lossy-decodes to Unicode
    /// scalars (replacement on invalid bytes).
    public func feed(_ data: Data) {
        let s = String(data: data, encoding: .utf8)
              ?? String(decoding: data, as: UTF8.self)
        for sc in s.unicodeScalars { feedScalar(sc) }
        generation &+= 1
    }

    private func feedScalar(_ s: Unicode.Scalar) {
        switch parser {
        case .ground:  handleGround(s)
        case .esc:     handleEsc(s)
        case .csi:     handleCSI(s)
        case .osc:     handleOSC(s)
        case .oscEsc:  parser = .ground
        case .charset: parser = .ground
        }
    }

    private func handleGround(_ s: Unicode.Scalar) {
        switch s.value {
        case 0x07: return                                  // BEL
        case 0x08: cursorBackspace()                       // BS
        case 0x09: cursorTab()                             // HT
        case 0x0A, 0x0B, 0x0C: lineFeed()                  // LF/VT/FF
        case 0x0D: col = 0; wrapPending = false            // CR
        case 0x1B: parser = .esc                           // ESC
        case 0x00...0x1F, 0x7F: return                     // ignore other C0+DEL
        default: putChar(s)
        }
    }

    private func handleEsc(_ s: Unicode.Scalar) {
        switch s.value {
        case 0x5B: parser = .csi; csiPriv = nil; csiParams.removeAll(keepingCapacity: true); csiCurrentParam = nil
        case 0x5D: parser = .osc; oscBuffer.removeAll(keepingCapacity: true)
        case 0x28, 0x29, 0x2A, 0x2B: parser = .charset
        case 0x37: activeBuffer.savedCursor = (row, col); parser = .ground
        case 0x38: if let sc = activeBuffer.savedCursor { row = sc.row; col = sc.col }; parser = .ground; wrapPending = false
        case 0x44: lineFeed();         parser = .ground   // IND
        case 0x45: col = 0; lineFeed(); parser = .ground  // NEL
        case 0x4D: reverseLineFeed();  parser = .ground   // RI
        case 0x63: reset();            parser = .ground   // RIS
        default:   parser = .ground
        }
    }

    private func handleCSI(_ s: Unicode.Scalar) {
        let v = s.value
        if csiParams.isEmpty && csiCurrentParam == nil &&
           (v == 0x3F || v == 0x3E || v == 0x3C || v == 0x3D) {
            csiPriv = Character(s); return
        }
        if v >= 0x30 && v <= 0x39 {
            csiCurrentParam = (csiCurrentParam ?? 0) * 10 + Int(v - 0x30); return
        }
        if v == 0x3B {
            csiParams.append(csiCurrentParam ?? 0)
            csiCurrentParam = nil; return
        }
        if v >= 0x20 && v <= 0x2F { return }              // intermediate
        if v >= 0x40 && v <= 0x7E {
            if let p = csiCurrentParam { csiParams.append(p); csiCurrentParam = nil }
            executeCSI(final: s)
            parser = .ground
            return
        }
        parser = .ground
    }

    private func handleOSC(_ s: Unicode.Scalar) {
        switch s.value {
        case 0x07: parser = .ground                       // BEL terminator
        case 0x1B: parser = .oscEsc                       // ESC \\ (ST) starts
        default:   if oscBuffer.count < 512 { oscBuffer.unicodeScalars.append(s) }
        }
    }

    // MARK: CSI dispatch

    private func executeCSI(final f: Unicode.Scalar) {
        let priv = csiPriv
        let p = csiParams
        func p0(_ i: Int, default def: Int = 1) -> Int { i < p.count ? max(p[i], def) : def }
        func p0z(_ i: Int) -> Int { i < p.count ? p[i] : 0 }

        switch f.value {
        case 0x41: row = max(scrollTop,    row - p0(0)); wrapPending = false                       // CUU
        case 0x42: row = min(scrollBottom, row + p0(0)); wrapPending = false                       // CUD
        case 0x43: col = min(cols - 1,     col + p0(0)); wrapPending = false                       // CUF
        case 0x44: col = max(0,            col - p0(0)); wrapPending = false                       // CUB
        case 0x45: col = 0; row = min(scrollBottom, row + p0(0)); wrapPending = false              // CNL
        case 0x46: col = 0; row = max(scrollTop,    row - p0(0)); wrapPending = false              // CPL
        case 0x47: col = max(0, min(cols - 1, p0(0) - 1)); wrapPending = false                     // CHA
        case 0x48, 0x66:                                                                            // CUP / HVP
            let r = max(0, min(rows - 1, p0(0) - 1))
            let c = max(0, min(cols - 1, p0(1) - 1))
            row = r; col = c; wrapPending = false

        case 0x4A:                                                                                  // ED
            switch p0z(0) {
            case 0: eraseInDisplay(from: (row, col), to: (rows - 1, cols - 1))
            case 1: eraseInDisplay(from: (0, 0),     to: (row, col))
            case 2, 3: activeBuffer.clear()
            default: break
            }

        case 0x4B:                                                                                  // EL
            switch p0z(0) {
            case 0: for c in col..<cols   { activeBuffer.setCell(row, c, blankWithPen()) }
            case 1: for c in 0...col      { activeBuffer.setCell(row, c, blankWithPen()) }
            case 2: for c in 0..<cols     { activeBuffer.setCell(row, c, blankWithPen()) }
            default: break
            }

        case 0x4C:                                                                                  // IL
            if row >= scrollTop && row <= scrollBottom {
                activeBuffer.scrollDown(top: row, bottom: scrollBottom, n: p0(0), blank: blankWithPen())
            }
        case 0x4D:                                                                                  // DL
            if row >= scrollTop && row <= scrollBottom {
                activeBuffer.scrollUp(top: row, bottom: scrollBottom, n: p0(0), blank: blankWithPen())
            }

        case 0x50:                                                                                  // DCH
            let n = min(p0(0), cols - col)
            for c in col..<(cols - n) { activeBuffer.setCell(row, c, activeBuffer.cell(row, c + n)) }
            for c in (cols - n)..<cols { activeBuffer.setCell(row, c, blankWithPen()) }
        case 0x40:                                                                                  // ICH
            let n = min(p0(0), cols - col)
            var c = cols - 1
            while c >= col + n { activeBuffer.setCell(row, c, activeBuffer.cell(row, c - n)); c -= 1 }
            for c in col..<(col + n) { activeBuffer.setCell(row, c, blankWithPen()) }

        case 0x58:                                                                                  // ECH
            let n = min(p0(0), cols - col)
            for c in col..<(col + n) { activeBuffer.setCell(row, c, blankWithPen()) }

        case 0x53: activeBuffer.scrollUp(top: scrollTop, bottom: scrollBottom, n: p0(0), blank: blankWithPen())    // SU
        case 0x54: activeBuffer.scrollDown(top: scrollTop, bottom: scrollBottom, n: p0(0), blank: blankWithPen())  // SD

        case 0x64: row = max(0, min(rows - 1, p0(0) - 1)); wrapPending = false                     // VPA
        case 0x6D: applySGR(p)                                                                      // SGR

        case 0x72:                                                                                  // DECSTBM
            let top = max(0, p0(0) - 1)
            let bot = min(rows - 1, (p.count >= 2 ? p[1] : rows) - 1)
            if top < bot { scrollTop = top; scrollBottom = bot }
            else         { scrollTop = 0;   scrollBottom = rows - 1 }
            row = scrollTop; col = 0; wrapPending = false

        case 0x68: applyMode(p, enable: true,  isPrivate: priv == "?")                              // SM / DECSET
        case 0x6C: applyMode(p, enable: false, isPrivate: priv == "?")                              // RM / DECRST

        case 0x73: activeBuffer.savedCursor = (row, col)                                            // save
        case 0x75: if let sc = activeBuffer.savedCursor { row = sc.row; col = sc.col; wrapPending = false } // restore

        case 0x6E:                                                                                  // DSR
            if p0z(0) == 6 {
                let resp = "\u{1B}[\(row + 1);\(col + 1)R"
                pendingReply.append(resp.data(using: .utf8) ?? Data())
            } else if p0z(0) == 5 {
                pendingReply.append("\u{1B}[0n".data(using: .utf8) ?? Data())
            }
        case 0x63:                                                                                  // DA
            pendingReply.append("\u{1B}[?6c".data(using: .utf8) ?? Data())

        default: break
        }
    }

    // MARK: cursor / write primitives

    private func putChar(_ s: Unicode.Scalar) {
        if wrapPending && autoWrap {
            col = 0
            lineFeed()
            wrapPending = false
        }
        var cell = pen
        cell.scalar = s.value
        activeBuffer.setCell(row, col, cell)
        if col + 1 >= cols {
            if autoWrap { wrapPending = true }
            else        { col = cols - 1 }
        } else {
            col += 1
        }
    }

    private func cursorBackspace() {
        if col > 0 { col -= 1 }
        wrapPending = false
    }

    private func cursorTab() {
        let next = ((col / 8) + 1) * 8
        col = min(cols - 1, next)
        wrapPending = false
    }

    private func lineFeed() {
        if row == scrollBottom {
            activeBuffer.scrollUp(top: scrollTop, bottom: scrollBottom, blank: blankWithPen())
        } else if row < rows - 1 {
            row += 1
        }
        wrapPending = false
    }

    private func reverseLineFeed() {
        if row == scrollTop {
            activeBuffer.scrollDown(top: scrollTop, bottom: scrollBottom, blank: blankWithPen())
        } else if row > 0 {
            row -= 1
        }
        wrapPending = false
    }

    private func eraseInDisplay(from a: (Int, Int), to b: (Int, Int)) {
        var (sr, sc) = a
        let (er, ec) = b
        while sr < er {
            for c in sc..<cols { activeBuffer.setCell(sr, c, blankWithPen()) }
            sc = 0
            sr += 1
        }
        for c in sc...ec where c >= 0 && c < cols {
            activeBuffer.setCell(sr, c, blankWithPen())
        }
    }

    private func blankWithPen() -> VTCell {
        var b = VTCell()
        b.fg = pen.fg
        b.bg = pen.bg
        return b
    }

    public func reset() {
        row = 0; col = 0
        pen = VTCell()
        scrollTop = 0; scrollBottom = rows - 1
        autoWrap = true; wrapPending = false; cursorVisible = true
        main.clear(); alt.clear()
        active = .main
        pendingReply.removeAll()
        generation &+= 1
    }

    // MARK: SGR

    private func applySGR(_ params: [Int]) {
        let ps = params.isEmpty ? [0] : params
        var i = 0
        while i < ps.count {
            let p = ps[i]
            switch p {
            case 0:  pen = VTCell()
            case 1:  pen.bold = true
            case 2:  pen.dim = true
            case 3:  pen.italic = true
            case 4:  pen.underline = true
            case 7:  pen.inverse = true
            case 9:  pen.strike = true
            case 22: pen.bold = false; pen.dim = false
            case 23: pen.italic = false
            case 24: pen.underline = false
            case 27: pen.inverse = false
            case 29: pen.strike = false
            case 30...37: pen.fg = .indexed(UInt8(p - 30))
            case 38:
                if let (color, consumed) = parseExtendedColor(ps, from: i) {
                    pen.fg = color; i += consumed; continue
                }
            case 39: pen.fg = .default
            case 40...47: pen.bg = .indexed(UInt8(p - 40))
            case 48:
                if let (color, consumed) = parseExtendedColor(ps, from: i) {
                    pen.bg = color; i += consumed; continue
                }
            case 49: pen.bg = .default
            case 90...97:   pen.fg = .indexed(UInt8(p - 90 + 8))
            case 100...107: pen.bg = .indexed(UInt8(p - 100 + 8))
            default: break
            }
            i += 1
        }
    }

    /// Parses CSI 38;5;N or 38;2;R;G;B starting at `start`.
    /// Returns (color, params-consumed-from-start) or nil.
    private func parseExtendedColor(_ ps: [Int], from start: Int) -> (VTColor, Int)? {
        guard start + 1 < ps.count else { return nil }
        let mode = ps[start + 1]
        if mode == 5, start + 2 < ps.count {
            let v = max(0, min(255, ps[start + 2]))
            return (.indexed(UInt8(v)), 3)
        }
        if mode == 2, start + 4 < ps.count {
            let r = UInt8(max(0, min(255, ps[start + 2])))
            let g = UInt8(max(0, min(255, ps[start + 3])))
            let b = UInt8(max(0, min(255, ps[start + 4])))
            return (.rgb(r, g, b), 5)
        }
        return nil
    }

    // MARK: modes

    private func applyMode(_ params: [Int], enable: Bool, isPrivate: Bool) {
        guard isPrivate else { return }
        for p in params {
            switch p {
            case 1:    applicationCursorKeys = enable
            case 7:    autoWrap = enable
            case 25:   cursorVisible = enable
            case 47, 1047: switchBuffer(toAlt: enable, clearOnEnter: false)
            case 1048:
                if enable { activeBuffer.savedCursor = (row, col) }
                else if let sc = activeBuffer.savedCursor { row = sc.row; col = sc.col }
            case 1049:
                if enable {
                    main.savedCursor = (row, col)
                    switchBuffer(toAlt: true, clearOnEnter: true)
                } else {
                    switchBuffer(toAlt: false, clearOnEnter: false)
                    if let sc = main.savedCursor { row = sc.row; col = sc.col }
                }
            case 2004: bracketedPaste = enable
            default:   break
            }
        }
    }

    private func switchBuffer(toAlt: Bool, clearOnEnter: Bool) {
        let target: BufferKind = toAlt ? .alt : .main
        if target == active { return }
        active = target
        if clearOnEnter { activeBuffer.clear() }
        wrapPending = false
    }
}
