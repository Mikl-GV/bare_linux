$env:Path="C:\msys64\ucrt64\bin;"+$env:Path
$ocd="C:\msys64\ucrt64\bin\openocd.exe"
$cfg="E:\Giga_1\Bare_linux\tools\jtag\h3_pico_jtag.cfg"
$tmp="C:\Users\PCB\AppData\Local\Temp\gigatool"
Get-Process -Name openocd -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep 1
Start-Process -FilePath $ocd -ArgumentList @('-f',"$cfg") -RedirectStandardOutput "$tmp\j.log" -RedirectStandardError "$tmp\j_err.log"
Start-Sleep 7
Write-Output "RUNNING"