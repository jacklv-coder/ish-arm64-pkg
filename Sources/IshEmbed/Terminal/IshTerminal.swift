// SPDX-License-Identifier: GPL-3.0-or-later
//
// IshTerminal — the top-level type a host app interacts with to run a
// full-screen TUI inside an iSH VM. Combines:
//
//   • IshSession  — the pty stream (stdin/stdout/signals/exit) to the
//                    Linux process inside iSH.
//   • VTEmulator  — parses the byte stream and maintains the screen.
//   • IshKeyEncoder — turns named keys into the right pty bytes.
//
// Lifecycle:
//   let term = try IshTerminal.start(in: vm,
//       command: ["/bin/sh", "-i"],
//       size: .init(rows: 30, cols: 100))
//   // ... receive snapshots via term.onChange = { snapshot in ... }
//   try term.send(.key(.up))
//   try term.send(.text("ls\n"))
//   term.close()
//
// Threading:
//   - All IshTerminal methods are safe from any host thread.
//   - The pty read pump runs on its own internal thread; user-visible
//     callbacks are dispatched onto the queue passed to
//     `setSnapshotHandler(queue:_:)`. Default is .main.
//   - The emulator itself is serialised via an internal lock.
//

import Foundation

public final class IshTerminal: @unchecked Sendable {

    /// The public callback predates Swift's Sendable annotations and is
    /// documented to cross onto `handlerQueue`. Keep that source-compatible
    /// signature while containing the unchecked queue crossing in one place.
    private final class EventHandlerBox: @unchecked Sendable {
        let handler: (Event) -> Void

        init(_ handler: @escaping (Event) -> Void) {
            self.handler = handler
        }

        func callAsFunction(_ event: Event) {
            handler(event)
        }
    }

    // MARK: public surface

    public enum Event: Sendable {
        /// The screen changed. Generation is monotonic.
        case screenUpdate(generation: UInt64)
        /// stdout/stderr byte chunk (rare: most consumers don't care
        /// because the screen is the source of truth).
        case streamData(Data, IshStreamKind)
        /// The underlying process exited.
        case exited(exitCode: Int32, signal: Int32)
        /// A read error or other unrecoverable transport failure.
        case error(Error)
    }

    public struct Size: Sendable, Equatable {
        public var rows: Int
        public var cols: Int
        public init(rows: Int = 24, cols: Int = 80) {
            self.rows = max(1, rows)
            self.cols = max(1, cols)
        }
    }

    public struct Options: Sendable {
        public var size: Size
        public var keyEncoderOptions: IshKeyEncoderOptions
        /// Cap on scrollback rows for the main buffer.
        public var scrollbackLimit: Int
        /// Environment passed to the spawned process.
        public var env: [String: String]
        /// Working directory.
        public var cwd: String?
        public init(size: Size = Size(),
                    keyEncoderOptions: IshKeyEncoderOptions = .init(),
                    scrollbackLimit: Int = 5000,
                    env: [String: String] = [:],
                    cwd: String? = nil) {
            self.size = size
            self.keyEncoderOptions = keyEncoderOptions
            self.scrollbackLimit = scrollbackLimit
            self.env = env
            self.cwd = cwd
        }
    }

    /// The underlying session (so callers that need direct write/signal
    /// access — e.g. graceful kill on shutdown — can reach it).
    public let session: IshSession

    /// The emulator. Exposed read-only to consumers that want full
    /// control over rendering (run their own snapshot loop).
    public let emulator: VTEmulator

    /// Encoder used by `send(_:)`. Mutable so callers can tweak (e.g.
    /// flip `backspaceIsBS`).
    public var keyEncoder: IshKeyEncoder

    public private(set) var size: Size

    private let emuLock = NSRecursiveLock()
    private var handlerQueue: DispatchQueue = .main
    private var eventHandler: EventHandlerBox?
    private let pumpQueue: DispatchQueue
    private var stopped: Bool = false

    // MARK: construction

    /// Spawn a process inside the given VM and start the read pump.
    /// Convenience over `IshInstance.spawn(in:)`.
    public static func start(in vm: IshVM,
                              instance: IshInstance = .shared,
                              command: [String] = ["/bin/sh", "-i"],
                              options: Options = .init()) throws -> IshTerminal {
        var spawn = IshSpawnOptions(argv: command)
        spawn.cwd = options.cwd ?? "/root"
        spawn.allocateTTY = true
        spawn.env = options.env.isEmpty ? nil : options.env
        // Hand the supervisor the initial winsize so the guest's pty
        // starts at the right dimensions — TUIs that read the size
        // synchronously at boot (vim, htop, ranger) will lay
        // themselves out correctly from the first frame.
        spawn.initialWindowSize = (
            rows: UInt16(clamping: options.size.rows),
            cols: UInt16(clamping: options.size.cols),
            xpixel: 0, ypixel: 0)
        let session = try instance.spawn(in: vm, spawn)
        return IshTerminal(session: session, options: options)
    }

    /// Wrap an already-spawned session. Useful when the caller has its
    /// own spawn pipeline; the session must have been created with
    /// `allocateTTY = true` for the emulator's output to make sense.
    public init(session: IshSession, options: Options = .init()) {
        self.session = session
        self.keyEncoder = IshKeyEncoder(options: options.keyEncoderOptions)
        self.size = options.size
        let emu = VTEmulator(rows: options.size.rows, cols: options.size.cols)
        emu.main.scrollbackLimit = options.scrollbackLimit
        self.emulator = emu
        self.pumpQueue = DispatchQueue(label: "ish.terminal.pump",
                                       qos: .userInitiated)
        startPump()
    }

