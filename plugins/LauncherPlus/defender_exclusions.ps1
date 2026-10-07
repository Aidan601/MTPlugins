# Adds or removes Defender PROCESS exclusions for cod2map64.exe, linker_modtools.exe and sound\snd_convert.exe (needs admin).
# Usage: defender_exclusions.ps1 add|remove "<game>\bin"
# Exit: 0 done, 1 refused (managed or no Defender), 2 not applied, 3 Defender passive (other antivirus), 4 bad arguments.
param(
    [Parameter(Position = 0)][string]$Mode,
    [Parameter(Position = 1)][string]$Bin
)
if (($Mode -ne 'add' -and $Mode -ne 'remove') -or -not $Bin) { exit 4 }
$Bin = $Bin.TrimEnd('\')
$tools = @("$Bin\cod2map64.exe", "$Bin\linker_modtools.exe", "$Bin\sound\snd_convert.exe")
try {
    $status = Get-MpComputerStatus -ErrorAction Stop
    if ($status.AMRunningMode -ne 'Normal') { exit 3 }
    if ($Mode -eq 'add') { Add-MpPreference -ExclusionProcess $tools -ErrorAction Stop }
    else { Remove-MpPreference -ExclusionProcess $tools -ErrorAction SilentlyContinue }
    $listed = @(Get-MpPreference -ErrorAction Stop).ExclusionProcess
    $have = @($tools | Where-Object { $listed -contains $_ }).Count
    if ($Mode -eq 'add' -and $have -ne $tools.Count) { exit 2 }
    if ($Mode -eq 'remove' -and $have -ne 0) { exit 2 }
    exit 0
} catch {
    exit 1
}
