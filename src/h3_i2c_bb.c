#include "h3_i2c_bb.h"
#include "ports.h"
#include "sys.h"

/* Битовый I2C. Линии - входы с подтяжкой вверх (pull-up), активный уровень -
   притянут к 0 переводом пина в выход-low. "Писать 1" = вернуть в input
   (подтяжка сама тянет вверх). Это даёт правильный open-drain без конфликта
   с slave-ACK. */

static uint32_t s_scl_port, s_scl_pin;
static uint32_t s_sda_port, s_sda_pin;

static void scl_hi(void) { set_pin_mode(s_scl_port, s_scl_pin, 0); }          /* input -> подтяжка вверх */
/* Переход в «выход-0»: сначала пишем data=0, затем режим output.
   Обратный порядок (mode, потом data) даёт короткий высокий импульс,
   если в data-регистре осталась 1 от прошлого такта. */
static void scl_lo(void) { set_pin_data(s_scl_port, s_scl_pin, 0); set_pin_mode(s_scl_port, s_scl_pin, 1); }
static void sda_hi(void) { set_pin_mode(s_sda_port, s_sda_pin, 0); }          /* input -> подтяжка вверх */
static void sda_lo(void) { set_pin_data(s_sda_port, s_sda_pin, 0); set_pin_mode(s_sda_port, s_sda_pin, 1); }
static uint32_t sda_read(void) { return get_pin_data(s_sda_port, s_sda_pin); }

static void delay_half(void) { udelay(5); }   /* ~100 кГц */

void i2cbb_init(uint32_t scl_port, uint32_t scl_pin,
                uint32_t sda_port, uint32_t sda_pin)
{
    s_scl_port = scl_port; s_scl_pin = scl_pin;
    s_sda_port = sda_port; s_sda_pin = sda_pin;

    /* Вход + подтяжка вверх на обеих линиях. */
    set_pin_mode(s_scl_port, s_scl_pin, 0);
    set_pin_mode(s_sda_port, s_sda_pin, 0);
    set_pin_pull(s_scl_port, s_scl_pin, 1);
    set_pin_pull(s_sda_port, s_sda_pin, 1);
    scl_hi(); sda_hi();
}

static void i2c_start(void)
{
    sda_hi(); scl_hi();
    delay_half();
    sda_lo();             /* SDA low while SCL high = START */
    delay_half();
    scl_lo();
    delay_half();
}

static void i2c_stop(void)
{
    sda_lo();
    delay_half();
    scl_hi();
    delay_half();
    sda_hi();             /* SDA rising while SCL high = STOP */
    delay_half();
}

static void i2c_clock_bit(void)
{
    scl_hi();
    delay_half();
    scl_lo();
    delay_half();
}

/* Передать 1 байт (MSB first), вернуть ACK бит (0=ACK). */
static int i2c_write_byte(uint8_t data)
{
    for (int i = 7; i >= 0; i--) {
        if (data & (1u << i)) sda_hi(); else sda_lo();
        delay_half();
        i2c_clock_bit();
    }
    /* 9-й такт: отпускаем SDA, читаем ACK */
    sda_hi();
    delay_half();
    scl_hi();
    delay_half();
    int ack = sda_read();
    scl_lo();
    delay_half();
    return ack;   /* 0 = ACK */
}

/* Прочитать байт, sent_ack: 1 = послать ACK, 0 = NACK (последний). */
static uint8_t i2c_read_byte(int sent_ack)
{
    uint8_t data = 0;
    sda_hi();   /* освободить SDA */
    for (int i = 0; i < 8; i++) {
        scl_hi();
        delay_half();
        data = (data << 1) | (sda_read() & 1u);
        scl_lo();
        delay_half();
    }
    if (sent_ack) sda_lo(); else sda_hi();
    delay_half();
    scl_hi();
    delay_half();
    scl_lo();
    sda_hi();
    delay_half();
    return data;
}

int i2cbb_write(uint8_t addr7, const uint8_t *buf, int len)
{
    i2c_start();
    if (i2c_write_byte((addr7 << 1) | 0) != 0) {  /* адрес + W */
        i2c_stop();
        return -1;
    }
    for (int i = 0; i < len; i++) {
        if (i2c_write_byte(buf[i]) != 0) {
            i2c_stop();
            return -2;
        }
    }
    i2c_stop();
    return 0;
}

int i2cbb_read(uint8_t addr7, uint8_t *buf, int len)
{
    i2c_start();
    if (i2c_write_byte((addr7 << 1) | 1) != 0) {  /* адрес + R */
        i2c_stop();
        return -1;
    }
    for (int i = 0; i < len; i++) {
        buf[i] = i2c_read_byte(i < len - 1 ? 1 : 0);
    }
    i2c_stop();
    return 0;
}