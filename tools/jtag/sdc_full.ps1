$c = New-Object System.Net.Sockets.TcpClient("127.0.0.1",4444)
$s=$c.GetStream(); $w=New-Object System.IO.StreamWriter($s); $w.AutoFlush=$true
$r=New-Object System.IO.StreamReader($s)
$w.WriteLine(""); Start-Sleep -Milliseconds 300
# Полный снимок регистров SDC1 и связанных, чтобы понять почему CMD5 не даёт ответа.
$cmds=@(
 "mdw 0x01C10000 12",   # GCTL, CKCR, TMOR, CTYPE, BLKSZ, BYCNT, CMDR, ARG, RESP0-3
 "mdw 0x01C1003C 4",    # STAR, FWLR, FUNS, A12A
 "mdw 0x01C2008C 1",    # SDMMC1_CLK
 "mdw 0x01C202C0 1",    # RST0
 "mdw 0x01C208D8 2",    # PG_CFG0 (PG0-3)
 "mdw 0x01C208DC 1"     # PG_CFG1 (PG4-5)
)
foreach($cmd in $cmds){ $w.WriteLine($cmd); Start-Sleep -Milliseconds 300 }
$w.WriteLine("resume"); Start-Sleep -Milliseconds 200
$w.WriteLine("shutdown"); Start-Sleep -Milliseconds 300
$out=$r.ReadToEnd(); $c.Close()
$out | Where-Object {$_ -match '0x01c|0x01f0'}