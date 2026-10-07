# Play Test: Defender exclusion for the Play Test folder so BO3LiveBridge.dll is not quarantined (false positive). Run elevated.
# Usage: defender_playtest.ps1 add|remove|repair "<game>\bin\plugins\PlayTest" (repair = add, then restore the DLL from quarantine)
# Exit: 0 done, 1 refused, 2 not applied, 3 Defender passive, 4 bad arguments, 5 DLL still missing.
param(
    [Parameter(Position = 0)][string]$Mode,
    [Parameter(Position = 1)][string]$Dir
)
if (($Mode -ne 'add' -and $Mode -ne 'remove' -and $Mode -ne 'repair') -or -not $Dir) { exit 4 }
$Dir = $Dir.TrimEnd('\')
$dll = Join-Path $Dir 'BO3LiveBridge.dll'
try {
    $status = Get-MpComputerStatus -ErrorAction Stop
    if ($status.AMRunningMode -ne 'Normal') {
        if ($Mode -eq 'repair' -and -not (Test-Path -LiteralPath $dll)) { exit 5 }
        exit 3
    }
    if ($Mode -eq 'remove') { Remove-MpPreference -ExclusionPath $Dir -ErrorAction SilentlyContinue }
    else { Add-MpPreference -ExclusionPath $Dir -ErrorAction Stop }
    $listed = @((Get-MpPreference -ErrorAction Stop).ExclusionPath)
    $has = $listed -contains $Dir
    if ($Mode -eq 'remove') { if ($has) { exit 2 }; exit 0 }
    if (-not $has) { exit 2 }
    if ($Mode -eq 'repair' -and -not (Test-Path -LiteralPath $dll)) {
        # by path (MpCmdRun refuses -Name together with -FilePath)
        $mpcmd = Join-Path $env:ProgramFiles 'Windows Defender\MpCmdRun.exe'
        & $mpcmd -Restore -FilePath $dll | Out-Null
        if (-not (Test-Path -LiteralPath $dll)) { exit 5 }
    }
    exit 0
} catch {
    exit 1
}
