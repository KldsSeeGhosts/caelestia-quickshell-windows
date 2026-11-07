# Windows fixture toolchain

The producer workflow consumes the shell repository's canonical public
`windows/toolchain.json`, Python locks and shared PowerShell helpers at
`327ef65455a2d8de863e4c2bd106b0b2a781e3dc`. There is no second compiler,
Rust or runtime policy in this repository. Updating that contract source is
a deliberate reviewed toolchain upgrade, not a floating branch checkout.

That exact commit must be anonymously fetchable in
`KldsSeeGhosts/caelestia-windows` before this workflow can pass. A missing source
commit is a publication prerequisite, not permission to add private credentials
or select a different toolchain. Hosted acceptance must be recorded after
publication; a prepared workflow is not evidence that its job ran.

The `windows-2022` label alone does not pin installed software. Before compiling,
the shared helper selects VC tools 14.44.35207 and Windows SDK 10.0.26100.0
explicitly, binds the x64 linker, and asserts actual clang-cl, Rust/target, Qt,
CMake, Ninja and CLI11 versions. Preparation verifies committed public download
hashes, including the Python tools, Qt archives and Rust release manifest.
Serviced compiler/linker versions and the official Rust commit are exact
requirements. The VS redist marker must select CRT DLLs whose actual versions
match the qualified `vcRedistVersion` and do not exceed the runtime floor.
An extracted tooling cache is trusted only through the existing trusted-main
writer boundary; archive checksums do not reauthenticate every cached file.
Qt's checked supplier SBOM build-tool versions are not a declared minimum CRT
requirement or a clean-VM qualification.

Public receipts contain only version/hash fields, backend and contract-source
revisions, and the separate VS installation version. They exclude raw paths,
environment reports and build logs. The portable fixture selection and native
test project are unchanged. Fixture passes do not establish a package/runtime,
interactive Windows desktop or hardware qualification.

Capture source can move within the shell repository without introducing another
toolchain policy: the specification and helpers do not select its source path.