    deinit { close() }

    // MARK: events

    /// Set the callback invoked on every event (screen update,
    /// exited, error). Called on `queue` (default .main).
    public func setEventHandler(queue: DispatchQueue = .main,
                                 _ handler: @escaping (Event) -> Void) {
        emuLock.lock()
        self.handlerQueue = queue
        self.eventHandler = EventHandlerBox(handler)
        emuLock.unlock()
    }

    // MARK: I/O

    /// Send one key. Encoded according to current key encoder options
    /// and emulator mode (DECCKM etc.).
    public func send(_ key: IshKey) throws {
        let bytes = encode(key)
        try send(bytes: bytes)
    }

    /// Send a sequence of keys (no inter-key delay).
    public func send(_ keys: [IshKey]) throws {
        let bytes = encode(keys)
        try send(bytes: bytes)
    }

    /// Send raw bytes verbatim (e.g. paste of already-encoded data).
    public func send(bytes: Data) throws {
        guard !bytes.isEmpty else { return }
        try session.write(bytes)
    }

    /// Encode without sending (e.g. to batch multiple keys with extra
    /// raw bytes between them).
    public func encode(_ key: IshKey) -> Data {
        emuLock.lock(); defer { emuLock.unlock() }
        return keyEncoder.encode(key, emulator: emulator)
    }
    public func encode(_ keys: [IshKey]) -> Data {
        emuLock.lock(); defer { emuLock.unlock() }
        return keyEncoder.encode(keys, emulator: emulator)
    }

    /// Send a Linux signal to the process (pty-translated in TTY mode).
    public func signal(_ signum: Int32) throws { try session.signal(signum) }
    /// Skip pty-byte translation and send the signal directly.
    public func signalDirect(_ signum: Int32) throws { try session.signalDirect(signum) }
    public func interrupt() throws { try session.interrupt() }
    public func terminate(graceMs: UInt32 = 1_500) throws { try session.terminate(graceMs: graceMs) }
    public func closeStdin() throws { try session.closeStdin() }

    // MARK: resize

    /// Resize the local emulator grid AND the guest pty. The guest
    /// receives SIGWINCH; ncurses/ratatui/Ink-based TUIs (vim,
    /// htop) will re-layout on their next paint. For pipe-mode
    /// sessions the pty resize is a silent no-op.
    public func resize(_ size: Size) {
        emuLock.lock()
        self.size = size
        emulator.resize(rows: size.rows, cols: size.cols)
        emuLock.unlock()
        try? session.resize(rows: UInt16(clamping: size.rows),
                            cols: UInt16(clamping: size.cols))
        deliver(.screenUpdate(generation: emulator.generation))
    }

    // MARK: snapshots

    /// Capture the current screen + scrollback. Cheap.
    public func snapshot() -> IshTerminalSnapshot {
        emuLock.lock(); defer { emuLock.unlock() }
        return emulator.snapshot()
    }

    // MARK: shutdown

    /// Stop the read pump and close the session. Idempotent.
    public func close() {
        emuLock.lock()
        guard !stopped else { emuLock.unlock(); return }
        stopped = true
        emuLock.unlock()
        session.close()
    }

    // MARK: read pump

    private func startPump() {
        pumpQueue.async { [weak self] in
            guard let self else { return }
            while true {
                if self.stoppedSafe() { return }
                do {
                    let ev = try self.session.read(timeout: nil)
                    switch ev {
                    case .data(let data, let kind, _):
                        self.handleStreamData(data, kind: kind)
                    case .exited(let code, let sig):
                        self.deliver(.exited(exitCode: code, signal: sig))
                        return
                    }
                } catch {
                    if self.stoppedSafe() { return }
                    self.deliver(.error(error))
                    return
                }
            }
        }
    }

    private func handleStreamData(_ data: Data, kind: IshStreamKind) {
        // Feed everything (stdout + stderr) into the emulator. The
        // pty already merged them in TTY mode; even when split, the
        // emulator's state is the visual truth.
        emuLock.lock()
        emulator.feed(data)
        let reply = emulator.drainReply()
        let gen = emulator.generation
        emuLock.unlock()
        if !reply.isEmpty { try? session.write(reply) }
        deliver(.streamData(data, kind))
        deliver(.screenUpdate(generation: gen))
    }

    private func deliver(_ ev: Event) {
        emuLock.lock()
        let q = handlerQueue
        let h = eventHandler
        emuLock.unlock()
        guard let h else { return }
        q.async { h(ev) }
    }

    private func stoppedSafe() -> Bool {
        emuLock.lock(); defer { emuLock.unlock() }
        return stopped
    }
}

// MARK: - VM convenience

public extension IshInstance {
    /// Spawn a TTY-attached process in `vm` and wrap it in an IshTerminal.
    /// Convenience for the most common case.
    func terminal(in vm: IshVM,
                  command: [String] = ["/bin/sh", "-i"],
                  options: IshTerminal.Options = .init()) throws -> IshTerminal {
        try IshTerminal.start(in: vm, instance: self,
                               command: command, options: options)
    }
}
