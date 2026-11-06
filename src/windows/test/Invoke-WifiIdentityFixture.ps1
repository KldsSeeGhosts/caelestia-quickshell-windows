[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$QtPrefix,
    [Parameter(Mandatory)][string]$LlvmBin,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(1, 2)][int]$BuildJobs = 2
)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$compiler = Join-Path $LlvmBin 'clang-cl.exe'
$moc = Join-Path $QtPrefix 'bin\moc.exe'
foreach ($tool in @($compiler, $moc)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required fixture tool missing: $tool" }
}
if (-not $env:VCToolsInstallDir) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'An installed x64 Visual Studio C++ toolset is required.' }
    $env:PATH = "$([IO.Path]::GetDirectoryName($vswhere));$env:PATH"
    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64 -vcvars_ver=14.44'
}
$env:PATH = "$QtPrefix\bin;$LlvmBin;$env:PATH"
$includes = @(
    $output, (Join-Path $sourceRoot 'windows'),
    (Join-Path $QtPrefix 'include'),
    (Join-Path $QtPrefix 'include\QtCore'),
    (Join-Path $QtPrefix 'include\QtQml'),
    (Join-Path $QtPrefix 'include\QtQmlIntegration'),
    (Join-Path $QtPrefix 'include\QtTest')
) | ForEach-Object { "-I$_" }
$definitions = @('-DQS_TEST', '-DQS_WIFI_NATIVE_FIXTURE', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN')
$common = @('/nologo', '/std:c++20', '/EHsc', '/MD', '/utf-8', '/O1', '/c') + $definitions + $includes
$headers = @(
    'windows\system\network.hpp', 'windows\system\network_wifi.hpp',
    'windows\system\network_wifi_item.hpp', 'windows\system\network_connectivity.hpp', 'core\model.hpp'
)
$generated = @()
foreach ($header in $headers) {
    $name = [IO.Path]::GetFileNameWithoutExtension($header)
    $destination = Join-Path $output "moc_$name.cpp"
    & $moc @definitions @includes (Join-Path $sourceRoot $header) -o $destination
    if ($LASTEXITCODE) { throw "moc failed: $header" }
    $generated += $destination
}
$testNames = @('network_wifi_identity_test', 'network_wifi_route_test')
foreach ($name in $testNames) {
    & $moc @definitions @includes (Join-Path $PSScriptRoot "$name.cpp") -o (Join-Path $output "$name.moc")
    if ($LASTEXITCODE) { throw "moc failed: $name" }
}
$shared = @(
    'windows\system\network.cpp', 'windows\system\network_wifi.cpp',
    'windows\system\network_wifi_item.cpp', 'core\model.cpp',
    'windows\test\network_wifi_stubs.cpp', 'windows\test\network_wifi_native_stubs.cpp'
) | ForEach-Object { Join-Path $sourceRoot $_ }
$sources = @($shared) + $generated + @($testNames | ForEach-Object { Join-Path $PSScriptRoot "$_.cpp" })
$pool = [RunspaceFactory]::CreateRunspacePool(1, $BuildJobs)
$pool.Open()
$work = @()
try {
    foreach ($source in $sources) {
        $object = Join-Path $output "$([IO.Path]::GetFileNameWithoutExtension($source)).obj"
        $arguments = $common + @("/Fo$object", $source)
        $pipeline = [PowerShell]::Create()
        $pipeline.RunspacePool = $pool
        [void]$pipeline.AddScript({
            param($exe, $arguments)
            $lines = & $exe @arguments 2>&1 | Out-String
            [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $lines }
        }).AddArgument($compiler).AddArgument($arguments)
        $work += [pscustomobject]@{ Pipeline = $pipeline; Handle = $pipeline.BeginInvoke(); Source = $source }
    }
    $failed = $false
    foreach ($entry in $work) {
        $result = $entry.Pipeline.EndInvoke($entry.Handle)
        $entry.Pipeline.Dispose()
        foreach ($part in $result) {
            if ($part.Output) { Write-Output $part.Output }
            if ($part.ExitCode -ne 0) { $failed = $true }
        }
        if ($result.Count -eq 0) { $failed = $true }
    }
    if ($failed) { throw 'Native fixture compilation failed.' }
} finally {
    $pool.Dispose()
}
$sharedObjects = @($shared + $generated) | ForEach-Object {
    Join-Path $output "$([IO.Path]::GetFileNameWithoutExtension($_)).obj"
}
$linker = Join-Path $LlvmBin 'lld-link.exe'
foreach ($name in $testNames) {
    $executable = Join-Path $output "$name.exe"
    & $linker /nologo "/out:$executable" /subsystem:console /manifest:no @sharedObjects `
        (Join-Path $output "$name.obj") "/libpath:$QtPrefix\lib" `
        Qt6Core.lib Qt6Qml.lib Qt6Test.lib shell32.lib user32.lib kernel32.lib
    if ($LASTEXITCODE) { throw "Fixture link failed: $name" }
    # Deliberately no wlanapi.lib: every WLAN symbol must resolve to our fake.
    $xmlResult = "$(Join-Path $output "$name.xml"),junitxml"
    $textResult = "$(Join-Path $output "$name.txt"),txt"
    & $executable -o $xmlResult -o $textResult
    $testExit = $LASTEXITCODE
    Get-Content -LiteralPath (Join-Path $output "$name.txt")
    if ($testExit) { throw "Fixture failed: $name (exit $testExit)" }
    [xml]$receipt = Get-Content -Raw -LiteralPath (Join-Path $output "$name.xml")
    if ([int]$receipt.testsuite.tests -eq 0 -or [int]$receipt.testsuite.failures -ne 0) {
        throw "Empty or failed fixture receipt: $name"
    }
}
