# SDIO-трейс W.233 (полный доступ к чипу RTL8189FTV, bare-metal H3)

> Снято прошивкой W.233 (WIFI_SDIO_TRACE в src/h3_sdc.c) с Orange Pi Lite
> (FEL), чип RTL8189FTV на SDC1 (PG0-PG5). Каждая строка — одна CMD52/CMD53:
>   <52R f{fn} r0x{reg} ->0x{val} rc0   (CMD52 чтение)
>   <52W f{fn} r0x{reg} =0x{val} rc0    (CMD52 запись)
>   <53R f{fn} b0x{addr} l{len} rc0     (CMD53 чтение, byte-mode)
>   <53W f{fn} b0x{addr} l{len} rc0     (CMD53 запись, byte-mode)
>   суффикс "ni" = noinc (без автоинкремента).
> Назначение: сравнение с эталонным поадресным логом Armbian (Linux + rtl8189fs)
> — где наша последовательность расходится (шина/такт/регистры).

## Ключевые точки лога (что проверить в Armbian)

- Ширина шины: весь лог на **1-bit** (CCCR 0x07=0x00, CTYPE=0; после FW 4-bit
  отключён — в W.230 переход на 4-bit ломал data-фазу).
- Такт: 6 МГц (24M/2/2, N=0,M=2) — не 400 кГц init-частоты Linux.
- pwon: 0x05/0x06/0x05/0x27 — WRITE-шаги (поллинги НЕ сходятся на этом чипе).
- fwdl: 5 страниц (0x1000..0x4950), tail l2; MCUFWDL 0x80: 5->4->6->C6 (FW READY).
- BB: 209 рег. (0x100, 0x800..0xF4C); AGC: 66 рег. 0xC78/0xC50;
  RF: 136 рег. через LSSI (0x840 write, 0x824 edge, 0x8B8/0x8A0 read).
- LCK: 50 мс (по трейсу), RF 0x18=0x401, RF 0x00=0x33D95.
- RX: IGI 0x1E (0xC50), MSR=NOLINK, H2C 0x20 (0x1D0/0x1F0/0x1CC), CCCR IENM/FBR,
  RCR 0x7000604E, RRSR=0xFFF, RXFLTMAP, RQPN=0x80E0020C, LLT, RDG.
- Свип: каналы 1/6/11, но cca=0x8CCD8300 СТАТИЧЕН, rxlen/pkt=0, hisr=0.
- TX probe: hiq_free=0 -> rc=-2 (TX FIFO HIQ не выделен).

## Что ищем в эталоне (Armania/8189fs)
1. Реальную ширину/частоту шины после init (dtb bus-width=<4>, но фазы/такm).
2. pwon: поллинги или WRITE?
3. Есть ли где-то в Linux путь 0x0800, 0x0874 (RFMOD), 0x0522 (TXPAUSE) ДО LCK.
4. Инициализация TX FIFO (rsvd pages / HIQ) до probe.
5. Что ставится в 0x0A08 (cca), 0x0C4C (ofdm) — наши ли регистры.