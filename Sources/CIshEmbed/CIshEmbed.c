/* Empty translation unit so the SwiftPM target produces a real
 * object file. The C ABI itself is implemented inside the prebuilt
 * libIshKernel.xcframework; this target only re-exports its header
 * to Swift via the modulemap in include/. */
