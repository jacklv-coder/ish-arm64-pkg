import Foundation
import XCTest
import CIshEmbed
@testable import IshEmbed

private let ishUnsupported: Int32 = -22

private final class LockedResults: @unchecked Sendable {
    private let lock = NSLock()
    private var values: [Int32]

    init(count: Int) {
        values = Array(repeating: -1, count: count)
    }

    func set(_ value: Int32, at index: Int) {
        lock.lock()
        defer { lock.unlock() }
        values[index] = value
    }

    func snapshot() -> [Int32] {
        lock.lock()
        defer { lock.unlock() }
        return values
    }
}

private func ishErrorCode(_ error: Error) -> Int32? {
    guard case IshError.raw(let code, _) = error else { return nil }
    return code
}

private func mallocBuffer(_ data: Data) -> UnsafeMutablePointer<UInt8> {
    let raw = malloc(data.count)!
    data.withUnsafeBytes { bytes in
        _ = memcpy(raw, bytes.baseAddress!, data.count)
    }
    return raw.assumingMemoryBound(to: UInt8.self)
}

/// Compile-time source-compatibility probe: an ordinary escaping callback must
/// remain accepted without requiring callers to declare it `@Sendable`.
private func installLegacyTerminalEventHandler(
    on terminal: IshTerminal,
    handler: @escaping (IshTerminal.Event) -> Void
) {
    terminal.setEventHandler(handler)
}

private final class LifecycleNativeHarness: @unchecked Sendable {
    let instanceRaw = OpaquePointer(bitPattern: 0x701)!
    let sessionRaw = OpaquePointer(bitPattern: 0x702)!

    private let spawnResult: Int32
    private var shutdownResults: [Int32]
    private let oneshotBody:
        (UnsafeMutablePointer<ish_embed_oneshot_result_t>) -> Int32
    private let timeoutObservedBody: (UInt32) -> Void
    private let spawnTimeoutObservedBody: (UInt32) -> Void
    private let renameBody:
        (String, String, UInt32, UnsafeMutablePointer<Int32>) -> Int32
    private let sessionCloseBody: () -> Void
    private let bufferFreedBody: () -> Void
    private let lock = NSLock()
    private var bootCalls = 0
    private var shutdownCalls = 0
    private var oneshotCalls = 0
    private var spawnCalls = 0
    private var renameCalls = 0
    private var sessionCloseCalls = 0
    private var freeBufferCalls = 0
    private var oneshotResultWasZeroed = true

    init(spawnResult: Int32 = 0,
         shutdownResults: [Int32] = [0],
         oneshotBody: @escaping
            (UnsafeMutablePointer<ish_embed_oneshot_result_t>) -> Int32 = {
                _ in -99
            },
         timeoutObservedBody: @escaping (UInt32) -> Void = { _ in },
         spawnTimeoutObservedBody: @escaping (UInt32) -> Void = { _ in },
         renameBody: @escaping
            (String, String, UInt32, UnsafeMutablePointer<Int32>) -> Int32 = {
                _, _, _, _ in ishUnsupported
            },
         sessionCloseBody: @escaping () -> Void = {},
         bufferFreedBody: @escaping () -> Void = {}) {
        self.spawnResult = spawnResult
        self.shutdownResults = shutdownResults
        self.oneshotBody = oneshotBody
        self.timeoutObservedBody = timeoutObservedBody
        self.spawnTimeoutObservedBody = spawnTimeoutObservedBody
        self.renameBody = renameBody
        self.sessionCloseBody = sessionCloseBody
        self.bufferFreedBody = bufferFreedBody
    }

