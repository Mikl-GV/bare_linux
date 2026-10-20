echo "=== FREE_TXPG (sdio_local 0x20) ==="
cat /proc/net/rtl8189fs/wlan0/sdio_local_reg_dump | head -2
echo "=== 0x400 row ==="
cat /proc/net/rtl8189fs/wlan0/mac_reg_dump | grep "^0x400"
echo "=== mac_qinfo ==="
cat /proc/net/rtl8189fs/wlan0/mac_qinfo
echo "=== tx_packets ==="
cat /sys/class/net/wlan0/statistics/tx_packets
echo "=== macid_info ==="
cat /proc/net/rtl8189fs/wlan0/macid_info
echo "=== sec_cam ==="
cat /proc/net/rtl8189fs/wlan0/sec_cam