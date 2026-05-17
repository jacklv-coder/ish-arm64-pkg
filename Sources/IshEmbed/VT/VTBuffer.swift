// SPDX-License-Identifier: GPL-3.0-or-later
//
// VTBuffer — one independent screen grid (main or alt-screen) plus
// optional scrollback. Pure logic, no platform deps.
//

import Foundation

/// One screen grid (main or alt-screen). Cells are stored row-major.
public final class VTBuffer {
    public private(set) var rows: Int
    public private(set) var cols: Int

    /// Row-major: cells[r * cols + c].
    public internal(set) var cells: [VTCell]

    /// ESC 7 / CSI s saved cursor for this buffer.
    public internal(set) var savedCursor: (row: Int, col: Int)? = nil

    /// Lines that have scrolled off the top. Populated only when
    /// `keepScrollback` is true. Oldest entries first.
    public internal(set) var scrollback: [[VTCell]] = []

    /// Maximum scrollback retention. Default 5000 ≈ a long apk/npm log.
    public var scrollbackLimit: Int = 5000

    /// Whether `scrollUp` archives lifted rows into `scrollback`.
    public var keepScrollback: Bool = false

    public init(rows: Int, cols: Int) {
        self.rows = rows
        self.cols = cols
        self.cells = Array(repeating: .empty, count: rows * cols)
    }

    @inline(__always)
    public func cell(_ r: Int, _ c: Int) -> VTCell {
        guard r >= 0, r < rows, c >= 0, c < cols else { return .empty }
        return cells[r * cols + c]
    }

    @inline(__always)
    public func setCell(_ r: Int, _ c: Int, _ v: VTCell) {
        guard r >= 0, r < rows, c >= 0, c < cols else { return }
        cells[r * cols + c] = v
    }

    public func clear() {
        cells = Array(repeating: .empty, count: rows * cols)
    }

    /// Resize. Preserves the top-left rectangle of the old contents.
    public func resize(rows newRows: Int, cols newCols: Int) {
        guard newRows != rows || newCols != cols else { return }
        var fresh = Array<VTCell>(repeating: .empty, count: newRows * newCols)
        let copyRows = min(rows, newRows)
        let copyCols = min(cols, newCols)
        for r in 0..<copyRows {
            for c in 0..<copyCols {
                fresh[r * newCols + c] = cells[r * cols + c]
            }
        }
        cells = fresh
        rows = newRows
        cols = newCols
    }

    /// Scroll region `top...bottom` up by `n` lines; new rows are blanked.
    /// If `keepScrollback` is set AND `top == 0`, the lifted rows are
    /// archived into `scrollback`.
    public func scrollUp(top: Int, bottom: Int, n: Int = 1, blank: VTCell = .empty) {
        guard n > 0, top >= 0, bottom < rows, top <= bottom else { return }
        let region = bottom - top + 1
        let lift = min(n, region)
        if keepScrollback && top == 0 {
            for r in 0..<lift {
                var row = [VTCell](repeating: .empty, count: cols)
                for c in 0..<cols { row[c] = cells[r * cols + c] }
                scrollback.append(row)
            }
            if scrollback.count > scrollbackLimit {
                scrollback.removeFirst(scrollback.count - scrollbackLimit)
            }
        }
        for r in top..<(bottom - lift + 1) {
            let srcStart = (r + lift) * cols
            let dstStart = r * cols
            for c in 0..<cols {
                cells[dstStart + c] = cells[srcStart + c]
            }
        }
        for r in (bottom - lift + 1)...bottom {
            for c in 0..<cols { cells[r * cols + c] = blank }
        }
    }

    /// Scroll region `top...bottom` down by `n` lines; new top rows blanked.
    public func scrollDown(top: Int, bottom: Int, n: Int = 1, blank: VTCell = .empty) {
        guard n > 0, top >= 0, bottom < rows, top <= bottom else { return }
        let region = bottom - top + 1
        let push = min(n, region)
        var r = bottom
        while r >= top + push {
            let srcStart = (r - push) * cols
            let dstStart = r * cols
            for c in 0..<cols {
                cells[dstStart + c] = cells[srcStart + c]
            }
            r -= 1
        }
        for r2 in top..<(top + push) {
            for c in 0..<cols { cells[r2 * cols + c] = blank }
        }
    }
}
