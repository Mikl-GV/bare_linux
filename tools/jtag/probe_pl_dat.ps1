$c = New-Object System.Net.Sockets.TcpClient("127.0.0.1",4444)
$s=$c.GetStream(); $w=New-Object System.IO.StreamWriter($s); $w.AutoFlush=$true
$r=New-Object System.IO.StreamReader($s)
$w.WriteLine(""); Start-Sleep -Milliseconds 300
# Зонд PL_DAT (0x01F02C10): прочитать, записать bit7, прочитать.
# Также прочитать PL_CFG0.
$cmds=@(
 "mdw 0x01F02C10 1",
 "mww 0x01F02C10 0x00000580",
 "mdw 0x01F02C10 1",
 "mdw 0x01F02C00 1"
)
foreach($cmd in $cmds){ $w.WriteLine($cmd); Start-Sleep -Milliseconds 300 }
$w.WriteLine("resume"); Start-Sleep -Milliseconds 200
$w.WriteLine("shutdown"); Start-Sleep -Milliseconds 300
$out=$r.ReadToEnd(); $c.Close()
$out | Where-Object {$_ -match '0x01f0'}