    func nativeCalls() -> IshLifecycleNativeCalls {
        IshLifecycleNativeCalls(
            boot: { [self] _ in
                lock.lock()
                bootCalls += 1
                lock.unlock()
                return (0, instanceRaw)
            },
            shutdown: { [self] _, _ in
                lock.lock()
                shutdownCalls += 1
                let result = shutdownResults.isEmpty
                    ? Int32(0)
                    : shutdownResults.removeFirst()
                lock.unlock()
                return result
            },
            runOneshot: { [self] _, opts, timeoutBudget, result in
                try timeoutBudget.apply(
                    to: opts,
                    at: ProcessInfo.processInfo.systemUptime
                )
                let initial = result.pointee
                let wasZeroed = initial.exit_code == 0
                    && initial.signal == 0
                    && initial.stdout_buf == nil
                    && initial.stdout_len == 0
                    && initial.stderr_buf == nil
                    && initial.stderr_len == 0
                    && initial.timed_out == 0
                lock.lock()
                oneshotCalls += 1
                oneshotResultWasZeroed = oneshotResultWasZeroed && wasZeroed
                lock.unlock()
                timeoutObservedBody(opts.pointee.timeout_ms)
                return oneshotBody(result)
            },
            spawn: { [self] _, opts, timeoutBudget in
                try timeoutBudget.apply(
                    to: opts,
                    at: ProcessInfo.processInfo.systemUptime
                )
                lock.lock()
                spawnCalls += 1
                lock.unlock()
                spawnTimeoutObservedBody(opts.pointee.timeout_ms)
                return (spawnResult,
                        spawnResult == 0 ? sessionRaw : nil)
            },
            renameNoReplace: { [self] _, source, destination, timeoutMs,
                               guestErrno in
                lock.lock()
                renameCalls += 1
                lock.unlock()
                return renameBody(
                    String(cString: source),
                    String(cString: destination),
                    timeoutMs,
                    guestErrno
                )
            },
            sessionClose: { [self] _ in
                lock.lock()
                sessionCloseCalls += 1
                lock.unlock()
                sessionCloseBody()
            },
            freeBuffer: { [self] buffer in
                lock.lock()
                freeBufferCalls += 1
                lock.unlock()
                free(buffer)
                bufferFreedBody()
            })
    }

    func counts() -> (boot: Int, shutdown: Int, oneshot: Int, spawn: Int,
                      rename: Int,
                      close: Int, freeBuffer: Int, resultWasZeroed: Bool) {
        lock.lock()
        defer { lock.unlock() }
        return (bootCalls, shutdownCalls, oneshotCalls, spawnCalls, renameCalls,
                sessionCloseCalls, freeBufferCalls,
                oneshotResultWasZeroed)
    }
}

/// These tests require a built rootfs at IshEmbedTests.rootfsPath.
/// They are skipped when ISH_EMBED_ROOTFS is not set.
final class IshEmbedTests: XCTestCase {

    static var rootfsPath: String? {
        ProcessInfo.processInfo.environment["ISH_EMBED_ROOTFS"]
    }

    override class func setUp() {
        guard let rootfs = rootfsPath else { return }
        try? IshInstance.shared.boot(.init(rootfsPath: rootfs, workdir: "/"))
    }

    override class func tearDown() {
        try? IshInstance.shared.shutdown(graceMs: 2000)
    }

    /// RootFS-independent compile-time smoke used by the Stage 1 manifest
    /// binary gate. It keeps the existing writable API surface exercised even
    /// when the native integration tests are intentionally skipped.
    func testTerminalKeyEncoderRemainsWritableForSourceCompatibility() {
        let _: WritableKeyPath<IshTerminal, IshKeyEncoder> = \IshTerminal.keyEncoder
        _ = installLegacyTerminalEventHandler
    }

