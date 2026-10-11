$c = New-Object System.Net.Sockets.TcpClient("127.0.0.1",4444)
$s=$c.GetStream(); $w=New-Object System.IO.StreamWriter($s); $w.AutoFlush=$true
$r=New-Object System.IO.StreamReader($s)
$w.WriteLine(""); Start-Sleep -Milliseconds 300
$w.WriteLine("reg pc"); Start-Sleep -Milliseconds 300
$w.WriteLine("reset run"); Start-Sleep -Milliseconds 1000
$w.WriteLine("shutdown"); Start-Sleep -Milliseconds 300
$out=$r.ReadToEnd(); $c.Close()
$out | Select-Object -Last 20