// SPDX-License-Identifier: GPL-3.0-or-later
//
// High-level Swift API over the iSH embed C ABI.
//
// One IshInstance per process. Spawned commands are returned as
// IshSession; many can be in flight concurrently. A hung command does
// not block other sessions because all I/O is multiplexed through a
// single supervisor pipe and demultiplexed per session_id by the
// reader thread.
//
// Implementation note: opaque C structs (`ish_embed_instance` /
// `ish_embed_session`) come through to Swift as OpaquePointer. We hand
// those pointers back to the C ABI verbatim.

import Foundation
@_implementationOnly import CIshEmbed

public enum IshError: Error, CustomStringConvertible {
    case raw(Int32, String)

    public var description: String {
        switch self {
        case .raw(let c, let m): return "IshError(\(c)): \(m)"
        }
    }

    static func from(_ rc: Int32) -> IshError {
        let s = String(cString: ish_embed_strerror(rc))
        return .raw(rc, s)
    }
}

public struct IshSpawnOptions {
    public var argv: [String]
    public var cwd: String?
    public var env: [String: String]?
    public var allocateTTY: Bool
    public var mergeStderrIntoStdout: Bool
    public var timeout: TimeInterval?
    /// If non-nil, the child chroots to this guest path before exec.
    /// Used for VM-style isolation; pass `/srv/vms/<name>` to confine
    /// the new process to that VM's filesystem.
    public var chrootPath: String?
    /// Initial pty window size. Only honored when `allocateTTY` is
    /// true; pipe spawns ignore. nil = use the supervisor's default
    /// (24×80). Pixel dimensions are informational and may be left at
    /// 0 if the host doesn't know them.
    public var initialWindowSize: (rows: UInt16, cols: UInt16,
                                   xpixel: UInt16, ypixel: UInt16)?

    public init(argv: [String],
                cwd: String? = nil,
                env: [String: String]? = nil,
                allocateTTY: Bool = false,
                mergeStderrIntoStdout: Bool = false,
                timeout: TimeInterval? = nil,
                chrootPath: String? = nil,
                initialWindowSize: (rows: UInt16, cols: UInt16,
                                    xpixel: UInt16, ypixel: UInt16)? = nil) {
        self.argv = argv
        self.cwd  = cwd
        self.env  = env
        self.allocateTTY = allocateTTY
        self.mergeStderrIntoStdout = mergeStderrIntoStdout
        self.timeout = timeout
        self.chrootPath = chrootPath
        self.initialWindowSize = initialWindowSize
    }
}

public struct IshOneshotResult {
    public var exitCode: Int32
    public var signal: Int32
    public var stdoutData: Data
    public var stderrData: Data
    public var timedOut: Bool
}

public enum IshStreamKind: Sendable {
    case stdout
    case stderr
}

public enum IshSessionEvent {
    case data(Data, kind: IshStreamKind, seq: UInt64)
    case exited(exitCode: Int32, signal: Int32)
}

// MARK: - Constants from C ABI

// The C header exposes ISH_OK / ISH_ERR_* as an unnamed enum and
// ISH_STREAM_* as #define ints; both arrive as Int32 in Swift.
private let ishOK: Int32                = ISH_OK.rawValue
private let ishErrNotRunning: Int32     = ISH_ERR_NOT_RUNNING.rawValue
private let ishErrAlreadyBooted: Int32  = ISH_ERR_ALREADY_BOOTED.rawValue
private let ishErrNoSession: Int32      = ISH_ERR_NO_SESSION.rawValue
private let ishErrInvalidArg: Int32     = ISH_ERR_INVALID_ARG.rawValue
private let ishErrInternal: Int32       = ISH_ERR_INTERNAL.rawValue
private let ishErrProtocol: Int32       = ISH_ERR_PROTOCOL.rawValue
private let ishErrTimeout: Int32        = ISH_ERR_TIMEOUT.rawValue
private let ishErrBoot: Int32           = ISH_ERR_BOOT.rawValue

// Mirrors `#define ISH_STREAM_*` in ishembed.h. Plain `#define` macros
// don't survive @_implementationOnly imports, so we re-declare them.
private let streamStdout: Int32 = 1
private let streamStderr: Int32 = 2
private let streamExited: Int32 = 3

