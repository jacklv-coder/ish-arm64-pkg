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
import CIshEmbed

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

    /// Swift's v0.3.3 compatibility gate can produce BUSY without entering
    /// the native library. Keep that locally-generated error intelligible
    /// even when an older binary has no matching strerror entry.
    static func busy() -> IshError {
        .raw(ISH_ERR_BUSY.rawValue,
             "IshInstance is busy with an active call, session, or lifecycle transition")
    }
}

public enum IshFilesystemError: Error, Equatable, CustomStringConvertible {
    /// The destination existed at the atomic rename point. Neither path was
    /// replaced, and the source remains unchanged.
    case destinationExists
    /// A positive Linux guest errno returned by the filesystem operation.
    case guestErrno(Int32)

    public var description: String {
        switch self {
        case .destinationExists:
            return "The destination already exists."
        case .guestErrno(let value):
            return "Guest filesystem operation failed with Linux errno \(value)."
        }
    }
}

public struct IshSpawnOptions {
    public var argv: [String]
    public var cwd: String?
    public var env: [String: String]?
    public var allocateTTY: Bool
    public var mergeStderrIntoStdout: Bool
    /// Starts at the Swift API entry. For oneshot commands, bounds Swift option
    /// staging plus the complete native execution and cleanup path. For
    /// streaming spawn, bounds Swift/native SPAWN staging and admission, and
    /// selects ordered bounded admission for stdin writes/close and terminate.
    /// Callers still confirm streaming termination by reading the authoritative
    /// exit event; stdin close may report busy while an active write owns its
    /// order gate.
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
private let ishUnsupported: Int32       = -22
private let ishErrNotRunning: Int32     = ISH_ERR_NOT_RUNNING.rawValue
private let ishErrAlreadyBooted: Int32  = ISH_ERR_ALREADY_BOOTED.rawValue
private let ishErrNoSession: Int32      = ISH_ERR_NO_SESSION.rawValue
private let ishErrInvalidArg: Int32     = ISH_ERR_INVALID_ARG.rawValue
private let ishErrInternal: Int32       = ISH_ERR_INTERNAL.rawValue
private let ishErrProtocol: Int32       = ISH_ERR_PROTOCOL.rawValue
private let ishErrTimeout: Int32        = ISH_ERR_TIMEOUT.rawValue
private let ishErrBusy: Int32           = ISH_ERR_BUSY.rawValue
private let ishErrBoot: Int32           = ISH_ERR_BOOT.rawValue

// Mirrors `#define ISH_STREAM_*` in ishembed.h. Plain `#define` macros
// don't survive @_implementationOnly imports, so we re-declare them.
private let streamStdout: Int32 = 1
private let streamStderr: Int32 = 2
private let streamExited: Int32 = 3

/// Converts the public relative timeout into one absolute Swift-side deadline.
/// The remaining native budget is resolved only after argv/env/cwd/chroot
/// staging, immediately before the C entry. A positive remainder below one
/// millisecond expires in Swift because `timeout_ms == 0` means unbounded.
struct IshSpawnTimeoutBudget {
    private static let maximumMilliseconds = Double(UInt32.max - 1)
    private let deadline: TimeInterval?

    init(timeout: TimeInterval?, startedAt: TimeInterval) throws {
        guard let timeout else {
            deadline = nil
            return
        }
        guard timeout.isFinite else {
            throw IshError.from(ishErrInvalidArg)
        }
        guard timeout > 0 else {
            deadline = nil
            return
        }

        let boundedSeconds = min(
            timeout,
            Self.maximumMilliseconds / 1_000
        )
        deadline = startedAt + boundedSeconds
    }

    func remainingMilliseconds(at now: TimeInterval) throws -> UInt32 {
        guard let deadline else {
            return 0
        }

        let milliseconds = min(
            Self.maximumMilliseconds,
            floor((deadline - now) * 1_000)
        )
        guard milliseconds >= 1 else {
            throw IshError.from(ishErrTimeout)
        }
        return UInt32(milliseconds)
    }

