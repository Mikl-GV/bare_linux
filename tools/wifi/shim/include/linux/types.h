#ifndef __SHIM_LINUX_TYPES_H__
#define __SHIM_LINUX_TYPES_H__

/* Минимальный Linux-совместимый слой типов для порта rtl8189fs под bare-metal.
   Покрывает базовые типы, которые драйвер берёт из linux/types.h под
   PLATFORM_LINUX. Без этого sdio_halinit.c не компилируется (~1191 ошибок
   были почти все про отсутствие u8/IN/VOID/BOOLEAN/PADAPTER и т.п.). */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --- Базовые целочисленные типы --- */
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;
typedef uint8_t   u1Byte;
typedef uint16_t  u2Byte;
typedef uint32_t  u4Byte;
typedef uint64_t  u8Byte;
typedef uint8_t   s1Byte;
typedef int8_t    s1Byte_alias;
typedef uint16_t  s2Byte_alias;
typedef uint32_t  s4Byte_alias;
typedef uint32_t  s4Byte;
typedef uint64_t  PU;
typedef void     *PVOID;
typedef uint8_t  *pu1Byte;
typedef uint16_t *pu2Byte;
typedef uint32_t *pu4Byte;

/* --- Deamon/маркеры DRIVER --- */
#define IN
#define OUT
#define VOID void

typedef int8_t  BOOLEAN;
typedef unsigned int NDIS_OID;
typedef unsigned int NDIS_STATUS;
typedef unsigned int uint;

typedef uint32_t SIZE_T;
typedef int32_t  SSIZE_T;
#define FIELD_OFFSET(s, field) ((SSIZE_T)&(((s*)0)->field))

#define UCHAR u8
#define USHORT u16
#define UINT   u32
#define ULONG  u32

typedef void (*proc_t)(void*);

/* --- атомарные (заглушки под bare-metal, без SMP) --- */
typedef volatile int atomic_t;
#define atomic_read(v)  (*(v))
#define atomic_set(v,i) (*(v) = (i))

/* --- bool --- */
#ifndef bool
#define bool _Bool
#endif

#endif /* __SHIM_LINUX_TYPES_H__ */