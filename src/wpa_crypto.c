#include "wpa_crypto.h"

/* Bare-metal: нет libc (флаг -ffreestanding -nostdlib). Мини-примитивы. */
static uint32_t wc_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}
static void wc_memcpy(void *d, const void *s, uint32_t n)
{
    uint8_t *dp = (uint8_t *)d;
    const uint8_t *sp = (const uint8_t *)s;
    uint32_t i;
    for (i = 0; i < n; i++) dp[i] = sp[i];
}
static void wc_memset(void *d, int c, uint32_t n)
{
    uint8_t *dp = (uint8_t *)d;
    uint32_t i;
    for (i = 0; i < n; i++) dp[i] = (uint8_t)c;
}
static int wc_memcmp(const void *a, const void *b, uint32_t n)
{
    const uint8_t *ap = (const uint8_t *)a;
    const uint8_t *bp = (const uint8_t *)b;
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (ap[i] != bp[i]) return ap[i] - bp[i];
    }
    return 0;
}

/* ==================== SHA-1 (RFC 3174) ==================== */

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static uint32_t sha1_kt(uint32_t t)
{
    if (t < 20) return 0x5A827999u;
    if (t < 40) return 0x6ED9EBA1u;
    if (t < 60) return 0x8F1BBCDCu;
    return 0xCA62C1D6u;
}

static uint32_t sha1_f(uint32_t t, uint32_t b, uint32_t c, uint32_t d)
{
    if (t < 20) return (b & c) | ((~b) & d);
    if (t < 40) return b ^ c ^ d;
    if (t < 60) return (b & c) | (b & d) | (c & d);
    return b ^ c ^ d;
}

