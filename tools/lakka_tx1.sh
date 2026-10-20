echo "===== MAC REG 0x400-0x5FF (TX FIFO BDNY) ====="
cat /proc/net/rtl8189fs/wlan0/mac_reg_dump | sed -n '/0x400/,/0x5f0/p'
echo "===== fwstate ====="
cat /proc/net/rtl8189fs/wlan0/fwstate
echo "===== hal_spec ====="
cat /proc/net/rtl8189fs/wlan0/hal_spec | head -30
echo "===== tx_power / bw ====="
cat /proc/net/rtl8189fs/wlan0/bw_mode