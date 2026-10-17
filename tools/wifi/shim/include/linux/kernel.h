#ifndef __SHIM_LINUX_KERNEL_H__
#define __SHIM_LINUX_KERNEL_H__

/* Минимальный совместимый слой <linux/kernel.h> для bare-metal порта rtl8189fs. */
#include "types.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define clamp(v, lo, hi) min(max((v), (lo)), (hi))
#define BITS_PER_LONG (32)
#define BIT(nr) (1UL << (nr))
#define BIT_ULL(nr) (1ULL << (nr))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define round_up(x, y) ((((x) + (y) - 1) / (y)) * (y))
#define round_down(x, y) (((x) / (y)) * (y))
#define lower_32_bits(n) ((u32)(n))
#define upper_32_bits(n) ((u32)((n) >> 32))
#define FIELD_GET(mask, reg) (((reg) & (mask)) >> __ffs(mask))
#define __ffs(x) (__builtin_ctz(x))

#define printk(...)
#define pr_err(...)
#define pr_warn(...)
#define pr_info(...)
#define pr_debug(...)

#define ktime_get_seconds() 0
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define cpu_to_le16(x) (x)
#define le16_to_cpu(x) (x)
#define cpu_to_be32(x) (x)
#define be32_to_cpu(x) (x)
#define htonl(x) (x)
#define ntohl(x) (x)
#define htons(x) (x)
#define ntohs(x) (x)

typedef unsigned long gfp_t;
#define GFP_KERNEL 0
#define GFP_ATOMIC 0

#endif /* __SHIM_LINUX_KERNEL_H__ */