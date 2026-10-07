# Used by NewPlugin.bat: copies example\HelloPlugin to MyPlugins\<Name>\ and renames everything in it.
param([string]$Name)

$ErrorActionPreference = 'Stop'
$sdk = Split-Path -Parent $PSScriptRoot
$example = Join-Path $sdk 'example\HelloPlugin'
$Name = "$Name".Trim()

function Fail([string]$why) { Write-Host ''; Write-Host "Not created: $why" -ForegroundColor Red; exit 1 }

if (-not $Name) { Fail 'no name given.' }
if ($Name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { Fail "'$Name' can only contain letters, digits and _ (and must not start with a digit)." }
if ($Name -eq 'HelloPlugin') { Fail 'HelloPlugin is the example itself; pick another name.' }
if (-not (Test-Path (Join-Path $example 'HelloPlugin.vcxproj'))) { Fail "the example was not found at $example." }

$dest = Join-Path $sdk "MyPlugins\$Name"
if (Test-Path $dest) { Fail "$dest already exists." }

# the build copies the DLL to <game>\bin\plugins\<Name>\, so an existing plugin of that name would be overwritten
if ($env:TA_TOOLS_PATH) {
    $installed = Join-Path $env:TA_TOOLS_PATH.Trim() "bin\plugins\$Name"
    if (Test-Path $installed) { Fail "a plugin called $Name is already installed ($installed). Pick another name." }
}

New-Item -ItemType Directory -Path $dest | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding $false
$guid = '{' + ([guid]::NewGuid().ToString().ToUpper()) + '}'
$files = Get-ChildItem -Path $example -File | Where-Object { $_.Extension -in '.cpp', '.h', '.hpp', '.vcxproj', '.txt', '.ini' }
foreach ($f in $files) {
    $text = [System.IO.File]::ReadAllText($f.FullName)
    $text = $text.Replace('{F4B6C8DA-3D5A-4B7C-9EAF-0A2B3C4D5E6F}', $guid)
    $text = $text.Replace('HelloPlugin', $Name).Replace('Hello Plugin', $Name).Replace('AddMenu(rp::self, "Hello")', "AddMenu(rp::self, `"$Name`")")
    if ($f.Extension -eq '.cpp') {
        $text = "// $Name`: made from the HelloPlugin example by NewPlugin.bat on $(Get-Date -Format yyyy-MM-dd).`r`n" + $text
    }
    $target = Join-Path $dest ($f.Name.Replace('HelloPlugin', $Name))
    [System.IO.File]::WriteAllText($target, $text, $utf8)
}

Write-Host ''
Write-Host "Created $dest" -ForegroundColor Green
Get-ChildItem $dest | ForEach-Object { Write-Host "  $($_.Name)" }
Write-Host ''
Write-Host 'Next:'
Write-Host "  1. Open $Name.vcxproj in Visual Studio, pick Release and x64, and Build (Ctrl+Shift+B)."
Write-Host "     The DLL is copied to <game>\bin\plugins\$Name\."
Write-Host "  2. Start Radiant or APE with plugins, press Ctrl+Shift+P and tick $Name."
Write-Host "  3. It starts as a copy of HelloPlugin, including the Ctrl+Shift+H hotkey: change $Name.cpp to make it yours."
exit 0
