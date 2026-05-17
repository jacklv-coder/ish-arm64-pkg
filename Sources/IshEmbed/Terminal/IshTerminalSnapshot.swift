// SPDX-License-Identifier: GPL-3.0-or-later
//
// IshTerminalSnapshot — immutable snapshot of an IshTerminal's screen.
// Cheap to pass between threads / actors. Includes:
//   - scrollback rows + on-screen rows (typed `[VTCell]`)
//   - cursor position
//   - active buffer kind
//   - convenience plain-text renderer (no platform deps)
//
// Renderers that want attributed/styled output (SwiftUI, AppKit,
// HTML, ANSI passthrough) iterate over the rows themselves; the
// snapshot just hands them VTCells.
//

import Foundation

public struct IshTerminalSnapshot: Sendable {
    /// Width in cells (columns).
    public let cols: Int
    /// On-screen rows count (height of the live grid).
    public let screenRows: Int
    /// Cursor position (row is relative to the live screen, 0-indexed).
    public let cursorRow: Int
    public let cursorCol: Int
    public let cursorVisible: Bool
    public let activeBuffer: VTEmulator.BufferKind
    /// Older lines that scrolled off the main screen. Empty when on
    /// alt-screen. Oldest-first.
    public let scrollback: [[VTCell]]
    /// The current screen rows, top to bottom.
    public let screen: [[VTCell]]
    /// Monotonic generation counter from the underlying emulator at
    /// the time the snapshot was made.
    public let generation: UInt64

    public init(cols: Int, screenRows: Int, cursorRow: Int, cursorCol: Int,
                cursorVisible: Bool, activeBuffer: VTEmulator.BufferKind,
                scrollback: [[VTCell]], screen: [[VTCell]], generation: UInt64) {
        self.cols = cols
        self.screenRows = screenRows
        self.cursorRow = cursorRow
        self.cursorCol = cursorCol
        self.cursorVisible = cursorVisible
        self.activeBuffer = activeBuffer
        self.scrollback = scrollback
        self.screen = screen
        self.generation = generation
    }

    /// Plain-text dump of (scrollback + screen). Trailing whitespace on
    /// each row is stripped. Useful for copy-to-clipboard, log capture,
    /// piping to other tools.
    public func plainText() -> String {
        var rows: [String] = []
        rows.reserveCapacity(scrollback.count + screen.count)
        func appendRow(_ cells: [VTCell]) {
            var line = ""
            line.reserveCapacity(cells.count)
            for cell in cells {
                if cell.scalar < 0x20 { line.append(" ") }
                else if let u = Unicode.Scalar(cell.scalar) { line.append(Character(u)) }
                else { line.append(" ") }
            }
            while let last = line.last, last == " " { line.removeLast() }
            rows.append(line)
        }
        for r in scrollback { appendRow(r) }
        for r in screen     { appendRow(r) }
        while let last = rows.last, last.isEmpty { rows.removeLast() }
        return rows.joined(separator: "\n")
    }

    /// Just the current screen (no scrollback). Useful for fixed-grid
    /// renderers that want what the user sees right now.
    public func screenText() -> String {
        var rows: [String] = []
        rows.reserveCapacity(screen.count)
        for cells in screen {
            var line = ""
            line.reserveCapacity(cells.count)
            for cell in cells {
                if cell.scalar < 0x20 { line.append(" ") }
                else if let u = Unicode.Scalar(cell.scalar) { line.append(Character(u)) }
                else { line.append(" ") }
            }
            rows.append(line)
        }
        return rows.joined(separator: "\n")
    }
}

public extension VTEmulator {
    /// Capture the current state into a thread-safe snapshot. Cheap:
    /// shallow-copies the cell arrays, no string rendering until the
    /// consumer asks for it.
    func snapshot() -> IshTerminalSnapshot {
        let b = activeBuffer
        var screen: [[VTCell]] = []
        screen.reserveCapacity(b.rows)
        for r in 0..<b.rows {
            var row = [VTCell](repeating: .empty, count: b.cols)
            for c in 0..<b.cols { row[c] = b.cells[r * b.cols + c] }
            screen.append(row)
        }
        let sb = active == .main ? b.scrollback : []
        return IshTerminalSnapshot(
            cols: b.cols,
            screenRows: b.rows,
            cursorRow: row,
            cursorCol: col,
            cursorVisible: cursorVisible,
            activeBuffer: active,
            scrollback: sb,
            screen: screen,
            generation: generation
        )
    }
}
