[CmdletBinding()]
param(
    # Overrides exist for regression tests; CI measures the actual runner.
    [int]$LogicalProcessors = [Environment]::ProcessorCount,
    [double]$MemoryBytes,
    [string]$EnvironmentFile = $env:GITHUB_ENV
)
$ErrorActionPreference = 'Stop'
if (-not $PSBoundParameters.ContainsKey('MemoryBytes')) {
    $MemoryBytes = [double](Get-CimInstance -ClassName Win32_ComputerSystem).TotalPhysicalMemory
}
# One job per logical processor, bounded by 3 GiB RAM per job and the fixture
# gate's accepted range (1-16). This does not parallelize tests.
if ($LogicalProcessors -lt 1 -or $MemoryBytes -le 0 -or
    [double]::IsNaN($MemoryBytes) -or [double]::IsInfinity($MemoryBytes)) {
    throw 'Cannot measure runner processors and memory; refusing to guess build parallelism.'
}
$byMemory = [int][Math]::Min(16.0, [Math]::Floor($MemoryBytes / 3GB))
$jobs = [int][Math]::Max(1, [Math]::Min(16, [Math]::Min($LogicalProcessors, $byMemory)))
if ($EnvironmentFile) { "BUILD_JOBS=$jobs" | Add-Content -LiteralPath $EnvironmentFile -Encoding UTF8 }
Write-Output ("Build parallelism: {0} jobs ({1} logical processors, {2:N1} GiB memory)" -f $jobs, $LogicalProcessors, ($MemoryBytes / 1GB))