static void sha1_block(uint32_t st[5], const uint8_t *p)
{
    uint32_t w[80];
    uint32_t t;
    for (t = 0; t < 16; t++)
        w[t] = ((uint32_t)p[t*4] << 24) | ((uint32_t)p[t*4+1] << 16) |
               ((uint32_t)p[t*4+2] << 8) | (uint32_t)p[t*4+3];
    for (t = 16; t < 80; t++)
        w[t] = ROL32(w[t-3] ^ w[t-8] ^ w[t-14] ^ w[t-16], 1);

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (t = 0; t < 80; t++) {
        uint32_t tmp = ROL32(a, 5) + sha1_f(t, b, c, d) + e + w[t] + sha1_kt(t);
        e = d; d = c; c = ROL32(b, 30); b = a; a = tmp;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

void wpa_sha1(const uint8_t *data, uint32_t len, uint8_t out[20])
{
    uint32_t st[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t bitlen = (uint64_t)len * 8;
    uint8_t blk[64];
    uint32_t full = len / 64;
    uint32_t i;
    for (i = 0; i < full; i++) sha1_block(st, data + i*64);

    uint32_t rem = len % 64;
    wc_memcpy(blk, data + full*64, rem);
    blk[rem] = 0x80;
    wc_memset(blk + rem + 1, 0, 64 - rem - 1);
    if (rem >= 56) {
        sha1_block(st, blk);
        wc_memset(blk, 0, 56);
    }
    for (i = 0; i < 8; i++)
        blk[63 - i] = (uint8_t)(bitlen >> (i * 8));
    sha1_block(st, blk);

    for (i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(st[i] >> 24);
        out[i*4+1] = (uint8_t)(st[i] >> 16);
        out[i*4+2] = (uint8_t)(st[i] >> 8);
        out[i*4+3] = (uint8_t)st[i];
    }
}

/* ==================== HMAC-SHA1 ==================== */

void wpa_hmac_sha1(const uint8_t *key, uint32_t key_len,
                   const uint8_t *msg, uint32_t msg_len,
                   uint8_t out[20])
{
    uint8_t k[64];
    uint8_t i_pad[64], o_pad[64];
    uint8_t inner[20];
    uint32_t i;

    if (key_len > 64) {
        wpa_sha1(key, key_len, k);
        wc_memset(k + 20, 0, 64 - 20);
    } else {
        wc_memcpy(k, key, key_len);
        wc_memset(k + key_len, 0, 64 - key_len);
    }
    for (i = 0; i < 64; i++) { i_pad[i] = k[i] ^ 0x36; o_pad[i] = k[i] ^ 0x5C; }

    /* inner = SHA1(K XOR ipad || msg) */
    {
        uint8_t buf[64 + 900];
        wc_memcpy(buf, i_pad, 64);
        wc_memcpy(buf + 64, msg, msg_len);
        wpa_sha1(buf, 64 + msg_len, inner);
    }
    {
        uint8_t buf[64 + 20];
        wc_memcpy(buf, o_pad, 64);
        wc_memcpy(buf + 64, inner, 20);
        wpa_sha1(buf, 64 + 20, out);
    }
}

/* ==================== PBKDF2-HMAC-SHA1 (RFC 2898) ==================== */

static void xor_block(uint8_t *d, const uint8_t *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) d[i] ^= s[i];
}

void wpa_pbkdf2_sha1(const uint8_t *pw, uint32_t pw_len,
                     const uint8_t *salt, uint32_t salt_len,
                     uint32_t iter, uint8_t *dk, uint32_t dk_len)
{
    uint32_t block = 1;
    uint32_t pos = 0;
    while (pos < dk_len) {
        uint8_t salt_blk[64 + 4];
        uint8_t u[20], tmp[20];
        uint32_t i;
        wc_memcpy(salt_blk, salt, salt_len);
        salt_blk[salt_len] = (uint8_t)((block >> 24) & 0xFF);
        salt_blk[salt_len+1] = (uint8_t)((block >> 16) & 0xFF);
        salt_blk[salt_len+2] = (uint8_t)((block >> 8) & 0xFF);
        salt_blk[salt_len+3] = (uint8_t)(block & 0xFF);
        wpa_hmac_sha1(pw, pw_len, salt_blk, salt_len + 4, u);
        wc_memcpy(tmp, u, 20);
        for (i = 1; i < iter; i++) {
            wpa_hmac_sha1(pw, pw_len, u, 20, u);
            xor_block(tmp, u, 20);
        }
        uint32_t n = (dk_len - pos > 20) ? 20 : (dk_len - pos);
        wc_memcpy(dk + pos, tmp, n);
        pos += n;
        block++;
    }
}

/* ==================== IEEE 802.11 PRF (T-PRF) ==================== */

void wpa_prf(const uint8_t *key, uint32_t key_len,
             const char *label,
             const uint8_t *data, uint32_t data_len,
             uint8_t *out, uint32_t bits)
{
    uint32_t i = 0;
    uint32_t pos = 0;
    uint32_t label_len = wc_strlen(label);
    while (bits > 0) {
        uint32_t A_len = 0;
        uint8_t A[20];
        /* A(0) = HMAC-SHA1(key, label||0||data||i) → потом HMAC в SHA1 ctx */
        uint8_t input[900];
        uint32_t ilen = 0;
        wc_memcpy(input, label, label_len); ilen += label_len;
        input[ilen++] = 0;
        wc_memcpy(input + ilen, data, data_len); ilen += data_len;
        input[ilen++] = (uint8_t)(i & 0xFF);
        /* A = HMAC-SHA1(key, input) */
        wpa_hmac_sha1(key, key_len, input, ilen, A);
        /* H = HMAC-SHA1(key, A || label || 0 || data || i) */
        wc_memcpy(input, A, 20); ilen = 20;
        wc_memcpy(input + ilen, label, label_len); ilen += label_len;
        input[ilen++] = 0;
        wc_memcpy(input + ilen, data, data_len); ilen += data_len;
        input[ilen++] = (uint8_t)(i & 0xFF);
        uint8_t H[20];
        wpa_hmac_sha1(key, key_len, input, ilen, H);

        uint32_t n = (bits > 160) ? 20 : (bits / 8);
        wc_memcpy(out + pos, H, n);
        pos += n;
        bits -= n * 8;
        i++;
        (void)A_len;
    }
}

/* ==================== EAPOL MIC check (HMAC-SHA1 over KCK) ==================== */

int wpa_mic_check(const uint8_t *kck, const uint8_t *eapol, uint32_t len,
                  const uint8_t *msg_mic)
{
    uint8_t digest[20];
    wpa_hmac_sha1(kck, 16, eapol, len, digest);
    /* MIC первых 16 байт дайджеста сравниваем с msg_mic */
    return (wc_memcmp(digest, msg_mic, 16) == 0) ? 1 : 0;
}

/* ==================== Nonce (deterministic from seed) ==================== */

void wpa_fill_nonce(uint8_t dst[32], uint32_t seed)
{
    uint32_t state = seed ^ 0x9E3779B9u;
    uint32_t i;
    for (i = 0; i < 8; i++) {
        state = state * 1664525u + 1013904223u;
        dst[i*4]   = (uint8_t)(state >> 24);
        dst[i*4+1] = (uint8_t)(state >> 16);
        dst[i*4+2] = (uint8_t)(state >> 8);
        dst[i*4+3] = (uint8_t)state;
    }
}