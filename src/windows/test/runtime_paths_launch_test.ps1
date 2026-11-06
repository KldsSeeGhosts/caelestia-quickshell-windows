[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BackendExe,
    [Parameter(Mandatory)][string]$QtPrefix,
    [Parameter(Mandatory)][string]$EvidenceRoot,
    [ValidateSet('base-file', 'base-denied', 'instance-file', 'shell-file', 'pid-file', 'path-file', 'valid')]
    [string[]]$Scenarios = @('base-file', 'base-denied', 'instance-file', 'shell-file', 'pid-file', 'path-file', 'valid'),
    [ValidateRange(1, 60000)][int]$LaunchTimeoutMs = 15000
)
# Real qs.exe launch acceptance, separate from the deterministic paths.cpp fixture.
# Never installs or launches the user's shell. Only a windowless synthetic QML
# config is loaded, on offscreen/software Qt with native providers disabled.
$ErrorActionPreference = 'Stop'
$BackendExe = (Resolve-Path -LiteralPath $BackendExe).Path
$QtPrefix = (Resolve-Path -LiteralPath $QtPrefix).Path
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)
if (Test-Path -LiteralPath $EvidenceRoot) { throw "EvidenceRoot must not exist: $EvidenceRoot" }
New-Item -ItemType Directory -Path $EvidenceRoot | Out-Null
$cases = @($Scenarios | Select-Object -Unique)
if (-not $cases.Count) { throw 'At least one launch scenario must be selected.' }
$receipts = @()

function Get-RuleFingerprint($Acl) {
    (@($Acl.Access | ForEach-Object {
        "$($_.IdentityReference)|$($_.FileSystemRights)|$($_.InheritanceFlags)|$($_.PropagationFlags)|$($_.AccessControlType)|$($_.IsInherited)"
    } | Sort-Object) -join "`n")
}