    func apply(
        to options: UnsafeMutablePointer<ish_embed_spawn_opts_t>,
        at now: TimeInterval
    ) throws {
        options.pointee.timeout_ms = try remainingMilliseconds(at: now)
    }
}

/// Internal lifecycle seam used by deterministic RootFS-free tests. The live
/// value remains a direct adapter over the same v0.3.3 C ABI; this does not add
/// or dynamically probe any newer retain/release symbols.
struct IshLifecycleNativeCalls: @unchecked Sendable {
    let boot: (UnsafePointer<ish_embed_boot_opts_t>) ->
        (result: Int32, instance: OpaquePointer?)
    let shutdown: (OpaquePointer, UInt32) -> Int32
    let runOneshot: (OpaquePointer,
                     UnsafeMutablePointer<ish_embed_spawn_opts_t>,
                     IshSpawnTimeoutBudget,
                     UnsafeMutablePointer<ish_embed_oneshot_result_t>) throws -> Int32
    let spawn: (OpaquePointer,
                UnsafeMutablePointer<ish_embed_spawn_opts_t>,
                IshSpawnTimeoutBudget) throws ->
        (result: Int32, session: OpaquePointer?)
    let renameNoReplace: (OpaquePointer, UnsafePointer<CChar>,
                          UnsafePointer<CChar>, UInt32,
                          UnsafeMutablePointer<Int32>) -> Int32
    let sessionClose: (OpaquePointer) -> Void
    let freeBuffer: (UnsafeMutablePointer<UInt8>) -> Void

    static let live = IshLifecycleNativeCalls(
        boot: { opts in
            var instance: OpaquePointer? = nil
            let result = ish_embed_boot(opts, &instance)
            return (result, instance)
        },
        shutdown: { instance, graceMs in
            ish_embed_shutdown(instance, graceMs)
        },
        runOneshot: { instance, opts, timeoutBudget, result in
            try timeoutBudget.apply(
                to: opts,
                at: ProcessInfo.processInfo.systemUptime
            )
            return ish_embed_run_oneshot(instance, opts, result)
        },
        spawn: { instance, opts, timeoutBudget in
            try timeoutBudget.apply(
                to: opts,
                at: ProcessInfo.processInfo.systemUptime
            )
            var session: OpaquePointer? = nil
            let result = ish_embed_spawn(instance, opts, &session)
            return (result, session)
        },
        renameNoReplace: { instance, source, destination, timeoutMs,
                           guestErrno in
            ish_embed_swift_rename_noreplace(
                instance, source, destination, timeoutMs, guestErrno
            )
        },
        sessionClose: { session in
            ish_embed_session_close(session)
        },
        freeBuffer: { buffer in
            ish_embed_free(buffer)
        })
}

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

    private let callGate = IshInstanceCallGate()
    private let nativeCalls: IshLifecycleNativeCalls

    public static let shared = IshInstance(nativeCalls: .live)

    init(nativeCalls: IshLifecycleNativeCalls) {
        self.nativeCalls = nativeCalls
    }

    public var isRunning: Bool {
        callGate.isRunning
    }

    public func boot(_ opts: BootOptions) throws {
        try callGate.beginBoot()
        var finishedBoot = false
        defer {
            if !finishedBoot { callGate.finishBoot(nil) }
        }

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

        let (rc, inst) = nativeCalls.boot(&optsC)
        if rc != ishOK { throw IshError.from(rc) }
        guard let inst = inst else { throw IshError.from(ishErrBoot) }

        callGate.finishBoot(inst)
        finishedBoot = true
    }

    public func shutdown(graceMs: UInt32 = 5_000) throws {
        let attempt = try callGate.beginShutdown()
        guard case .ready(let r) = attempt else {
            return
        }

        // beginShutdown() has atomically blocked new calls and established
        // that no call or session lifetime lease can still reference `r`.
        // This preserves compatibility with v0.3.3, which has no native
        // instance retain/release and cannot safely free an in-use handle.
        let rc = nativeCalls.shutdown(r, graceMs)
        callGate.finishShutdown(attempted: r, result: rc)

        if rc != ishOK { throw IshError.from(rc) }
    }

