[CmdletBinding()]
param([Parameter(Mandatory)][string]$ToolRoot)
$ErrorActionPreference = 'Stop'

# This optional fixture accelerator is not part of the production toolchain.
$version = '0.15.0'
$sha256 = 'b0b257a164bf438b2dea134ca7ded41c100f59a64b3bf275a202f1e8102ab217'
$name = "sccache-v$version-x86_64-pc-windows-msvc"
$archive = Join-Path $ToolRoot "downloads/$name.tar.gz"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $archive) | Out-Null
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    curl.exe -L --fail --silent --show-error --connect-timeout 30 --max-time 180 `
        "https://github.com/mozilla/sccache/releases/download/v$version/$name.tar.gz" -o $archive
    if ($LASTEXITCODE -ne 0) { throw 'sccache download failed.' }
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ine $sha256) {
    throw 'sccache checksum mismatch.'
}
# Extract only after checking the pinned public release archive, even on a hit.
& "$env:SystemRoot/System32/tar.exe" -xf $archive -C $ToolRoot
if ($LASTEXITCODE -ne 0) { throw 'sccache extraction failed.' }
$launcher = Join-Path $ToolRoot "$name/sccache.exe"
$actual = & $launcher --version
if ($LASTEXITCODE -ne 0 -or "$actual".Trim() -cne "sccache $version") {
    throw 'sccache version mismatch.'
}
if ($env:GITHUB_ENV) {
    "QS_FIXTURE_COMPILER_LAUNCHER=$launcher" | Out-File -LiteralPath $env:GITHUB_ENV -Encoding utf8 -Append
}
Write-Output $launcher
