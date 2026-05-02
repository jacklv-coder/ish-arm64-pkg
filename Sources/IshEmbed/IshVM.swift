// SPDX-License-Identifier: GPL-3.0-or-later
//
// VM management on top of IshInstance.
//
// Architecture: one iSH kernel runs as a single process; "VMs" are
// just per-VM directory trees under /srv/vms/<name>/ in the shared
// fakefs. New processes spawned with `chrootPath: "/srv/vms/<name>"`
// are confined to that tree — apk, /etc, /home, /root, /tmp etc. are
// fully isolated, and persist across app launches because the fakefs
// itself is in Application Support.
//
// On first boot the bundled rootfs already contains:
//
//     /srv/vms/.template/            ← stock Alpine to clone from
//     /srv/vms/                      ← parent dir for user VMs
//
// Creating a VM = `cp -a /srv/vms/.template/. /srv/vms/<name>/`
// (executed inside the guest, of course).
//

import Foundation

public struct IshVM: Identifiable, Hashable, Sendable {
    public let name: String          // also the directory name under /srv/vms/
    public var id: String { name }

    public init(name: String) { self.name = name }

    /// Guest absolute path of this VM's root.
    public var guestPath: String { "/srv/vms/\(name)" }
}

public enum IshVMError: Error {
    case invalidName(String)
    case alreadyExists(String)
    case notFound(String)
    case createFailed(String)
}

public extension IshInstance {

    /// Convenience: spawn inside a specific VM (chroot).
    func spawn(in vm: IshVM, _ opts: IshSpawnOptions) throws -> IshSession {
        var o = opts
        o.chrootPath = vm.guestPath
        // If the caller didn't override env/cwd, use sensible per-VM defaults.
        if o.cwd == nil { o.cwd = "/root" }
        return try spawn(o)
    }

    /// List existing VMs (`/srv/vms/<name>/` directories, ignoring
    /// dotfiles like `.template`).
    func listVMs() throws -> [IshVM] {
        let r = try runOneshot(.init(
            argv: ["/bin/sh", "-c",
                   "ls -1 /srv/vms 2>/dev/null | grep -v '^\\.' | sort"]
        ))
        guard r.exitCode == 0 else {
            // /srv/vms might not exist on a brand-new rootfs; treat as empty.
            return []
        }
        let names = String(decoding: r.stdoutData, as: UTF8.self)
            .split(whereSeparator: { $0.isNewline })
            .map { String($0).trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty && !$0.hasPrefix(".") }
        return names.map(IshVM.init(name:))
    }

    /// Create a new VM by cloning /srv/vms/.template into /srv/vms/<name>.
    /// This runs inside the guest (busybox cp -a). Idempotent: if the
    /// VM already exists, throws `.alreadyExists`.
    @discardableResult
    func createVM(name: String) throws -> IshVM {
        try validateVMName(name)
        if try listVMs().contains(where: { $0.name == name }) {
            throw IshVMError.alreadyExists(name)
        }
        // Use a tmp dir + rename so a partial clone doesn't leave a half-VM.
        let script = """
        set -e
        TMP="/srv/vms/.\(name).new"
        rm -rf "$TMP"
        mkdir -p "$TMP"
        cp -a /srv/vms/.template/. "$TMP/"
        # Customize hostname so the prompt shows the VM name.
        echo "\(name)" > "$TMP/etc/hostname"
        mv "$TMP" "/srv/vms/\(name)"
        """
        let r = try runOneshot(.init(argv: ["/bin/sh", "-c", script]))
        guard r.exitCode == 0 else {
            let err = String(decoding: r.stderrData, as: UTF8.self)
            throw IshVMError.createFailed(err.isEmpty
                ? "exit \(r.exitCode)" : err)
        }
        return IshVM(name: name)
    }

    /// Permanently delete a VM and everything inside it.
    func deleteVM(name: String) throws {
        try validateVMName(name)
        guard try listVMs().contains(where: { $0.name == name }) else {
            throw IshVMError.notFound(name)
        }
        let r = try runOneshot(.init(
            argv: ["/bin/sh", "-c", "rm -rf /srv/vms/\(name)"]
        ))
        guard r.exitCode == 0 else {
            throw IshVMError.createFailed("delete failed: exit \(r.exitCode)")
        }
    }

    /// Make sure at least one VM exists; create "default" if the rootfs
    /// is brand-new. Call once after boot.
    @discardableResult
    func ensureDefaultVM() throws -> IshVM {
        let existing = try listVMs()
        if let first = existing.first { return first }
        return try createVM(name: "default")
    }

    private func validateVMName(_ name: String) throws {
        // Conservative: alphanumerics + dash + underscore, 1..32 chars,
        // no leading dot (we use those for templates / staging).
        guard !name.isEmpty, name.count <= 32, !name.hasPrefix(".") else {
            throw IshVMError.invalidName(name)
        }
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_"))
        guard name.unicodeScalars.allSatisfy({ allowed.contains($0) }) else {
            throw IshVMError.invalidName(name)
        }
    }
}
