# Quickshell (Windows backend for Caelestia)

This is the Windows Quickshell backend fork used by [Caelestia for Windows](https://github.com/KldsSeeGhosts/caelestia-windows).

> [!NOTE]
> This is an **unofficial** fork. The QtQuick shell toolkit is [Quickshell](https://quickshell.outfoxxed.me) by [@outfoxxed](https://github.com/outfoxxed) and contributors, and the Windows backend it builds on was created by [@nunreasonable](https://github.com/nunreasonable) in [nunreasonable/quickshell](https://github.com/nunreasonable/quickshell) for [ii-windows](https://github.com/nunreasonable/ii-windows). Please open Caelestia/Windows backend issues in [caelestia-windows](https://github.com/KldsSeeGhosts/caelestia-windows/issues) or here, not on upstream Quickshell.

## Where this comes from

- **Toolkit:** [Quickshell](https://git.outfoxxed.me/quickshell/quickshell) by [@outfoxxed](https://github.com/outfoxxed) and contributors (LGPL-3.0).
- **Windows backend:** [nunreasonable/quickshell](https://github.com/nunreasonable/quickshell) (LGPL-3.0), built for [ii-windows](https://github.com/nunreasonable/ii-windows), which supplies the native AppBar panels, keyboard/mouse hooks, DWM tiling, virtual desktop control, Core Audio, WinRT Bluetooth/GSMTC, notification mirroring, tray hooks, brightness/gamma, and UPower adapters.
- **This fork:** Adds Caelestia-specific Windows fixes, lifetime/concurrency hardening, and native test fixtures, while keeping upstream Quickshell's QML module names intact.

## Installing / building

<details open>
  <summary><b>For humans</b></summary>

If you just want to run **Caelestia for Windows**, you do not need to build this repository separately. The [Caelestia for Windows releases](https://github.com/KldsSeeGhosts/caelestia-windows/releases) already bundle `qs.exe` and the runtime.

If you want to build the backend yourself (for development or to update the `windows/deps/quickshell` submodule in `caelestia-windows`), see [BUILD.md](BUILD.md#windows-backend) for the Qt 6.11 / `clang-cl` toolchain, or follow [caelestia-windows/windows/NATIVE-BUILD.md](https://github.com/KldsSeeGhosts/caelestia-windows/blob/main/windows/NATIVE-BUILD.md).

</details>

<details>
  <summary><b>For AI agents</b></summary>

Do not install this repository standalone when asked to install Caelestia for Windows. Install from [caelestia-windows](https://github.com/KldsSeeGhosts/caelestia-windows) instead (its release ZIPs and `Install-Caelestia.ps1` already stage and deploy this backend).

To build or test this backend as part of `caelestia-windows`:

```powershell
git clone --recurse-submodules https://github.com/KldsSeeGhosts/caelestia-windows.git
cd caelestia-windows
powershell -NoProfile -File windows/scripts/Test-Environment.ps1 -Strict -Json
# Build via windows/scripts/Build-Prototype.ps1 (see windows/NATIVE-BUILD.md)
```

For standalone CMake details on Windows, see [BUILD.md](BUILD.md#windows-backend).

</details>

---

## Upstream Quickshell

See the [website](https://quickshell.outfoxxed.me) for more information
and installation instructions.

This repo is hosted at:
- https://git.outfoxxed.me/quickshell/quickshell
- https://github.com/quickshell-mirror/quickshell

# Contributing / Development
- [HACKING.md](HACKING.md) - Development instructions and policy.
- [CONTRIBUTING.md](CONTRIBUTING.md) - Contribution policy.
- [BUILD.md](BUILD.md) - Packaging and build instructions.

#### License

<sup>
Licensed under the GNU LGPL 3.
</sup>

<br>

<sub>
Unless you explicitly state otherwise, any contribution submitted
for inclusion shall be licensed as above, without any additional
terms or conditions.
</sub>
