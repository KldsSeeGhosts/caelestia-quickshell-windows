[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Configure', 'Build', 'Portable')][string]$Phase,
    [Parameter(Mandatory)][string]$ContractRoot,
    [Parameter(Mandatory)][string]$ToolRoot,
    [Parameter(Mandatory)][string]$BuildRoot,
    [Parameter(Mandatory)][string]$Results,
    [ValidateRange(1, 16)][int]$Jobs = 2
)
$ErrorActionPreference = 'Stop'
. (Join-Path $ContractRoot 'windows/scripts/Caelestia-Toolchain.ps1')
$spec = Get-CaelestiaToolchain
$qt = Join-Path $ToolRoot $spec.qtDirectory
$llvm = Join-Path $ToolRoot "$($spec.llvmDirectory)/bin"

if ($Phase -in @('Configure', 'Build')) {
    # Keep selection and actual identity assertions in the canonical entry point.
    # The outer recorded configure stage includes this setup, not just cmake.
    $instance = Enter-CaelestiaToolchain -QtPrefix $qt -LlvmBin $llvm -ToolsBin "$ToolRoot/tools/Scripts"
    Select-CaelestiaRust
    if ($Phase -eq 'Build') {
        cmake --build $BuildRoot --parallel $Jobs
        exit $LASTEXITCODE
    }
    $actual = Get-CaelestiaActualToolchain -QtPrefix $qt -Cli11Include (Join-Path $ToolRoot $spec.cli11Directory)
    $actual | Add-Member -NotePropertyName vsInstallationVersion -NotePropertyValue $instance.installationVersion
    $actual | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$Results/actual-toolchain.json" -Encoding UTF8
    $mt = Join-Path $env:WindowsSdkDir "bin/$($spec.windowsSdkVersion)/x64/mt.exe"
    # Configuration is separate so compiler failures have a distinct stage.
    cmake -S src/windows/test -B $BuildRoot -G Ninja "-DCMAKE_PREFIX_PATH=$qt" `
        "-DCMAKE_CXX_COMPILER:FILEPATH=$llvm/clang-cl.exe" "-DCMAKE_MT:FILEPATH=$mt" `
        "-DCMAKE_LINKER:FILEPATH=$env:VCToolsInstallDir/bin/Hostx64/x64/link.exe" -DCMAKE_BUILD_TYPE=Release
    exit $LASTEXITCODE
}

$env:PATH = "$ToolRoot/tools/Scripts;$qt/bin;$env:PATH"
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$qt/plugins/platforms"
$env:QT_QUICK_BACKEND = 'software'
$temporaryRoot = Join-Path $Results 'temp'
New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
$env:TEMP = $temporaryRoot
$env:TMP = $temporaryRoot
# The parent recorder removes this raw file after producing allowlisted XML.
ctest --test-dir $BuildRoot -L '^portable$' --no-tests=error --output-on-failure `
    --output-junit "$Results/portable.raw.xml"
exit $LASTEXITCODE
