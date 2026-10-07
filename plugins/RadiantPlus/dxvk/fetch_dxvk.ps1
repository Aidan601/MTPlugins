# Downloads the pinned DXVK release, checks its SHA-256 and extracts x64 dxgi.dll and d3d11.dll (not in git).
# Usage: powershell -ExecutionPolicy Bypass -File fetch_dxvk.ps1 [-Dest <folder>]   (default: this script's folder)
param([string]$Dest = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$version = '3.1.1'   # keep version.txt (shown by the plugin's status) the same
$sha256 = '40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608'
$url = "https://github.com/doitsujin/dxvk/releases/download/v$version/dxvk-$version.tar.gz"

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("dxvk-fetch-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $tgz = Join-Path $tmp "dxvk-$version.tar.gz"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $tgz -UseBasicParsing
    $got = (Get-FileHash -Algorithm SHA256 -Path $tgz).Hash.ToLower()
    if ($got -ne $sha256) { throw "dxvk-$version.tar.gz has SHA-256 $got, expected $sha256" }
    & "$env:SystemRoot\System32\tar.exe" -xzf $tgz -C $tmp
    if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE)" }
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    foreach ($n in 'dxgi.dll', 'd3d11.dll') { Copy-Item -Force (Join-Path $tmp "dxvk-$version\x64\$n") (Join-Path $Dest $n) }
    Write-Output "DXVK ${version}: dxgi.dll and d3d11.dll in $Dest"
}
finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
