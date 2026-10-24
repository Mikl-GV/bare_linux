# Заливка прошивки h3_bare.bin через FEL и запуск.
# Использование: powershell -ExecutionPolicy Bypass -File tools/fel/run_fel.ps1 [-NoExe]
param(
    [switch]$NoExe     # только записать в RAM, не запускать
)

$ErrorActionPreference = "Stop"
$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$FEL  = Join-Path $PSScriptRoot "bin\sunxi-fel.exe"
$Bin  = Join-Path $Root "h3_bare.bin"
$Spl  = Join-Path $PSScriptRoot "spl\spl.bin"

Write-Host "== Prepare RAM =="
& $FEL writel 0x01C20800 0x3333
if ($LASTEXITCODE -ne 0) { throw "writel failed" }
& $FEL spl $Spl
if ($LASTEXITCODE -ne 0) { throw "spl failed" }
& $FEL write 0x40000000 $Bin
if ($LASTEXITCODE -ne 0) { throw "write failed" }
Write-Host "== Image loaded at 0x40000000 =="

if ($NoExe) {
    Write-Host "== Skipped execute (NoExe) =="
    exit 0
}

& $FEL exe 0x40000000
if ($LASTEXITCODE -ne 0) { throw "exe failed" }
Write-Host "== Running =="