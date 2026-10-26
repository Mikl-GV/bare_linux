/* Слой ввода/звука для эмуляторов (см. emu_io.h).
 *
 * ЕДИНСТВЕННОЕ место, где ядро встречается с реальными драйверами:
 * pcf8574 (кнопки), sega_pad (I2C 6-btn), usb_kbd (HID), i2s (DMA, CPU1).
 * Ядро работает с PAD_* маской и audio_push, никогда не зная про шины.
 *
 * Все функции НЕБЛОКИРУЮЩИЕ (кроме init): опрос ввода в течение кадра —
 * только чтение регистров (bit-bang I2C ~единицы мкс) и USB-очереди.
 */

#include "emu_io.h"
#include "pcf8574.h"
#include "sega_pad.h"
#include "usb_kbd.h"
#include "i2s.h"
#include "sys.h"

/* ---- Маппинг PCF8574-кнопок -> PAD_* ----
 * PCF8574 (0x27): UP/LEFT/RIGHT/DOWN/A/B/START/SELECT (маска BTN_*). */
static uint16_t pcf_to_pad(uint32_t b)
{
    uint16_t p = 0;
    if (b & BTN_UP)     p |= PAD_UP;
    if (b & BTN_DOWN)   p |= PAD_DOWN;
    if (b & BTN_LEFT)   p |= PAD_LEFT;
    if (b & BTN_RIGHT)  p |= PAD_RIGHT;
    if (b & BTN_A)      p |= PAD_A;
    if (b & BTN_B)      p |= PAD_B;
    if (b & BTN_START)  p |= PAD_START;
    if (b & BTN_SELECT) p |= PAD_SELECT;
    return p;
}

/* USB-клавиатура уже отдаёт BTN_*-маску (usb_kbd_poll) — тот же маппинг. */

void emu_input_poll(struct emu_input *st)
{
    if (!st) return;

    /* По умолчанию: один порт, кнопки PCF8574 (всегда доступны; молчит,
     * если PCF не отвечает — pcf8574_read возвращает 0 на ошибке шины). */
    st->npads = 1;
    st->pads[0] = pcf_to_pad(pcf8574_read());

    /* SEGA 6-btn джойстик (I2C @0x20) НЕ опрашиваем в играх/эмуляторе:
     * его блокирующий bit-bang скан (sega_pad_scan) каждым кадром вносит
     * задержку и может вешать цикл (зависание Game Boy на ~4-м кадре).
     * Джойстик служит только пункт «тест» в меню (retro_menu.c). Кнопки
     * PCF8574 (0x27) и клавиатура остаются — они неблокирующие. */

    /* USB-клавиатура: если активна и даёт нажатия — добавляем (старший
     * приоритет: кнопки поверх для навигации по меню). */
    if (usb_kbd_active()) {
        uint32_t kb = usb_kbd_poll();
        if (kb) st->pads[0] |= pcf_to_pad(kb);
    }

    for (int i = 1; i < EMU_MAX_PADS; i++) st->pads[i] = 0;
}

int emu_input_exit_hold(uint32_t hold_ms)
{
    /* Долгое удержание START: из любого источника ввода. Простейший способ —
     * опросить клавиатуру и I2C. (Бит PAD_START в pads[0] уже собран.) */
    struct emu_input st;
    emu_input_poll(&st);
    /* Удержание отслеживается по таймеру: первый вызов запоминает момент. */
    static uint32_t hold_since = 0;
    uint32_t now = micros() / 1000;
    if (st.pads[0] & PAD_START) {
        if (!hold_since) hold_since = now;
        if ((uint32_t)(now - hold_since) >= hold_ms) {
            hold_since = 0;
            return 1;
        }
    } else {
        hold_since = 0;
    }
    return 0;
}

/* ---- Звук: обёртка НАД почтой CPU1 (см. audio_cpu1.c) ----
 * Ядро отдаёт PCM-буфер сюда; слой шлёт его на CPU1 и ждёт подтверждения,
 * ПОКА CPU1 не прочитал прошлый кусок (рукопожатие по audio_done) — иначе
 * ядро перезапишет буфер, который CPU1 ещё читает (data race, треск).
 * Всё рукопожатие и знание про mailbox живёт ТОЛЬКО здесь. */

#include "mailbox.h"

static uint32_t g_audio_underrun = 0;
static uint32_t g_audio_overrun  = 0;
static uint32_t g_audio_sent     = 0;   /* сколько кусков ушло на CPU1 */

int emu_audio_push(const int16_t *pcm, unsigned frames)
{
    if (!pcm || !frames) return 0;

    /* Ждём, пока CPU1 вычитает предыдущий кусок (audio_done догонит sent).
       НЕ используем wfe: на этом двухъядерном ARM CPU0 не просыпается
       автоматически от записи CPU1 (нет SEV) — после первого аудио-куска
       CPU0 зависал здесь навсегда (белый растр + вис, кадр не обновлялся).
       Опрашиваем счётчик busy-poll, как делает CPU1 в audio_cpu1.c. */
    struct mailbox *mb = mb_get();
    /* Ожидание audio_done БЕЗ бесконечного зависания: если CPU1 (звук)
       по любой причине не подтверждает (застрял в i2s_write/кольцо забито),
       не блокируем эмулятор навсегда — после таймаута пропускаем этот кусок,
       кадр продолжит рендериться (звук может кратко прерваться). */
    uint32_t spin = 0;
    while (mb->audio_done < g_audio_sent) {
        __asm volatile("dmb" ::: "memory");
        if (++spin > 200000u) return 0;   /* ~2-3 мс на 977 МГц — сбрасываем */
    }

    /* Шлём текущий кусок; считаем устранённые переполнения — но при выходе
     * за кольцо CPU1 сам недополучит; здесь считаем только отказы до отправки. */
    mb_send(MB_MSG_AUDIO_DATA, (uint32_t)(uintptr_t)(const void *)pcm, frames);
    g_audio_sent++;

    /* CPU1 подтвердит после записи в кольцо; если кольцо полное — i2s_write
     * на CPU1 примет меньше, но мы уже передали — счётчик overrun на слое: */
    return (int)frames;
}

void emu_audio_stats(uint32_t *underrun, uint32_t *overrun)
{
    /* i2s не сообщает underrun — считаем только overrun (недобор виден
     * как отсутствие данных в кольце по таймеру; пока оставляем overrun). */
    if (underrun) *underrun = g_audio_underrun;
    if (overrun)  *overrun  = g_audio_overrun;
}

void emu_audio_start(void) { i2s_start(); }
void emu_audio_stop(void)  { i2s_stop(); }