    func testProductionOneshotLeaseBlocksShutdownUntilNativeReturns() throws {
        let callEntered = DispatchSemaphore(value: 0)
        let releaseCall = DispatchSemaphore(value: 0)
        let callDone = DispatchSemaphore(value: 0)
        let bufferFreed = DispatchSemaphore(value: 0)
        let results = LockedResults(count: 1)
        let native = LifecycleNativeHarness(
            oneshotBody: { result in
                callEntered.signal()
                releaseCall.wait()

                let stdout = Data("oneshot-out\n".utf8)
                let stderr = Data("oneshot-err\n".utf8)
                result.pointee.exit_code = 7
                result.pointee.signal = 0
                result.pointee.stdout_buf = mallocBuffer(stdout)
                result.pointee.stdout_len = stdout.count
                result.pointee.stderr_buf = mallocBuffer(stderr)
                result.pointee.stderr_len = stderr.count
                result.pointee.timed_out = 0
                return 0
            },
            bufferFreedBody: {
                bufferFreed.signal()
            })
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))

        DispatchQueue.global().async {
            do {
                var result: IshOneshotResult? = try instance.runOneshot(
                    .init(argv: ["/bin/true"]))
                let valid = result?.exitCode == 7
                    && result?.signal == 0
                    && result?.stdoutData == Data("oneshot-out\n".utf8)
                    && result?.stderrData == Data("oneshot-err\n".utf8)
                    && result?.timedOut == false
                result = nil
                results.set(valid ? 0 : -1, at: 0)
            } catch {
                results.set(ishErrorCode(error) ?? -999, at: 0)
            }
            callDone.signal()
        }
        XCTAssertEqual(callEntered.wait(timeout: .now() + 1), .success)

        XCTAssertThrowsError(try instance.shutdown()) { error in
            XCTAssertEqual(ishErrorCode(error), -19)
            XCTAssertTrue(String(describing: error).localizedCaseInsensitiveContains("busy"))
        }
        var counts = native.counts()
        XCTAssertEqual(counts.oneshot, 1)
        XCTAssertTrue(counts.resultWasZeroed)
        XCTAssertEqual(counts.shutdown, 0,
                       "active oneshot lease must block native instance free")

        releaseCall.signal()
        XCTAssertEqual(callDone.wait(timeout: .now() + 1), .success)
        XCTAssertEqual(results.snapshot(), [0])
        XCTAssertEqual(bufferFreed.wait(timeout: .now() + 1), .success)
        XCTAssertEqual(bufferFreed.wait(timeout: .now() + 1), .success)
        counts = native.counts()
        XCTAssertEqual(counts.freeBuffer, 2)

        try instance.shutdown()
        XCTAssertEqual(native.counts().shutdown, 1)
    }

    func testAtomicRenameMapsGuestResultsAndValidatesPaths() throws {
        var observed: [(String, String, UInt32)] = []
        let native = LifecycleNativeHarness(
            renameBody: { source, destination, timeoutMs, guestErrno in
                observed.append((source, destination, timeoutMs))
                guestErrno.pointee =
                    destination.hasSuffix("exists") ? 17 :
                    destination.hasSuffix("denied") ? 13 : 0
                return 0
            })
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))

        try instance.renameNoReplace(
            from: "/workspace/source",
            to: "/workspace/renamed"
        )
        XCTAssertThrowsError(
            try instance.renameNoReplace(
                from: "/workspace/source",
                to: "/workspace/exists"
            )
        ) { error in
            XCTAssertEqual(error as? IshFilesystemError, .destinationExists)
        }
        XCTAssertThrowsError(
            try instance.renameNoReplace(
                from: "/workspace/source",
                to: "/workspace/denied"
            )
        ) { error in
            XCTAssertEqual(error as? IshFilesystemError, .guestErrno(13))
        }
        XCTAssertThrowsError(
            try instance.renameNoReplace(from: "relative", to: "/valid")
        ) { error in
            XCTAssertEqual(ishErrorCode(error), ISH_ERR_INVALID_ARG.rawValue)
        }

        XCTAssertEqual(observed.count, 3)
        XCTAssertEqual(observed.first?.0, "/workspace/source")
        XCTAssertEqual(observed.first?.1, "/workspace/renamed")
        XCTAssertTrue(observed.allSatisfy { $0.2 > 0 && $0.2 <= 5_000 })
        XCTAssertEqual(native.counts().rename, 3)
        try instance.shutdown()
    }

    func testAtomicRenameReportsOlderNativeBinaryClearly() throws {
        let native = LifecycleNativeHarness()
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))

        XCTAssertThrowsError(
            try instance.renameNoReplace(from: "/source", to: "/destination")
        ) { error in
            XCTAssertEqual(ishErrorCode(error), ishUnsupported)
            XCTAssertTrue(String(describing: error).contains("abi.7"))
        }
        try instance.shutdown()
    }

    func testAtomicRenameLeaseBlocksConcurrentShutdown() throws {
        let entered = DispatchSemaphore(value: 0)
        let release = DispatchSemaphore(value: 0)
        let finished = DispatchSemaphore(value: 0)
        let native = LifecycleNativeHarness(
            renameBody: { _, _, _, guestErrno in
                entered.signal()
                release.wait()
                guestErrno.pointee = 0
                return 0
            })
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))

        DispatchQueue.global().async {
            try? instance.renameNoReplace(from: "/source", to: "/destination")
            finished.signal()
        }
        XCTAssertEqual(entered.wait(timeout: .now() + 1), .success)
        XCTAssertThrowsError(try instance.shutdown()) { error in
            XCTAssertEqual(ishErrorCode(error), ISH_ERR_BUSY.rawValue)
        }
        release.signal()
        XCTAssertEqual(finished.wait(timeout: .now() + 1), .success)
        try instance.shutdown()
    }

    func testFailedSpawnReleasesInstanceLease() throws {
        let native = LifecycleNativeHarness(spawnResult: -15)
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))

        XCTAssertThrowsError(
            try instance.spawn(.init(argv: ["/bin/true"]))) {
            XCTAssertEqual(ishErrorCode($0), -15)
        }

        // If spawn's lease leaked, this would return Swift-gate BUSY and the
        // injected native shutdown callback would never be reached.
        try instance.shutdown()
        let counts = native.counts()
        XCTAssertEqual(counts.spawn, 1)
        XCTAssertEqual(counts.close, 0)
        XCTAssertEqual(counts.shutdown, 1)
    }

    func testProductionSessionLeaseSurvivesUntilNativeCloseReturns() throws {
        let closeEntered = DispatchSemaphore(value: 0)
        let releaseClose = DispatchSemaphore(value: 0)
        let closeDone = DispatchSemaphore(value: 0)
        let native = LifecycleNativeHarness(sessionCloseBody: {
            closeEntered.signal()
            releaseClose.wait()
        })
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))
        let session = try instance.spawn(.init(argv: ["/bin/true"]))

        DispatchQueue.global().async {
            session.close()
            closeDone.signal()
        }
        XCTAssertEqual(closeEntered.wait(timeout: .now() + 1), .success)
        XCTAssertEqual(closeDone.wait(timeout: .now() + 0.05), .timedOut)

        XCTAssertThrowsError(try instance.shutdown()) { error in
            XCTAssertEqual(ishErrorCode(error), -19)
            XCTAssertTrue(String(describing: error).localizedCaseInsensitiveContains("busy"))
        }
        XCTAssertEqual(native.counts().shutdown, 0,
                       "active session lease must block native instance free")

        releaseClose.signal()
        XCTAssertEqual(closeDone.wait(timeout: .now() + 1), .success)
        try instance.shutdown()
        let counts = native.counts()
        XCTAssertEqual(counts.close, 1)
        XCTAssertEqual(counts.shutdown, 1)
    }

    func testSessionDeinitClosesNativeSessionAndReleasesLease() throws {
        let closeObserved = DispatchSemaphore(value: 0)
        let native = LifecycleNativeHarness(sessionCloseBody: {
            closeObserved.signal()
        })
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))

        weak var weakSession: IshSession?
        var session: IshSession? = try instance.spawn(
            .init(argv: ["/bin/true"]))
        weakSession = session
        session = nil

        XCTAssertEqual(closeObserved.wait(timeout: .now() + 1), .success)
        XCTAssertNil(weakSession)
        try instance.shutdown()
        let counts = native.counts()
        XCTAssertEqual(counts.close, 1)
        XCTAssertEqual(counts.shutdown, 1)
    }

    func testNativeShutdownFailureRestoresHandleAndAllowsRetry() throws {
        let native = LifecycleNativeHarness(shutdownResults: [-19, 0])
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))

        XCTAssertThrowsError(try instance.shutdown()) {
            XCTAssertEqual(ishErrorCode($0), -19)
        }
        XCTAssertTrue(instance.isRunning)

        // A real production spawn after native failure proves that admission
        // and the original instance handle were restored for retry.
        let session = try instance.spawn(.init(argv: ["/bin/true"]))
        session.close()
        try instance.shutdown()
        XCTAssertFalse(instance.isRunning)
        let counts = native.counts()
        XCTAssertEqual(counts.shutdown, 2)
        XCTAssertEqual(counts.spawn, 1)
        XCTAssertEqual(counts.close, 1)
    }

    func testNativeShutdownTimeoutQuarantinesHandle() throws {
        let native = LifecycleNativeHarness(shutdownResults: [-12, 0])
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))

        XCTAssertThrowsError(try instance.shutdown()) {
            XCTAssertEqual(ishErrorCode($0), -12)
        }
        XCTAssertFalse(instance.isRunning)
        XCTAssertThrowsError(try instance.runOneshot(
            .init(argv: ["/bin/true"]))) {
            XCTAssertEqual(ishErrorCode($0), -9)
        }
        XCTAssertThrowsError(
            try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))) {
            XCTAssertEqual(ishErrorCode($0), -10)
        }
        XCTAssertNoThrow(try instance.shutdown(),
                         "a quarantined timeout must permit cleanup retry")
        XCTAssertNoThrow(try instance.shutdown(),
                         "shutdown is idempotent after retry succeeds")
        let counts = native.counts()
        XCTAssertEqual(counts.boot, 1)
        XCTAssertEqual(counts.shutdown, 2,
                       "timeout must preserve native shutdown cleanup")
        XCTAssertEqual(counts.oneshot, 0,
                       "timeout must not re-admit the native handle")
    }

    func testSuccessfulShutdownConsumesProductionInstanceLifecycle() throws {
        let native = LifecycleNativeHarness()
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))
        try instance.shutdown()

        XCTAssertThrowsError(
            try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))) {
            XCTAssertEqual(ishErrorCode($0), -10)
        }
        XCTAssertNoThrow(try instance.shutdown(),
                         "shutdown remains idempotent after consumption")
        let counts = native.counts()
        XCTAssertEqual(counts.boot, 1,
                       "consumed lifecycle must not re-enter native boot")
        XCTAssertEqual(counts.shutdown, 1)
    }

    func testTimeoutConversionRejectsNonFiniteValuesBeforeNativeEntry() throws {
        let observedTimeout = LockedResults(count: 1)
        let observedSpawnTimeout = LockedResults(count: 1)
        let native = LifecycleNativeHarness(
            oneshotBody: { _ in -12 },
            timeoutObservedBody: { value in
                observedTimeout.set(Int32(value), at: 0)
            },
            spawnTimeoutObservedBody: { value in
                observedSpawnTimeout.set(Int32(value), at: 0)
            })
        let instance = IshInstance(nativeCalls: native.nativeCalls())
        try instance.boot(.init(rootfsPath: "/unused-test-rootfs"))

        for invalid in [TimeInterval.nan,
                        TimeInterval.infinity,
                        -TimeInterval.infinity] {
            XCTAssertThrowsError(try instance.runOneshot(
                .init(argv: ["/bin/true"], timeout: invalid))) {
                XCTAssertEqual(ishErrorCode($0), -13)
            }
        }
        XCTAssertEqual(native.counts().oneshot, 0,
                       "invalid timeout must not enter native oneshot")

        XCTAssertThrowsError(try instance.runOneshot(
            .init(argv: ["/bin/true"], timeout: 1))) {
            XCTAssertEqual(ishErrorCode($0), -12)
        }
        XCTAssertTrue((1...1_000).contains(observedTimeout.snapshot()[0]),
                      "finite oneshot timeout must reach native entry")

        for invalid in [TimeInterval.nan,
                        TimeInterval.infinity,
                        -TimeInterval.infinity] {
            XCTAssertThrowsError(try instance.spawn(
                .init(argv: ["/bin/true"], timeout: invalid))) {
                XCTAssertEqual(ishErrorCode($0), -13)
            }
        }
        XCTAssertEqual(native.counts().spawn, 0,
                       "invalid timeout must not enter native spawn")

        let session = try instance.spawn(
            .init(argv: ["/bin/true"], timeout: 1))
        XCTAssertTrue((1...1_000).contains(observedSpawnTimeout.snapshot()[0]),
                       "finite streaming timeout must reach native spawn")
        for invalid in [TimeInterval.nan,
                        TimeInterval.infinity,
                        -TimeInterval.infinity] {
            XCTAssertThrowsError(try session.read(timeout: invalid)) {
                XCTAssertEqual(ishErrorCode($0), -13)
            }
        }
        session.close()
        try instance.shutdown()
        let counts = native.counts()
        XCTAssertEqual(counts.oneshot, 1)
        XCTAssertEqual(counts.spawn, 1)
        XCTAssertEqual(counts.close, 1)
        XCTAssertEqual(counts.shutdown, 1)
    }

    func testTimeoutBudgetDeductsSwiftStagingBeforeNativeEntry() throws {
        let budget = try IshSpawnTimeoutBudget(
            timeout: 0.100,
            startedAt: 500
        )
        let remaining = try budget.remainingMilliseconds(at: 500.025)
        XCTAssertTrue((74...75).contains(remaining))

        XCTAssertThrowsError(
            try budget.remainingMilliseconds(at: 500.100)
        ) {
            XCTAssertEqual(ishErrorCode($0), -12)
        }

        let submillisecond = try IshSpawnTimeoutBudget(
            timeout: 0.000_9,
            startedAt: 700
        )
        XCTAssertThrowsError(
            try submillisecond.remainingMilliseconds(at: 700)
        ) {
            XCTAssertEqual(ishErrorCode($0), -12)
        }

        let legacy = try IshSpawnTimeoutBudget(
            timeout: 0,
            startedAt: 900
        )
        XCTAssertEqual(
            try legacy.remainingMilliseconds(at: 1_000),
            0,
            "non-positive timeout must retain legacy unbounded semantics"
        )
    }

    func testShutdownGateRejectsNewCallsAndConcurrentShutdown() throws {
        let gate = IshInstanceCallGate()
        let handle = OpaquePointer(bitPattern: 0x104)!
        try gate.beginBoot()
        gate.finishBoot(handle)

        guard case .ready(let attempted) = try gate.beginShutdown() else {
            return XCTFail("running gate did not reserve shutdown")
        }

        let results = LockedResults(count: 1)
        let callDone = DispatchSemaphore(value: 0)
        DispatchQueue.global().async {
            do {
                let lease = try gate.acquireCall()
                lease.release()
                results.set(0, at: 0)
            } catch {
                results.set(ishErrorCode(error) ?? -999, at: 0)
            }
            callDone.signal()
        }
        XCTAssertEqual(callDone.wait(timeout: .now() + 1), .success)
        XCTAssertEqual(results.snapshot(), [-19])
        XCTAssertThrowsError(try gate.beginShutdown()) {
            XCTAssertEqual(ishErrorCode($0), -19)
        }

        gate.finishShutdown(attempted: attempted, result: -19)
        let restoredLease = try gate.acquireCall()
        restoredLease.release()
    }

    func testBootTransitionSerializesBootCallsAndShutdown() throws {
        let gate = IshInstanceCallGate()
        let handle = OpaquePointer(bitPattern: 0x105)!

        try gate.beginBoot()
        XCTAssertFalse(gate.isRunning)
        XCTAssertThrowsError(try gate.beginBoot()) {
            XCTAssertEqual(ishErrorCode($0), -19)
        }
        XCTAssertThrowsError(try gate.acquireCall()) {
            XCTAssertEqual(ishErrorCode($0), -19)
        }
        XCTAssertThrowsError(try gate.beginShutdown()) {
            XCTAssertEqual(ishErrorCode($0), -19)
        }

        gate.finishBoot(handle)
        XCTAssertTrue(gate.isRunning)
        XCTAssertThrowsError(try gate.beginBoot()) {
            XCTAssertEqual(ishErrorCode($0), -10)
        }

        guard case .ready(let attempted) = try gate.beginShutdown() else {
            return XCTFail("booted gate did not reserve shutdown")
        }
        XCTAssertThrowsError(try gate.beginBoot()) {
            XCTAssertEqual(ishErrorCode($0), -19)
        }
        gate.finishShutdown(attempted: attempted, result: 0)
        XCTAssertFalse(gate.isRunning)
        XCTAssertThrowsError(try gate.beginBoot()) {
            XCTAssertEqual(ishErrorCode($0), -10)
        }
        XCTAssertThrowsError(try gate.acquireCall()) {
            XCTAssertEqual(ishErrorCode($0), -9)
        }
        guard case .notRunning = try gate.beginShutdown() else {
            return XCTFail("consumed gate did not preserve shutdown idempotence")
        }

        // A failed boot returns to idle and can be attempted again.
        let retryGate = IshInstanceCallGate()
        try retryGate.beginBoot()
        retryGate.finishBoot(nil)
        try retryGate.beginBoot()
        retryGate.finishBoot(nil)
    }

    func testSessionCloseWaitsForAdmittedCall() throws {
        let gate = IshSessionCallGate(raw: OpaquePointer(bitPattern: 0x1)!)
        let entered = DispatchSemaphore(value: 0)
        let releaseCall = DispatchSemaphore(value: 0)
        let callDone = DispatchSemaphore(value: 0)
        let detached = DispatchSemaphore(value: 0)
        let closeDone = DispatchSemaphore(value: 0)

        DispatchQueue.global().async {
            _ = try? gate.withRaw { _ in
                entered.signal()
                releaseCall.wait()
            }
            callDone.signal()
        }
        XCTAssertEqual(entered.wait(timeout: .now() + 1), .success)

        DispatchQueue.global().async {
            gate.close(onDetached: { detached.signal() }) { _ in }
            closeDone.signal()
        }
        XCTAssertEqual(detached.wait(timeout: .now() + 1), .success)
        XCTAssertEqual(closeDone.wait(timeout: .now() + 0.05), .timedOut)

        releaseCall.signal()
        XCTAssertEqual(callDone.wait(timeout: .now() + 1), .success)
        XCTAssertEqual(closeDone.wait(timeout: .now() + 1), .success)
        XCTAssertThrowsError(try gate.withRaw { _ in () })
    }

    func testEcho() throws {
        try XCTSkipIf(Self.rootfsPath == nil, "ISH_EMBED_ROOTFS not set")
        let r = try IshInstance.shared.runOneshot(.init(argv: ["/bin/echo", "hi"]))
        XCTAssertEqual(r.exitCode, 0)
        XCTAssertEqual(String(data: r.stdoutData, encoding: .utf8), "hi\n")
    }

    func testFalse() throws {
        try XCTSkipIf(Self.rootfsPath == nil, "ISH_EMBED_ROOTFS not set")
        let r = try IshInstance.shared.runOneshot(.init(argv: ["/bin/false"]))
        XCTAssertEqual(r.exitCode, 1)
    }

    func testStdin() throws {
        try XCTSkipIf(Self.rootfsPath == nil, "ISH_EMBED_ROOTFS not set")
        let s = try IshInstance.shared.spawn(.init(argv: ["/bin/cat"]))
        try s.write(Data("hello\n".utf8))
        try s.closeStdin()
        var got = Data()
        loop: while true {
            switch try s.read(timeout: 5) {
            case .data(let d, kind: .stdout, _): got.append(d)
            case .data(_, kind: .stderr, _): break
            case .exited: break loop
            }
        }
        XCTAssertEqual(String(data: got, encoding: .utf8), "hello\n")
    }

    func testCtrlCInterruptsSleep() throws {
        try XCTSkipIf(Self.rootfsPath == nil, "ISH_EMBED_ROOTFS not set")
        let s = try IshInstance.shared.spawn(.init(argv: ["/bin/sleep", "60"]))
        usleep(200_000)
        try s.interrupt() // SIGINT
        var sawExit = false
        var killedBySignal: Int32 = 0
        for _ in 0..<60 {
            switch try s.read(timeout: 1) {
            case .data: continue
            case .exited(_, let sig):
                sawExit = true
                killedBySignal = sig
                break
            }
            if sawExit { break }
        }
        XCTAssertTrue(sawExit)
        XCTAssertNotEqual(killedBySignal, 0)
    }

    func testConcurrentSessions() throws {
        try XCTSkipIf(Self.rootfsPath == nil, "ISH_EMBED_ROOTFS not set")
        let group = DispatchGroup()
        let results = LockedResults(count: 4)
        let queue = DispatchQueue(label: "ish.concurrent", attributes: .concurrent)
        for i in 0..<4 {
            group.enter()
            queue.async {
                defer { group.leave() }
                let r = try? IshInstance.shared.runOneshot(.init(argv: ["/bin/echo", "x\(i)"]))
                results.set(r?.exitCode ?? -1, at: i)
            }
        }
        group.wait()
        XCTAssertEqual(results.snapshot(), [0, 0, 0, 0])
    }
}