/// One iSH instance per host process.
public final class IshInstance: @unchecked Sendable {

    public struct BootOptions {
        public var rootfsPath: String
        public var workdir: String
        public var supervisorGuestPath: String?
        public var kernelLogFD: Int32  // -1 -> stderr

        public init(rootfsPath: String,
                    workdir: String = "/",
                    supervisorGuestPath: String? = nil,
                    kernelLogFD: Int32 = -1) {
            self.rootfsPath = rootfsPath
            self.workdir = workdir
            self.supervisorGuestPath = supervisorGuestPath
            self.kernelLogFD = kernelLogFD
        }
    }

    private var raw: OpaquePointer?
    private let lock = NSLock()

    public static let shared = IshInstance()

    private init() {}

    public var isRunning: Bool {
        lock.lock(); defer { lock.unlock() }
        return raw != nil
    }

    public func boot(_ opts: BootOptions) throws {
        lock.lock()
        if raw != nil {
            lock.unlock()
            throw IshError.from(ishErrAlreadyBooted)
        }
        lock.unlock()

        let rootfsCStr  = strdup(opts.rootfsPath)!
        let workdirCStr = strdup(opts.workdir)!
        let supCStr: UnsafeMutablePointer<CChar>? = opts.supervisorGuestPath.flatMap { strdup($0) }
        defer {
            free(rootfsCStr); free(workdirCStr)
            if let s = supCStr { free(s) }
        }

        var optsC = ish_embed_boot_opts_t()
        optsC.rootfs_path = UnsafePointer(rootfsCStr)
        optsC.workdir     = UnsafePointer(workdirCStr)
        optsC.supervisor_guest_path = supCStr.map { UnsafePointer($0) }
        optsC.kernel_log_fd = opts.kernelLogFD
        optsC.reserved_flags = 0

        var inst: OpaquePointer? = nil
        let rc = ish_embed_boot(&optsC, &inst)
        if rc != ishOK { throw IshError.from(rc) }
        guard let inst = inst else { throw IshError.from(ishErrBoot) }

        lock.lock(); self.raw = inst; lock.unlock()
    }

    public func shutdown(graceMs: UInt32 = 5_000) throws {
        lock.lock()
        let r = self.raw
        self.raw = nil
        lock.unlock()
        guard let r = r else { return }
        let rc = ish_embed_shutdown(r, graceMs)
        if rc != ishOK { throw IshError.from(rc) }
    }

    /// Run a single command and collect output.
    public func runOneshot(_ opts: IshSpawnOptions) throws -> IshOneshotResult {
        let r = try currentRaw()
        return try withCSpawnOpts(opts) { cOpts in
            var res = ish_embed_oneshot_result_t()
            let rc = ish_embed_run_oneshot(r, cOpts, &res)
            if rc != ishOK { throw IshError.from(rc) }
            let out = bytesToData(res.stdout_buf, length: res.stdout_len)
            let err = bytesToData(res.stderr_buf, length: res.stderr_len)
            return IshOneshotResult(
                exitCode: res.exit_code,
                signal:   res.signal,
                stdoutData: out,
                stderrData: err,
                timedOut: res.timed_out != 0
            )
        }
    }

    /// Spawn a streaming session.
    public func spawn(_ opts: IshSpawnOptions) throws -> IshSession {
        let r = try currentRaw()
        let tty = opts.allocateTTY
        return try withCSpawnOpts(opts) { cOpts in
            var sess: OpaquePointer? = nil
            let rc = ish_embed_spawn(r, cOpts, &sess)
            if rc != ishOK { throw IshError.from(rc) }
            guard let sess = sess else { throw IshError.from(ishErrInternal) }
            return IshSession(raw: sess, isTTY: tty)
        }
    }

    // MARK: - private helpers

    private func currentRaw() throws -> OpaquePointer {
        lock.lock(); defer { lock.unlock() }
        guard let r = raw else { throw IshError.from(ishErrNotRunning) }
        return r
    }

