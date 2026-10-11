# Заливка прошивки h3_bare.bin через FEL и снятие UART-лога.
# Использование: powershell -ExecutionPolicy Bypass -File tools/fel/run_fel.ps1 [-ExeCo | -NoExe]
param(
    [switch]$NoExe     # только записать в RAM, не запускать
)

$ErrorActionPreference = "Stop"
$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$FEL  = Join-Path $PSScriptRoot "bin\sunxi-fel.exe"
$Bin  = Join-Path $Root "h3_bare.bin"
$Spl  = Join-Path $PSScriptRoot "spl\spl.bin"
$Log  = Join-Path $Root "uart_capture.txt"
$COM  = "COM10"
$Baud = 115200

Write-Host "== Prapare RAM =="
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

# Открыть UART и запустить прошивку
$port = New-Object System.IO.Ports.SerialPort $COM,$Baud,None,8,One
$port.ReadTimeout = 200
$port.NewLine = "`n"
$port.Open()

$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds(15)
$started = $false

# Запустить CPU
$p = Start-Process -FilePath $FEL -ArgumentList "exe","0x40000000" -NoNewWindow -PassThru

while ((Get-Date) -lt $deadline) {
    while ($port.BytesToRead -gt 0) {
        try {
            $ch = $port.ReadExisting()
            [void]$sb.Append($ch)
            if (-not $started) { $started = $true }
        } catch {}
    }
    if ($p.HasExited -and -not $started) { Start-Sleep -Milliseconds 50 }
    if ($started -and $sb.ToString().Contains("wifi ok")) { break }
    Start-Sleep -Milliseconds 20
}

$port.Close()
$txt = $sb.ToString()
Set-Content -LiteralPath $Log -Value $txt -Encoding ASCII
Write-Host "== UART captured ($($txt.Length) chars) =="
Write-Host $txt