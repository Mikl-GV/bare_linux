#ifndef HANDY_H
#define HANDY_H

/* Заглушка libretro-обвязки для bare-metal порта Handy (Atari Lynx).
 * В оригинале handy.h тянул libretro.h и handy_log; здесь лог — в uart. */

#include "uart.h"

enum retro_log_level {
    RETRO_LOG_DEBUG = 0,
    RETRO_LOG_INFO,
    RETRO_LOG_WARN,
    RETRO_LOG_ERROR,
};

/* Печать в UART (без очистки формат-строки — у нас нет vsnprintf в ядре;
 * сообщения в ядре статические). */
static inline void handy_log(enum retro_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}

#define RETRO_VFS_FILE_ACCESS_READ      1
#define RETRO_VFS_FILE_ACCESS_WRITE     2
#define RETRO_VFS_FILE_ACCESS_HINT_NONE 0
#define RETRO_VFS_SEEK_POSITION_START   0
#define RETRO_VFS_SEEK_POSITION_END     2

/* ---- Заглушки libretro-common (ром подаём буфером, файлы не нужны) ---- */
#define PATH_MAX_LENGTH  1024

/* string_is_empty определён в sysbase.h (включается раньше). */
static inline int path_is_valid(const char *p)   { (void)p; return 0; }
static inline void fill_pathname_resolve_relative(char *out, const char *base,
                                                  const char *name, size_t size)
{
    (void)base; (void)name; (void)size;
    if (out && size) out[0] = 0;
}

typedef struct RFILE RFILE;   /* opaque: RFILE* — указатель без void** */
static inline RFILE *filestream_open(const char *p, unsigned m, unsigned h)
{ (void)p; (void)m; (void)h; return 0; }
static inline void filestream_close(RFILE *f) { (void)f; }
static inline long filestream_tell(RFILE *f)  { (void)f; return 0; }
static inline int  filestream_seek(RFILE *f, long o, int w) { (void)f; (void)o; (void)w; return 0; }
static inline int  filestream_read(RFILE *f, void *b, size_t n) { (void)f; (void)b; (void)n; return 0; }
static inline int  filestream_write(RFILE *f, const void *b, size_t n) { (void)f; (void)b; (void)n; return 0; }

#endif /* HANDY_H */