# Живые замеры с работающей Lakka 2.3.1 (H3 + RTL8189FTV, тот же чип)

Дата: 2026-10-20. Получено напрямую по SSH с платы **10.72.75.55** (root/root), к которой подключена та же сеть **A05s пользователя М** (BSSID `9a:4d:56:7e:81:4f`, ch 1, WPA2-PSK) — ровно та, к которой подключается наш bare-metal.

Это эталонные значения регистров, CAM и security-конфигурации, снятые с **реально работающего** стека на этом железе. Использовать как primary source при сверке.

## Система

```
Linux Lakka 4.14.18 #1 SMP Fri Sep 13 14:36:07 CEST 2019 armv7l GNU/Linux
8189fs  987136  0
cfg80211 245760  1 8189fs
wlan0: inet 10.72.75.55/24, MAC 02:81:38:27:88:cd
Driver: v4.3.24_15589.20151023
```

## Security: CAM (главный ответ на «куда писать ключи»)

```
===/proc/net/rtl8189fs/wlan0/sec_cam===
max_num:16, used:0x00000030
reg_scr:0x010c

HW sec cam:
id ctrl   addr              key                              kid type MK GK valid
 0 0x0000 00:00:00:00:00:00 4c847c50...                       0 N/A  0  0  0
 1 0x0000 00:00:00:00:00:00 3d057864...                       0 N/A  0  0  0
 2 0x0000 00:00:00:00:00:00 8e232214...                       0 N/A  0  0  0
 3 0x0000 00:00:00:00:00:00 9c5eed2e...                       0 N/A  0  0  0
 4 0x8010 9a:4d:56:7e:81:4f 1f74306b43415fe984404ddbfc36c865   0 AES  0  0  1   <- pairwise (BSSID!)
 5 0x8051 9a:4d:56:7e:81:4f 6d6852f7ac066590331ed4dec354b2e1   1 AES  0  1  1   <- group GTK (BSSID!, keyid=1)
 6..15 ctrl=0x0000, invalid
```

SW-кэш (cache) — те же записи 4 и 5.

**Вывод:** для STA-режима (A05s, WPA2, GTK keyid=1):
- **pairwise**: CAM entry **4**, `ctrl=0x8010`, `addr = BSSID`.
- **group (GTK)**: CAM entry **5** (НЕ 1, НЕ 0), `ctrl=0x8051`, `addr = BSSID` (НЕ null!), `kid=1`, group-бит.
- `reg_scr = 0x010c` = `SCR_CHK_KEYID(BIT8)|SCR_RxDecEnable(BIT3)|SCR_TxEncEnable(BIT2)`.

`sec_info`:
```
auth_alg=0x2, enc_alg=0x4, auth_type=0x7, enc_type=0x6
hw_decrypted=1
aes_sw_dec_cnt=0  (аппаратный RX-decrypt работает)
```

## MAC-регистры (выборочно, ключевые)

Из `/proc/net/rtl8189fs/wlan0/mac_reg_dump`:

| Адрес | Значение | Комментарий |
|---|---|---|
| 0x600 | `0x000206ff` | CR = MACTXEN\|MACRXEN (наш `0x206FF`) ✔ |
| 0x608 | `0x700060ce` | **RCR (наш `0x700060CE`) ✔** |
| 0x610 | `0x27388102` | наш MAC 02:81:38:27:88:cd |
| 0x618 | `0x7e564d9a` | BSSID: 9a:4d:56:7e:81:4f ✔ |
| 0x630 | `0x100a2828` | rateset |
| 0x670 | `0x0000007d` | CAMCMD (последняя операция, bit17/read?) |
| 0x674 | `0x4d9a8051` | **CAMWRITE word0 = 0x8051\|9a<<16\|4d<<24 → entry 5 GTK BSSID** ✔ |
| 0x680 | `0x0000010c` | **REG_SECCFG = 0x10C** ✔ |
| 0x6a0 | `0x0400ffff` | RXFLTMAP |

## MLME/состояние

```
fwstate=0x9
pmlmeinfo->state=0x4402
qos_option=1, ht_option=1
macid_info: id=0 (9a:4d:56:7e:81:4f, state C AP), id=1 (ff:.. bmc RSVD)
current_tx_rate: macid 0 → MCS7
```

## Соответствие нашему bare-metal (W.537)

| Параметр | Lakka (эталон) | Наш код до W.537 | Наш код W.537 |
|---|---|---|---|
| pairwise CAM | entry 4, BSSID, 0x8010 | entry 4, BSSID, 0x8010 ✔ | ✔ |
| GTK CAM | entry 5, **BSSID**, 0x8051 | entry 1, **null**, 0x8051 ✘ | **entry 5, BSSID, 0x8051** ✔ |
| REG_SECCFG | 0x010C | 0x030C/0x034C ✘ | **0x010C** ✔ |
| RCR | 0x700060CE | 0x700060CE ✔ | ✔ |

## Полный MAC-дамп сохранён

Полный `mac_reg_dump` (0x000..0x7f0) — в файле `LAKKA-MACREG-FULL.txt` рядом.