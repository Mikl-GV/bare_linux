$c = New-Object System.Net.Sockets.TcpClient("127.0.0.1",4444)
$s=$c.GetStream(); $w=New-Object System.IO.StreamWriter($s); $w.AutoFlush=$true
$r=New-Object System.IO.StreamReader($s)
$w.WriteLine(""); Start-Sleep -Milliseconds 300
# SDC1 регистры (база 0x01C10000) + GPIO порт G (PG0-5, база 0x01C208D8) + порт L (PL7 WIFI_EN, база 0x01F02C00)
$cmds=@(
 "mdw 0x01C10000 12",
 "mdw 0x01C10040 4",
 "mdw 0x01C1005C 2",
 "mdw 0x01C10078 1",
 # порт G: cfg0 0x00 (PG0-3), cfg1 0x04 (PG4-5), data 0x10
 "mdw 0x01C208D8 2",
 "mdw 0x01C208DC 1",
 "mdw 0x01C208E8 1",
 # порт L: cfg0 0x00, data 0x10 (бит7 = WIFI_EN PL7)
 "mdw 0x01F02C00 2",
 "mdw 0x01F02C10 1",
 # CCU такты MMC1
 "mdw 0x01C2008C 1",
 "mdw 0x01C20060 1",
 "mdw 0x01C202C0 1"
)
foreach($cmd in $cmds){ $w.WriteLine($cmd); Start-Sleep -Milliseconds 400 }
$w.WriteLine("resume"); Start-Sleep -Milliseconds 200
$w.WriteLine("shutdown"); Start-Sleep -Milliseconds 300
$out=$r.ReadToEnd(); $c.Close()
$out | Where-Object {$_ -match '0x01c|0x01f0'}