    private func withCSpawnOpts<R>(_ opts: IshSpawnOptions,
                                   _ body: (UnsafePointer<ish_embed_spawn_opts_t>) throws -> R) throws -> R {
        // argv: [String] -> NULL-terminated [const char *]
        var argvStorage: [UnsafeMutablePointer<CChar>?] = opts.argv.map { strdup($0) }
        argvStorage.append(nil)
        defer { for p in argvStorage { free(p) } }

        var envStorage: [UnsafeMutablePointer<CChar>?] = []
        if let env = opts.env {
            envStorage = env.map { strdup("\($0.key)=\($0.value)") }
        } else {
            let defaultEnv = ["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                              "HOME=/root", "TERM=xterm-256color"]
            envStorage = defaultEnv.map { strdup($0) }
        }
        envStorage.append(nil)
        defer { for p in envStorage { free(p) } }

        let cwdC: UnsafeMutablePointer<CChar>? = opts.cwd.flatMap { strdup($0) }
        defer { if let p = cwdC { free(p) } }

        let chrootC: UnsafeMutablePointer<CChar>? = opts.chrootPath.flatMap { strdup($0) }
        defer { if let p = chrootC { free(p) } }

        var cOpts = ish_embed_spawn_opts_t()
        return try argvStorage.withUnsafeBufferPointer { argvBuf in
            try envStorage.withUnsafeBufferPointer { envBuf in
                cOpts.argv = UnsafeRawPointer(argvBuf.baseAddress!).assumingMemoryBound(to: Optional<UnsafePointer<CChar>>.self)
                cOpts.envp = UnsafeRawPointer(envBuf.baseAddress!).assumingMemoryBound(to: Optional<UnsafePointer<CChar>>.self)
                cOpts.cwd  = cwdC.map { UnsafePointer($0) }
                cOpts.allocate_tty = opts.allocateTTY ? 1 : 0
                cOpts.merge_stderr_into_stdout = opts.mergeStderrIntoStdout ? 1 : 0
                cOpts.timeout_ms = opts.timeout.map { UInt32(min(max(0, $0 * 1000), Double(UInt32.max - 1))) } ?? 0
                cOpts.chroot_path = chrootC.map { UnsafePointer($0) }
                cOpts.reserved_flags = 0
                if let ws = opts.initialWindowSize {
                    cOpts.init_rows   = ws.rows
                    cOpts.init_cols   = ws.cols
                    cOpts.init_xpixel = ws.xpixel
                    cOpts.init_ypixel = ws.ypixel
                } else {
                    cOpts.init_rows = 0; cOpts.init_cols = 0
                    cOpts.init_xpixel = 0; cOpts.init_ypixel = 0
                }
                return try body(&cOpts)
            }
        }
    }
}

/// A streaming session — a single Linux process inside iSH.
public final class IshSession: @unchecked Sendable {
    private var raw: OpaquePointer?
    private let lock = NSLock()
    /// True if the session was spawned with allocateTTY = true. In that
    /// case the supervisor wired the child to a pty, so Ctrl+C and other
    /// terminal-style signals must be delivered as control bytes on the
    /// stdin pipe (the kernel tty layer turns them into pgrp signals),
    /// not as guest-side `kill -SIGINT` which would hit the shell itself.
    private let isTTY: Bool

    init(raw: OpaquePointer, isTTY: Bool = false) {
        self.raw = raw
        self.isTTY = isTTY
    }

    deinit {
        if let r = raw { ish_embed_session_close(r) }
    }

    /// Wait up to `timeout` for the next event. timeout == nil means forever.
    public func read(timeout: TimeInterval?) throws -> IshSessionEvent {
        let r = try currentRaw()
        let waitMs: UInt32
        switch timeout {
        case nil: waitMs = UInt32.max
        case .some(let t) where t <= 0: waitMs = 0
        case .some(let t): waitMs = UInt32(min(max(0, t * 1000), Double(UInt32.max - 1)))
        }
        var buf: UnsafeMutablePointer<UInt8>? = nil
        var len: Int = 0
        var kind: Int32 = 0
        var seq: UInt64 = 0
        var exitCode: Int32 = 0
        var signal: Int32 = 0
        let rc = ish_embed_session_read(r, waitMs,
                                         &buf, &len, &kind, &seq, &exitCode, &signal)
        if rc != ishOK { throw IshError.from(rc) }
        switch kind {
        case streamStdout:
            return .data(bytesToData(buf, length: len), kind: .stdout, seq: seq)
        case streamStderr:
            return .data(bytesToData(buf, length: len), kind: .stderr, seq: seq)
        case streamExited:
            return .exited(exitCode: exitCode, signal: signal)
        default:
            throw IshError.from(ishErrProtocol)
        }
    }

