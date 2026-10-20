LAKKA 2.3.1 live, H3 + RTL8189FTV, A05s пользователя М (9a:4d:56:7e:81:4f)
Снято 2026-10-20 по SSH (10.72.75.55). Полный дамп для сверки с bare-metal.

==========================================================================
/proc/net/rtl8189fs/wlan0/sec_cam (HW CAM)
==========================================================================
sec_cap:0x00
flags:0x00000000
max_num:16
used:0x00000030
reg_scr:0x010c
HW sec cam:
id ctrl   addr              key                              kid type    MK GK valid
 0 0x0000 00:00:00:00:00:00 4c847c507b546458d02c180daf3bbe77   0 N/A      0  0     0
 1 0x0000 00:00:00:00:00:00 3d05786404fa39d0a8667724ed471467   0 N/A      0  0     0
 2 0x0000 00:00:00:00:00:00 8e2322141383b3577c0b4fa45ca98014   0 N/A      0  0     0
 3 0x0000 00:00:00:00:00:00 9c5eed2ece4fbe45ef26cd675a523a45   0 N/A      0  0     0
 4 0x8010 9a:4d:56:7e:81:4f 1f74306b43415fe984404ddbfc36c865   0 AES      0  0     1
 5 0x8051 9a:4d:56:7e:81:4f 6d6852f7ac066590331ed4dec354b2e1   1 AES      0  1     1
 6 0x0000 00:00:00:00:00:00 1c9fbc724a74ff2d9484a48b05f67b4f   0 N/A      0  0     0
 7 0x0000 00:00:00:00:00:00 c6043b60f16f8527cbcef8b301ad9f7a   0 N/A      0  0     0
 8 0x0000 00:00:00:00:00:00 030a9f47b72ed7a03948de228f2c3986   0 N/A      0  0     0
 9 0x0000 00:00:00:00:00:00 6104d9d660aa1c73bf135ffe510a7066   0 N/A      0  0     0
10 0x0000 00:00:00:00:00:00 b53b9642076d793342c7648f001d8bbb   0 N/A      0  0     0
11 0x0000 00:00:00:00:00:00 f52dbfee6f42040af74c56849a5896a2   0 N/A      0  0     0
12 0x0000 00:00:00:00:00:00 df0440b0367fd10c7679a0831e259506   0 N/A      0  0     0
13 0x0000 00:00:00:00:00:00 0302c43c75ca37071db6c83d02601f1d   0 N/A      0  0     0
14 0x0000 00:00:00:00:00:00 621d3a0191f9a6be12941f7ae6160362   0 N/A      0  0     0
15 0x0000 00:00:00:00:00:00 e9bd2231ac4c04514a1194bd779d7607   0 N/A      0  0     0

==========================================================================
/proc/net/rtl8189fs/wlan0/sec_cam_cache (SW CAM cache)
==========================================================================
SW sec cam cache:
id ctrl   addr              key                              kid type    MK GK valid
 4 0x8010 9a:4d:56:7e:81:4f 1f74306b43415fe984404ddbfc36c865   0 AES      0  0     1
 5 0x8051 9a:4d:56:7e:81:4f 6d6852f7ac066590331ed4dec354b2e1   1 AES      0  1     1

==========================================================================
/proc/net/rtl8189fs/wlan0/sec_info
==========================================================================
auth_alg=0x2, enc_alg=0x4, auth_type=0x7, enc_type=0x6
hw_decrypted=1
wep_sw_enc_cnt=0, 0, 0
wep_sw_dec_cnt=0, 0, 0
tkip_sw_enc_cnt=0, 0, 0
tkip_sw_dec_cnt=0, 0, 0
aes_sw_enc_cnt=0, 0, 2
aes_sw_dec_cnt=0, 0, 0

==========================================================================
/proc/net/rtl8189fs/wlan0/ap_info
==========================================================================
SSID=A05s пользователя М
sta's macaddr:9a:4d:56:7e:81:4f
cur_channel=1, cur_bwmode=0, cur_ch_offset=0
wireless_mode=0xb, rtsen=0, cts2slef=0
state=0x1, aid=8, macid=0, raid=3
qos_en=1, ht_en=1, init_rate=19
bwmode=0, ch_offset=0, sgi_20m=1,sgi_40m=0
ampdu_enable = 1
agg_enable_bitmap=0, candidate_tid_bitmap=0
ldpc_cap=0x0, stbc_cap=0x0, beamform_cap=0x0
tid=0, enable=1, ampdu_size=16, indicate_seq=236
tid=6, enable=0, ampdu_size=255, indicate_seq=2
tid=7, enable=1, ampdu_size=16, indicate_seq=65535

==========================================================================
/proc/net/rtl8189fs/wlan0/macid_info
==========================================================================
max_num:16
used:0x00000003
id  bmc if_g ch_g macaddr           status
  0   0    0   -1 9a:4d:56:7e:81:4f  C AP
  1   1   -1   -1 ff:ff:ff:ff:ff:ff  RSVD

==========================================================================
/proc/net/rtl8189fs/wlan0/current_tx_rate
==========================================================================
macid if_g macaddr           tx_rate
    0    0 9a:4d:56:7e:81:4f MCS7

==========================================================================
/proc/net/rtl8189fs/wlan0/fwstate / mlmext_state / qos_option / ht_option
==========================================================================
fwstate=0x9
pmlmeinfo->state=0x4402
qos_option=1
ht_option=1