#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(command):
    completed = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return completed.stdout.strip()


def tool_record(name, version_arguments=None):
    executable = shutil.which(name)
    if executable is None:
        raise SystemExit(f"required build environment tool is missing: {name}")
    resolved = os.path.realpath(executable)
    info = os.stat(resolved)
    if not os.path.isfile(resolved):
        raise SystemExit(f"build environment tool is not a regular file: {name}")
    record = {
        "name": name,
        "resolvedPath": resolved,
        "byteCount": info.st_size,
        "sha256": sha256(resolved),
    }
    if version_arguments is not None:
        record["versionCommand"] = [name, *version_arguments]
        record["versionOutput"] = command_output([name, *version_arguments])
    return record


def linked_library_record(load_path, *, compatibility=None, current=None):
    resolved = os.path.realpath(load_path)
    present = os.path.isfile(resolved)
    record = {
        "loadPath": load_path,
        "resolvedPath": resolved if present else None,
        "filePresent": present,
        "byteCount": os.stat(resolved).st_size if present else None,
        "sha256": sha256(resolved) if present else None,
    }
    if compatibility is not None:
        record["compatibilityVersion"] = compatibility
    if current is not None:
        record["currentVersion"] = current
    if not present and platform.system() == "Darwin" and load_path.startswith("/usr/lib/"):
        record["storage"] = "dyld-shared-cache"
    return record


def linked_libraries(binary):
    system = platform.system()
    if system == "Darwin":
        output = command_output(["otool", "-L", str(binary)])
        records = []
        for line in output.splitlines()[1:]:
            match = re.fullmatch(
                r"\s*(?P<path>.+?) \(compatibility version "
                r"(?P<compatibility>[^,]+), current version "
                r"(?P<current>[^)]+)\)",
                line,
            )
            if match is None:
                raise SystemExit(f"could not parse linked library record: {line}")
            records.append(
                linked_library_record(
                    match.group("path"),
                    compatibility=match.group("compatibility"),
                    current=match.group("current"),
                )
            )
        return {
            "evidenceCommand": ["otool", "-L", str(binary)],
            "records": records,
        }
    if system == "Linux":
        output = command_output(["ldd", str(binary)])
        records = []
        for line in output.splitlines():
            match = re.fullmatch(
                r"\s*(?:\S+ => )?(?P<path>/\S+)(?: \(0x[0-9a-fA-F]+\))?",
                line,
            )
            if match is not None:
                records.append(linked_library_record(match.group("path")))
        if not records:
            raise SystemExit("ldd did not report any linked library paths")
        return {
            "evidenceCommand": ["ldd", str(binary)],
            "records": records,
        }
    raise SystemExit(f"unsupported host for linked-library evidence: {system}")


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--repository-revision", required=True)
    parser.add_argument("--ish-revision", required=True)
    parser.add_argument("--rootfs-pin-sha256", required=True)
    parser.add_argument("--candidate-script-sha256", required=True)
    parser.add_argument("--capture-script-sha256", required=True)
    parser.add_argument("--fakefsify", required=True)
    parser.add_argument("--fakefsify-provenance-sha256", required=True)
    return parser.parse_args()


def require_digest(value, label):
    if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise SystemExit(f"{label} must be a lowercase SHA-256 digest")


def main():
    arguments = parse_arguments()
    for value, label in (
        (arguments.rootfs_pin_sha256, "rootfs pin"),
        (arguments.candidate_script_sha256, "candidate script"),
        (arguments.capture_script_sha256, "capture script"),
        (arguments.fakefsify_provenance_sha256, "fakefsify provenance"),
    ):
        require_digest(value, label)

    fakefsify = pathlib.Path(arguments.fakefsify).resolve(strict=True)
    if not fakefsify.is_file():
        raise SystemExit("fakefsify must be a regular file")

    system = platform.system()
    tools = [
        tool_record("bash", ["--version"]),
        tool_record("cc", ["--version"]),
        tool_record("curl", ["--version"]),
        tool_record("git", ["--version"]),
        tool_record("make", ["--version"]),
        tool_record("meson", ["--version"]),
        tool_record("ninja", ["--version"]),
        tool_record("pkg-config", ["--version"]),
        tool_record("python3", ["--version"]),
        tool_record("shasum", ["--version"]),
        tool_record("tar", ["--version"]),
        tool_record("zig", ["version"]),
    ]
    # Apple otool versions differ in whether --version is accepted. Its
    # resolved binary digest still identifies the exact inspector without
    # making capture depend on that option.
    if system == "Darwin":
        tools.append(tool_record("otool"))
    else:
        tools.append(tool_record("ldd", ["--version"]))
    libraries = linked_libraries(fakefsify)

    document = {
        "schemaVersion": 2,
        "status": "captured-unapproved-candidate-build-environment",
        "source": {
            "repository": "https://github.com/jacklv-coder/ish-arm64-pkg",
            "revision": arguments.repository_revision,
            "ishRevision": arguments.ish_revision,
            "rootfsPinSHA256": arguments.rootfs_pin_sha256,
            "candidateScriptSHA256": arguments.candidate_script_sha256,
            "captureScriptSHA256": arguments.capture_script_sha256,
        },
        "host": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "platform": platform.platform(),
        },
        "environment": {
            "ambientValuesCaptured": False,
            "sanitizationPolicy": "candidate-entrypoint-minimal-allowlist",
            "preservedVariableNames": [
                "TMPDIR",
                "HTTP_PROXY",
                "HTTPS_PROXY",
                "ALL_PROXY",
                "NO_PROXY",
                "http_proxy",
                "https_proxy",
                "all_proxy",
                "no_proxy",
                "CURL_CA_BUNDLE",
                "SSL_CERT_FILE",
                "SSL_CERT_DIR",
            ],
            "pathPolicy": "host-architecture-trusted-system-and-package-prefixes",
            "effectivePATH": os.environ["PATH"],
            "transportValuesCaptured": False,
            "selectionEvidence": "resolved tool paths and executable digests",
        },
        "hostTool": {
            "name": "fakefsify",
            "byteCount": fakefsify.stat().st_size,
            "sha256": sha256(fakefsify),
            "provenanceSHA256": arguments.fakefsify_provenance_sha256,
        },
        "tools": tools,
        "linkedLibraries": libraries,
        "scope": {
            "sameEnvironmentDoubleBuildComparedByteForByte": True,
            "crossEnvironmentReproducibilityVerified": False,
            "distributionAuthorized": False,
        },
    }
    output = pathlib.Path(arguments.output)
    output.write_text(
        json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