    /// Run a single command and collect output.
    public func runOneshot(_ opts: IshSpawnOptions) throws -> IshOneshotResult {
        let timeoutBudget = try IshSpawnTimeoutBudget(
            timeout: opts.timeout,
            startedAt: ProcessInfo.processInfo.systemUptime
        )
        let lease = try callGate.acquireCall()
        defer { lease.release() }
        return try withCSpawnOpts(opts) { cOpts in
            var res = ish_embed_oneshot_result_t()
            let rc = try nativeCalls.runOneshot(
                lease.raw,
                cOpts,
                timeoutBudget,
                &res
            )
            if rc != ishOK { throw IshError.from(rc) }
            let out = bytesToData(res.stdout_buf,
                                  length: res.stdout_len,
                                  freeBuffer: nativeCalls.freeBuffer)
            let err = bytesToData(res.stderr_buf,
                                  length: res.stderr_len,
                                  freeBuffer: nativeCalls.freeBuffer)
            return IshOneshotResult(
                exitCode: res.exit_code,
                signal:   res.signal,
                stdoutData: out,
                stderrData: err,
                timedOut: res.timed_out != 0
            )
        }
    }

    /// Atomically renames a guest file or directory without replacing an
    /// existing destination. Both paths are absolute inside the Linux guest.
    /// This operation does not invoke a shell and does not perform a racy
    /// existence check before rename.
    public func renameNoReplace(
        from source: String,
        to destination: String,
        timeout: TimeInterval? = 5
    ) throws {
        guard source.first == "/", source.count > 1,
              destination.first == "/", destination.count > 1,
              !source.utf8.contains(0), !destination.utf8.contains(0) else {
            throw IshError.from(ishErrInvalidArg)
        }
        let timeoutBudget = try IshSpawnTimeoutBudget(
            timeout: timeout,
            startedAt: ProcessInfo.processInfo.systemUptime
        )
        let lease = try callGate.acquireCall()
        defer { lease.release() }
        let timeoutMs = try timeoutBudget.remainingMilliseconds(
            at: ProcessInfo.processInfo.systemUptime
        )
        var guestErrno: Int32 = 0
        let rc = source.withCString { sourceC in
            destination.withCString { destinationC in
                nativeCalls.renameNoReplace(
                    lease.raw, sourceC, destinationC, timeoutMs, &guestErrno
                )
            }
        }
        if rc == ishUnsupported {
            throw IshError.raw(
                rc,
                "Atomic no-replace rename requires IshEmbed v0.4.0-abi.7 or newer."
            )
        }
        if rc != ishOK { throw IshError.from(rc) }
        if guestErrno == 17 {
            throw IshFilesystemError.destinationExists
        }
        if guestErrno != 0 {
            throw IshFilesystemError.guestErrno(guestErrno)
        }
    }

    /// Spawn a streaming session.
    public func spawn(_ opts: IshSpawnOptions) throws -> IshSession {
        let timeoutBudget = try IshSpawnTimeoutBudget(
            timeout: opts.timeout,
            startedAt: ProcessInfo.processInfo.systemUptime
        )
        let lease = try callGate.acquireCall()
        let tty = opts.allocateTTY
        do {
            return try withCSpawnOpts(opts) { cOpts in
                let (rc, sess) = try nativeCalls.spawn(
                    lease.raw,
                    cOpts,
                    timeoutBudget
                )
                if rc != ishOK { throw IshError.from(rc) }
                guard let sess = sess else { throw IshError.from(ishErrInternal) }

                // The admitted native-call lease is transferred without a
                // zero-count window to the returned session. It remains held
                // until that session has completed native close.
                return IshSession(raw: sess,
                                  isTTY: tty,
                                  instanceLease: lease,
                                  nativeClose: nativeCalls.sessionClose)
            }
        } catch {
            lease.release()
            throw error
        }
    }

    // MARK: - private helpers

