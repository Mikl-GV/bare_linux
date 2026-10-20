#!/bin/sh
# Снятие TX-пути с живой Lakka 2.3.1 (rtl8189fs, та же плата/сеть).
# Снимает ПОЛНЫЙ дамп MAC-регистров (включая TX-FIFO 0x400-0x5FF) и proc-файлы.
# Выполняется на Lakka через SSH (root@10.72.75.55).

echo "===== MAC REG DUMP (0x000-0x7FF), особ. 0x400-0x5FF ====="
cat /proc/net/rtl8189fs/wlan0/mac_reg_dump 2>/dev/null || \
cat /proc/net/rtl8189fs/wlan0/reg_dump 2>/dev/null || \
echo "NO mac_reg_dump"

echo
echo "===== TX FIFO status (SDIO local) через debugfs ====="
ls /sys/kernel/debug/ 2>/dev/null | head
ls /sys/kernel/debug/rtl8189fs 2>/dev/null
for f in /sys/kernel/debug/rtl8189fs/wlan0/*; do
  echo "--- $f ---"; cat "$f" 2>/dev/null
done

echo
echo "===== proc net rtl8189fs ====="
for f in /proc/net/rtl8189fs/wlan0/*; do
  echo "--- $f ---"; cat "$f" 2>/dev/null
done

echo
echo "===== free page counters (echo read 0x20) ====="
# Попытка через proc (если есть)
for entry in /proc/net/rtl8189fs/wlan0/*tx* /proc/net/rtl8189fs/wlan0/*fifo* /proc/net/rtl8189fs/wlan0/*page*; do
  [ -f "$entry" ] && { echo "--- $entry ---"; cat "$entry"; }
done

echo
echo "===== devmem available? ====="
which devmem 2>/dev/null || echo "no devmem"
which busybox 2>/dev/null
busybox devmem 0x01c20ac0 2>/dev/null || echo "no busybox devmem"

echo
echo "===== wlan state ====="
ip addr show wlan0 2>/dev/null
cat /proc/version 2>/dev/null | head -1