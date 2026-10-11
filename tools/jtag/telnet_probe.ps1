$c = New-Object System.Net.Sockets.TcpClient("127.0.0.1",4444)
$s=$c.GetStream(); $w=New-Object System.IO.StreamWriter($s); $w.AutoFlush=$true
$r=New-Object System.IO.StreamReader($s)
$w.WriteLine(""); Start-Sleep -Milliseconds 300
$cmds=@(
 # только ЧТЕНИЕ L1 записей (не писать в таблицу!)
 "mdw 0x5FFF5000 8",
 # канал 5: EN 0x40, PAU 0x44, DESC 0x48, CFG 0x4C
 "mdw 0x01C02240 16",
 # записать DESC в канал 5 (MMIO, безопасно) и прочитать обратно
 "mww 0x01C02248 0x40100000",
 "mdw 0x01C02248 2",
 # readback канала целиком
 "mdw 0x01C02240 16",
 # глобальные readback
 "mdw 0x01C02028 1",
 "mdw 0x01C02020 1"
)
foreach($cmd in $cmds){ $w.WriteLine($cmd); Start-Sleep -Milliseconds 500 }
$w.WriteLine("resume"); Start-Sleep -Milliseconds 300
$w.WriteLine("shutdown"); Start-Sleep -Milliseconds 400
$out=$r.ReadToEnd(); $c.Close()
$out | Where-Object {$_ -match '0x01c|0x5fff|0x4010'}