    public func write(_ data: Data) throws {
        let r = try currentRaw()
        try data.withUnsafeBytes { rawBuf in
            let rc = ish_embed_session_write(r,
                                             rawBuf.baseAddress?.assumingMemoryBound(to: UInt8.self),
                                             rawBuf.count)
            if rc != ishOK { throw IshError.from(rc) }
        }
    }

    public func closeStdin() throws {
        let r = try currentRaw()
        let rc = ish_embed_session_close_stdin(r)
        if rc != ishOK { throw IshError.from(rc) }
    }

    /// Send a signal (standard Linux signum). Use 2 for SIGINT (Ctrl+C).
    ///
    /// In TTY mode, common terminal-control signals are translated into
    /// the matching control byte written to stdin (e.g. SIGINT → ^C =
    /// 0x03). The kernel tty layer turns them into a signal sent to the
    /// foreground process group, which is what an interactive shell
    /// expects. Sending SIGINT directly to the session pgid would kill
    /// the shell itself.
    public func signal(_ signum: Int32) throws {
        if isTTY {
            switch signum {
            case 2:  // SIGINT  → ^C
                try write(Data([0x03])); return
            case 3:  // SIGQUIT → ^\
                try write(Data([0x1c])); return
            case 20: // SIGTSTP → ^Z
                try write(Data([0x1a])); return
            default: break // fall through to direct signal for everything else
            }
        }
        let r = try currentRaw()
        let rc = ish_embed_session_signal(r, signum)
        if rc != ishOK { throw IshError.from(rc) }
    }

    /// Send a real signal to the session process group even for TTY sessions.
    public func signalDirect(_ signum: Int32) throws {
        let r = try currentRaw()
        let rc = ish_embed_session_signal(r, signum)
        if rc != ishOK { throw IshError.from(rc) }
    }

    /// Resize the session's pty and deliver SIGWINCH to the foreground
    /// process group. Silently no-op for non-TTY sessions. `xpixel` /
    /// `ypixel` are informational; pass 0 if unknown.
    public func resize(rows: UInt16, cols: UInt16,
                       xpixel: UInt16 = 0, ypixel: UInt16 = 0) throws {
        let r = try currentRaw()
        let rc = ish_embed_session_resize(r, rows, cols, xpixel, ypixel)
        if rc != ishOK { throw IshError.from(rc) }
    }

    /// Convenience: SIGINT (Ctrl+C). In TTY mode this is a control byte
    /// to the pty master, otherwise a direct kill(-pgid, SIGINT).
    public func interrupt() throws { try signal(2) }

    /// SIGTERM, then SIGKILL after grace.
    public func terminate(graceMs: UInt32 = 1_500) throws {
        let r = try currentRaw()
        let rc = ish_embed_session_terminate(r, graceMs)
        if rc != ishOK { throw IshError.from(rc) }
    }

    public func close() {
        lock.lock()
        let r = self.raw
        self.raw = nil
        lock.unlock()
        if let r = r { ish_embed_session_close(r) }
    }

    private func currentRaw() throws -> OpaquePointer {
        lock.lock(); defer { lock.unlock() }
        guard let r = raw else { throw IshError.from(ishErrNoSession) }
        return r
    }
}

// MARK: - helpers

private func bytesToData(_ buf: UnsafeMutablePointer<UInt8>?, length: Int) -> Data {
    guard let buf = buf, length > 0 else {
        if let buf = buf { ish_embed_free(buf) }
        return Data()
    }
    return Data(bytesNoCopy: buf, count: length,
                deallocator: .custom { p, _ in ish_embed_free(p) })
}
