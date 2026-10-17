#ifndef __SHIM_LINUX_SPINLOCK_H__
#define __SHIM_LINUX_SPINLOCK_H__

/* Минимальный <linux/spinlock.h> для bare-metal порта rtl8189fs.
   Bare-metal однопоточен, поэтому все примитивы — no-op. */

#include "types.h"

typedef struct {
    int raw_lock;
} spinlock_t;

typedef struct {
    int lock;
} raw_spinlock_t;

typedef struct {
    int val;
} atomic_t_s;

#define spin_lock(lock)      do { (void)(lock); } while (0)
#define spin_unlock(lock)    do { (void)(lock); } while (0)
#define spin_lock_bh(lock)   do { (void)(lock); } while (0)
#define spin_unlock_bh(lock) do { (void)(lock); } while (0)
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (void)(flags); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
#define spin_lock_init(lock) do { (void)(lock); } while (0)

#define DEFINE_SPINLOCK(x) spinlock_t x = { 0 }
#define DEFINE_MUTEX(x)    int x = 0

typedef struct { int initialized; } mutex_t;

#define mutex_init(m)      do { (void)(m); } while (0)
#define mutex_lock(m)      do { (void)(m); } while (0)
#define mutex_unlock(m)    do { (void)(m); } while (0)
#define mutex_trylock(m)   ({ (void)(m); 1; })

#endif /* __SHIM_LINUX_SPINLOCK_H__ */