    private func withCSpawnOpts<R>(
        _ opts: IshSpawnOptions,
        _ body: (UnsafeMutablePointer<ish_embed_spawn_opts_t>) throws -> R
    ) throws -> R {
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
                // The live native adapter resolves this immediately before C
                // entry. Zero is only a placeholder while Swift stages opts.
                cOpts.timeout_ms = 0
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

/// Result of reserving the single native shutdown transition.
enum IshInstanceShutdownAttempt {
    case notRunning
    case ready(OpaquePointer)
}

/// Owns the v0.3.3 instance handle and prevents it from being freed while a
/// Swift operation can still use it. The binary currently referenced by
/// Package.swift predates native instance retain/release, so every admitted
/// call is represented by a Swift lifetime lease instead.
final class IshInstanceCallGate: @unchecked Sendable {
    private enum State {
        case idle
        case booting
        case running(OpaquePointer)
        case shuttingDown(OpaquePointer)
        case quarantined(OpaquePointer)
        case consumed
    }

    private var state: State = .idle
    private var activeLeases = 0
    private let lock = NSLock()

    var isRunning: Bool {
        lock.lock()
        defer { lock.unlock() }
        switch state {
        case .running, .shuttingDown:
            return true
        case .idle, .booting, .quarantined, .consumed:
            return false
        }
    }

    /// Reserve the whole boot transition, including the native call.
    func beginBoot() throws {
        lock.lock()
        defer { lock.unlock() }
        switch state {
        case .idle:
            state = .booting
        case .running:
            throw IshError.from(ishErrAlreadyBooted)
        case .quarantined, .consumed:
            // A shutdown attempt consumes boot admission for the only
            // lifecycle supported by iSH's process-global/TLS runtime.
            throw IshError.from(ishErrAlreadyBooted)
        case .booting, .shuttingDown:
            throw IshError.busy()
        }
    }

    /// Publish a successful native handle, or return to idle after failure.
    func finishBoot(_ raw: OpaquePointer?) {
        lock.lock()
        guard case .booting = state else {
            lock.unlock()
            preconditionFailure("finishBoot without a reserved boot transition")
        }
        if let raw {
            state = .running(raw)
        } else {
            state = .idle
        }
        lock.unlock()
    }

    /// Admit a native call and pin its instance handle until release.
    func acquireCall() throws -> IshInstanceCallLease {
        lock.lock()
        defer { lock.unlock() }
        switch state {
        case .running(let raw):
            activeLeases += 1
            return IshInstanceCallLease(raw: raw, owner: self)
        case .idle, .quarantined, .consumed:
            throw IshError.from(ishErrNotRunning)
        case .booting, .shuttingDown:
            throw IshError.busy()
        }
    }

    /// Atomically closes admission before checking existing call/session
    /// leases. Busy is reported immediately; shutdown never waits for an
    /// arbitrary native call to finish and never calls old native free while
    /// a lease exists.
    func beginShutdown() throws -> IshInstanceShutdownAttempt {
        lock.lock()
        defer { lock.unlock() }
        switch state {
        case .idle, .consumed:
            return .notRunning
        case .booting, .shuttingDown:
            throw IshError.busy()
        case .quarantined(let raw):
            // Native shutdown already closed ordinary admission, but it still
            // owns the allocation and threads until a retry completes cleanup.
            state = .shuttingDown(raw)
            return .ready(raw)
        case .running(let raw):
            state = .shuttingDown(raw)
            guard activeLeases == 0 else {
                state = .running(raw)
                throw IshError.busy()
            }
            return .ready(raw)
        }
    }

    /// Commit native destruction on success. Busy means native shutdown never
    /// closed admission, so the running handle can be restored. Every other
    /// failure quarantines ordinary calls while retaining the handle solely for
    /// a later shutdown retry that can finish native thread cleanup.
    func finishShutdown(attempted: OpaquePointer, result: Int32) {
        lock.lock()
        guard case .shuttingDown(let current) = state,
              current == attempted else {
            lock.unlock()
            preconditionFailure("finishShutdown without its reserved transition")
        }
        if result == ishOK {
            state = .consumed
        } else if result == ishErrBusy {
            state = .running(current)
        } else {
            state = .quarantined(current)
        }
        lock.unlock()
    }

    fileprivate func releaseCall() {
        lock.lock()
        precondition(activeLeases > 0, "unbalanced instance call lease")
        activeLeases -= 1
        lock.unlock()
    }
}

/// A single admitted instance call. The lease can move from a successful
/// spawn call into IshSession without decrementing the gate in between.
final class IshInstanceCallLease: @unchecked Sendable {
    let raw: OpaquePointer

    private var owner: IshInstanceCallGate?
    private let lock = NSLock()

    fileprivate init(raw: OpaquePointer, owner: IshInstanceCallGate) {
        self.raw = raw
        self.owner = owner
    }

    func release() {
        lock.lock()
        let owner = self.owner
        self.owner = nil
        lock.unlock()
        owner?.releaseCall()
    }

    deinit {
        release()
    }
}

/// Serializes a raw v0.3.3-compatible session handle with its close operation.
/// Kept internal so the ordering can be tested without booting a RootFS.
final class IshSessionCallGate: @unchecked Sendable {
    private var raw: OpaquePointer?
    private var activeCalls = 0
    private let condition = NSCondition()

    init(raw: OpaquePointer) {
        self.raw = raw
    }

    func withRaw<R>(_ body: (OpaquePointer) throws -> R) throws -> R {
        condition.lock()
        guard let r = raw else {
            condition.unlock()
            throw IshError.from(ishErrNoSession)
        }
        activeCalls += 1
        condition.unlock()

        defer {
            condition.lock()
            activeCalls -= 1
            if activeCalls == 0 { condition.broadcast() }
            condition.unlock()
        }
        return try body(r)
    }

    func close(onDetached: (() -> Void)? = nil,
               _ body: (OpaquePointer) -> Void) {
        condition.lock()
        let r = raw
        raw = nil
        guard let r else {
            condition.unlock()
            return
        }
        condition.unlock()
        onDetached?()

        condition.lock()
        while activeCalls != 0 { condition.wait() }
        condition.unlock()
        body(r)
    }
}

/// A streaming session — a single Linux process inside iSH.
public final class IshSession: @unchecked Sendable {
    /// Stage 1 cannot call the new native retain/release symbols while
    /// Package.swift still points at v0.3.3. The gate therefore tracks Swift
    /// calls and makes close wait for every admitted call. A nil-timeout read
    /// polls in bounded intervals, but another old/custom-supervisor C call
    /// can still determine an unbounded close wait if it never returns.
    private let callGate: IshSessionCallGate
    /// The instance lease is released only after native session close. This
    /// keeps v0.3.3's parent instance alive for the full session lifetime.
    private let instanceLease: IshInstanceCallLease
    private let nativeClose: (OpaquePointer) -> Void
    private static let readPollMilliseconds: UInt32 = 100
    /// True if the session was spawned with allocateTTY = true. In that
    /// case the supervisor wired the child to a pty, so Ctrl+C and other
    /// terminal-style signals must be delivered as control bytes on the
    /// stdin pipe (the kernel tty layer turns them into pgrp signals),
    /// not as guest-side `kill -SIGINT` which would hit the shell itself.
    private let isTTY: Bool

    init(raw: OpaquePointer,
         isTTY: Bool = false,
         instanceLease: IshInstanceCallLease,
         nativeClose: @escaping (OpaquePointer) -> Void) {
        self.callGate = IshSessionCallGate(raw: raw)
        self.isTTY = isTTY
        self.instanceLease = instanceLease
        self.nativeClose = nativeClose
    }

    deinit {
        close()
    }

    /// Wait up to `timeout` for the next event. nil means forever, a finite
    /// non-positive value performs one nonblocking read, and non-finite values
    /// are rejected as invalid arguments.
    public func read(timeout: TimeInterval?) throws -> IshSessionEvent {
        if let timeout, !timeout.isFinite {
            throw IshError.from(ishErrInvalidArg)
        }
        if let timeout, timeout <= 0 {
            return try readOnce(waitMilliseconds: 0)
        }

        let deadline = timeout.map {
            ProcessInfo.processInfo.systemUptime + $0
        }

        while true {
            let waitMilliseconds: UInt32
            if let deadline {
                let remaining = deadline - ProcessInfo.processInfo.systemUptime
                if remaining <= 0 { throw IshError.from(ishErrTimeout) }
                waitMilliseconds = UInt32(min(
                    Double(Self.readPollMilliseconds),
                    max(1, ceil(remaining * 1_000))))
            } else {
                waitMilliseconds = Self.readPollMilliseconds
            }

            do {
                return try readOnce(waitMilliseconds: waitMilliseconds)
            } catch IshError.raw(let code, _) where code == ishErrTimeout {
                if deadline == nil { continue }
            }
        }
    }

    private func readOnce(waitMilliseconds: UInt32) throws -> IshSessionEvent {
        try withRawCall { r in
            var buf: UnsafeMutablePointer<UInt8>? = nil
            var len: Int = 0
            var kind: Int32 = 0
            var seq: UInt64 = 0
            var exitCode: Int32 = 0
            var signal: Int32 = 0
            let rc = ish_embed_session_read(r, waitMilliseconds,
                                             &buf, &len, &kind, &seq,
                                             &exitCode, &signal)
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
    }

    /// Queues stdin bytes in order. Finite-timeout sessions reuse the original
    /// SPAWN admission deadline, so a stalled transport cannot make this call
    /// wait indefinitely. A failed multi-frame write may have admitted a
    /// prefix; callers requiring transactional semantics must stage input.
    public func write(_ data: Data) throws {
        try withRawCall { r in
            try data.withUnsafeBytes { rawBuf in
                let rc = ish_embed_session_write(
                    r,
                    rawBuf.baseAddress?.assumingMemoryBound(to: UInt8.self),
                    rawBuf.count)
                if rc != ishOK { throw IshError.from(rc) }
            }
        }
    }

    /// Finite-timeout sessions reuse their native SPAWN admission deadline.
    /// Expiry returns `ISH_ERR_TIMEOUT` without publishing a late EOF frame.
    public func closeStdin() throws {
        try withRawCall { r in
            let rc = ish_embed_session_close_stdin(r)
            if rc != ishOK { throw IshError.from(rc) }
        }
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
        try withRawCall { r in
            let rc = ish_embed_session_signal(r, signum)
            if rc != ishOK { throw IshError.from(rc) }
        }
    }

    /// Send a real signal to the session process group even for TTY sessions.
    public func signalDirect(_ signum: Int32) throws {
        try withRawCall { r in
            let rc = ish_embed_session_signal(r, signum)
            if rc != ishOK { throw IshError.from(rc) }
        }
    }

    /// Resize the session's pty and deliver SIGWINCH to the foreground
    /// process group. Silently no-op for non-TTY sessions. `xpixel` /
    /// `ypixel` are informational; pass 0 if unknown.
    public func resize(rows: UInt16, cols: UInt16,
                       xpixel: UInt16 = 0, ypixel: UInt16 = 0) throws {
        try withRawCall { r in
            let rc = ish_embed_session_resize(r, rows, cols, xpixel, ypixel)
            if rc != ishOK { throw IshError.from(rc) }
        }
    }

    /// Convenience: SIGINT (Ctrl+C). In TTY mode this is a control byte
    /// to the pty master, otherwise a direct kill(-pgid, SIGINT).
    public func interrupt() throws { try signal(2) }

    /// SIGTERM, then SIGKILL after grace.
    public func terminate(graceMs: UInt32 = 1_500) throws {
        try withRawCall { r in
            let rc = ish_embed_session_terminate(r, graceMs)
            if rc != ishOK { throw IshError.from(rc) }
        }
    }

    public func close() {
        callGate.close {
            nativeClose($0)
            instanceLease.release()
        }
    }

    private func withRawCall<R>(
        _ body: (OpaquePointer) throws -> R
    ) throws -> R {
        try callGate.withRaw(body)
    }
}

// MARK: - helpers

private func bytesToData(
    _ buf: UnsafeMutablePointer<UInt8>?,
    length: Int,
    freeBuffer: @escaping (UnsafeMutablePointer<UInt8>) -> Void = {
        ish_embed_free($0)
    }
) -> Data {
    guard let buf = buf, length > 0 else {
        if let buf = buf { freeBuffer(buf) }
        return Data()
    }
    return Data(bytesNoCopy: buf, count: length,
                deallocator: .custom { p, _ in
                    freeBuffer(p.assumingMemoryBound(to: UInt8.self))
                })
}