function Protect-FixtureEvidence([string]$Text) {
    $mappings = @(
        @{ value = $BackendExe; label = '<backend-executable>' },
        @{ value = $EvidenceRoot; label = '<fixture>' },
        @{ value = $QtPrefix; label = '<qt>' },
        @{ value = $env:USERPROFILE; label = '<profile>' }
    ) | Sort-Object { $_.value.Length } -Descending
    foreach ($mapping in $mappings) {
        if (-not $mapping.value) { continue }
        foreach ($spelling in @($mapping.value, $mapping.value.Replace('\', '/'))) {
            $Text = [regex]::Replace($Text, [regex]::Escape($spelling), $mapping.label, 'IgnoreCase')
        }
    }
    return [regex]::Replace($Text, 'S-1-(?:[0-9]+-)*[0-9]+', '<sid>')
}

foreach ($scenario in $cases) {
    $caseRoot = Join-Path $EvidenceRoot $scenario
    $working = Join-Path $caseRoot 'working'
    $runtime = Join-Path $caseRoot 'runtime'
    $originalAcl = $null
    $process = $null
    $started = $false
    $pipe = $null
    $stdout = $null
    $stderr = $null
    $receipt = [ordered]@{ scenario = $scenario; evidenceClass = 'actual-offscreen-launch'; passed = $false }
    try {
        New-Item -ItemType Directory -Path $working | Out-Null
        foreach ($name in @('data', 'state', 'cache', 'temp', 'appdata', 'localappdata')) {
            New-Item -ItemType Directory -Path (Join-Path $caseRoot $name) | Out-Null
        }
        $qml = @"
//@ pragma ShellId runtime-path-launch
//@ pragma DataDir $($caseRoot.Replace('\', '/'))/data
//@ pragma StateDir $($caseRoot.Replace('\', '/'))/state
//@ pragma CacheDir $($caseRoot.Replace('\', '/'))/cache
import QtQuick
import Quickshell
ShellRoot {
    Timer {
        interval: 3000
        running: true
        onTriggered: {
            console.log("RUNTIME_PATH_CONTROL_READY")
            Qt.quit()
        }
    }
}
"@
        $qmlPath = Join-Path $caseRoot 'shell.qml'
        [IO.File]::WriteAllText($qmlPath, $qml)
        if ($scenario -eq 'base-file') {
            [IO.File]::WriteAllText($runtime, 'regular-file blocker, not ACL denial')
            $receipt.failureKind = 'regular-file'
        } else {
            New-Item -ItemType Directory -Path $runtime | Out-Null
            if ($scenario -eq 'base-denied') {
                # Deny creation of immediate child directories only, on this
                # newly owned empty fixture. Keep read/ACL-restore rights.
                $originalAcl = Get-Acl -LiteralPath $runtime
                $deniedAcl = Get-Acl -LiteralPath $runtime
                $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
                $rule = [Security.AccessControl.FileSystemAccessRule]::new(
                    $sid, [Security.AccessControl.FileSystemRights]::CreateDirectories,
                    [Security.AccessControl.AccessControlType]::Deny)
                $deniedAcl.AddAccessRule($rule)
                Set-Acl -LiteralPath $runtime -AclObject $deniedAcl
                $receipt.failureKind = 'ACL-denied-creation'
                $receipt.deniedRule = 'Deny CreateDirectories (current user, fixture directory only)'
                try {
                    [IO.Directory]::CreateDirectory((Join-Path $runtime 'denial-probe')) | Out-Null
                    throw 'ACL denial was ineffective'
                } catch [System.UnauthorizedAccessException] {
                    $receipt.denialProbe = 'UnauthorizedAccessException'
                }
            } elseif ($scenario -ne 'valid') {
                New-Item -ItemType Directory -Path (Join-Path $runtime 'run') | Out-Null
                $blocked = switch ($scenario) {
                    'instance-file' { 'by-id' }
                    'shell-file' { 'by-shell' }
                    'pid-file' { 'by-pid' }
                    'path-file' { 'by-path' }
                }
                [IO.File]::WriteAllText((Join-Path $runtime "run/$blocked"), 'regular-file blocker')
                $receipt.failureKind = 'regular-file'
            }
        }

        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = $BackendExe
        $info.Arguments = '--no-color --path "' + $qmlPath + '"'
        $info.WorkingDirectory = $working
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        foreach ($key in @($info.EnvironmentVariables.Keys)) {
            if ($key -like 'QS_*' -or $key -like 'CAELESTIA_*' -or
                $key -like 'QML*' -or $key -like 'QT_*' -or $key -like 'QSG_*') {
                $info.EnvironmentVariables.Remove($key)
            }
        }
        $values = @{
            PATH = "$QtPrefix/bin;$env:SystemRoot/System32;$env:SystemRoot"
            QT_QPA_PLATFORM = 'offscreen'
            QT_QUICK_BACKEND = 'software'
            QT_FORCE_STDERR_LOGGING = '1'
            QT_LOGGING_RULES = 'quickshell.paths.debug=true;quickshell.ipc.info=true'
            QML_DISABLE_DISK_CACHE = '1'
            QS_RUNTIME_DIR = $runtime
            QS_DISABLE_CRASH_HANDLER = '1'
            QS_WINDOWS_TRAY_HOOK = '0'
            QS_WINDOWS_BLUR = '0'
            APPDATA = (Join-Path $caseRoot 'appdata')
            LOCALAPPDATA = (Join-Path $caseRoot 'localappdata')
            TEMP = (Join-Path $caseRoot 'temp')
            TMP = (Join-Path $caseRoot 'temp')
            CAELESTIA_WINDOWS_TILING = 'off'
            CAELESTIA_SHELL_LIVE = '0'
            CAELESTIA_SHELL_SESSION = '0'
        }
        foreach ($provider in @('AUDIO', 'AUDIO_CONTROLS', 'BATTERY', 'POWER_PROFILES',
            'POWER_PROFILE_WRITES', 'MEDIA', 'MEDIA_CONTROLS', 'BRIGHTNESS',
            'BRIGHTNESS_CONTROLS', 'BRIGHTNESS_SOFTWARE', 'NETWORK', 'NETWORK_CONTROLS',
            'HYPR', 'HYPR_DISPATCH', 'BLUETOOTH', 'BLUETOOTH_CONTROLS', 'APPS',
            'APP_LAUNCH', 'TRAY', 'TRAY_ACTIVATION', 'SESSION', 'SESSION_ACTIONS',
            'NOTIFICATIONS', 'NOTIFICATION_MIRROR', 'NOTIFICATION_MIRROR_DISMISS',
            'NOTIFICATION_APP_OPEN', 'BACKGROUND')) {
            $values["CAELESTIA_WINDOWS_$provider"] = '0'
        }
        foreach ($key in $values.Keys) { $info.EnvironmentVariables[$key] = $values[$key] }
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $info
        if (-not $process.Start()) { throw 'qs.exe did not start' }
        $started = $true
        $receipt.pid = $process.Id
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if ($scenario -eq 'valid') {
            # Verify a real IPC endpoint during the positive control's lifetime.
            # This probes only this child's runtime directory and named pipe.
            $deadline = [DateTime]::UtcNow.AddSeconds(10)
            do {
                $ids = @(Get-ChildItem -LiteralPath (Join-Path $runtime 'run/by-id') -Directory -ErrorAction SilentlyContinue)
                if ($ids.Count -eq 1) {
                    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', "quickshell-$($ids[0].Name)", [IO.Pipes.PipeDirection]::InOut)
                    try { $pipe.Connect(100) } catch [TimeoutException] { $pipe.Dispose(); $pipe = $null }
                    if ($pipe -and $pipe.IsConnected) { break }
                }
                if ($process.HasExited) { break }
                Start-Sleep -Milliseconds 25
            } while ([DateTime]::UtcNow -lt $deadline)
            $receipt.ipcEndpointConnected = ($null -ne $pipe -and $pipe.IsConnected)
            if ($pipe) { $pipe.Dispose(); $pipe = $null }
        }
        if (-not $process.WaitForExit($LaunchTimeoutMs)) {
            $receipt.timedOut = $true
            throw 'qs.exe timed out'
        }
        $text = $stdout.Result + $stderr.Result
        $receipt.exitCode = $process.ExitCode
        $receipt.relativeArtifacts = @(Get-ChildItem -LiteralPath $working -Force | Select-Object -ExpandProperty Name)
        $receipt.runtimeFiles = @(if (Test-Path -LiteralPath $runtime -PathType Container) {
            Get-ChildItem -LiteralPath $runtime -Recurse -File | ForEach-Object { $_.FullName.Substring($runtime.Length + 1) }
        })
        if ($receipt.relativeArtifacts.Count) { throw 'Unexpected working-directory artifact' }
        if ($scenario -eq 'valid') {
            if ($process.ExitCode -ne 0 -or $text -notmatch 'RUNTIME_PATH_CONTROL_READY' -or
                $text -notmatch 'Configuration Loaded' -or -not $receipt.ipcEndpointConnected) {
                throw 'Valid launch failed its QML/exit/IPC control'
            }
            foreach ($leaf in @('instance.lock', 'log.log', 'log.qslog')) {
                if (-not @($receipt.runtimeFiles | Where-Object { $_ -like "*\$leaf" }).Count) {
                    throw "Valid control missing $leaf"
                }
            }
        } else {
            if ($process.ExitCode -ne -1 -or
                $text -notmatch 'Cannot launch: required runtime bookkeeping could not be created' -or
                $text -notmatch 'Could not create' -or
                $text -match 'RUNTIME_PATH_CONTROL_READY|Configuration Loaded|Started IPC server') {
                throw 'Failure was not a controlled pre-QML/pre-IPC startup refusal'
            }
            if (@($receipt.runtimeFiles | Where-Object { $_ -match 'instance\.lock$|log\.(log|qslog)$' }).Count) {
                throw 'Failure published a lock or runtime log'
            }
        }
        $receipt.passed = $true
    } catch {
        $receipt.error = Protect-FixtureEvidence $_.Exception.Message
    } finally {
        if ($pipe) { $pipe.Dispose() }
        if ($process) {
            if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            # Drain and preserve output even after a timeout or failed assertion.
            # Successful and failed logs are scrubbed before being written.
            if ($started -and $stdout -and $stderr) {
                [IO.File]::WriteAllText((Join-Path $EvidenceRoot "$scenario.log"),
                    (Protect-FixtureEvidence ($stdout.Result + $stderr.Result)))
            }
            $process.Dispose()
        }
        if ($originalAcl) {
            Set-Acl -LiteralPath $runtime -AclObject $originalAcl
            # Windows may normalize the DACL auto-inheritance control bit on
            # Set-Acl. Compare effective rules, protection, owner and group,
            # not the byte spelling of the SDDL control flags.
            $restored = Get-Acl -LiteralPath $runtime
            $receipt.aclRestored = ((Get-RuleFingerprint $restored) -eq (Get-RuleFingerprint $originalAcl) -and
                $restored.AreAccessRulesProtected -eq $originalAcl.AreAccessRulesProtected -and
                $restored.Owner -eq $originalAcl.Owner -and $restored.Group -eq $originalAcl.Group)
            if (-not $receipt.aclRestored) { throw 'Fixture ACL restoration failed' }
            [IO.Directory]::CreateDirectory((Join-Path $runtime 'restoration-probe')) | Out-Null
        }
        # Keep failed synthetic profiles locally for diagnosis, after restoring
        # ACLs. They can contain absolute QML paths: never upload this folder.
        $resolvedCase = [IO.Path]::GetFullPath($caseRoot)
        if ([IO.Path]::GetDirectoryName($resolvedCase) -ne $EvidenceRoot) { throw 'Unsafe cleanup target' }
        if ($receipt.passed) { Remove-Item -LiteralPath $resolvedCase -Recurse -Force }
        else { $receipt.privateFailureFixturePreserved = $true }
    }
    $receipts += [pscustomobject]$receipt
}
$result = [ordered]@{
    backendFile = [IO.Path]::GetFileName($BackendExe)
    backendSha256 = (Get-FileHash -LiteralPath $BackendExe -Algorithm SHA256).Hash
    qtVersion = (& (Join-Path $QtPrefix 'bin/qtpaths.exe') --qt-version | Select-Object -First 1)
    platform = [Environment]::OSVersion.VersionString
    cases = $receipts
    passed = @($receipts | Where-Object { $_.passed }).Count
    failed = @($receipts | Where-Object { -not $_.passed }).Count
}
$result | ConvertTo-Json -Depth 8 | Out-File -LiteralPath (Join-Path $EvidenceRoot 'receipt.json') -Encoding utf8
$receipts | Format-Table scenario, passed, exitCode, error -AutoSize
if ($result.failed) { exit 1 }
