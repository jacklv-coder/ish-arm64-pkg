import XCTest
@testable import IshEmbed

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
        var results: [Int32] = Array(repeating: -1, count: 4)
        let queue = DispatchQueue(label: "ish.concurrent", attributes: .concurrent)
        for i in 0..<4 {
            group.enter()
            queue.async {
                defer { group.leave() }
                let r = try? IshInstance.shared.runOneshot(.init(argv: ["/bin/echo", "x\(i)"]))
                results[i] = r?.exitCode ?? -1
            }
        }
        group.wait()
        XCTAssertEqual(results, [0,0,0,0])
    }
}
