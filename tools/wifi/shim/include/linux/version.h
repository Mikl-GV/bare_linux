#ifndef __SHIM_LINUX_VERSION_H__
#define __SHIM_LINUX_VERSION_H__

#define LINUX_VERSION_CODE 132640   /* 4.14.0 → (4<<16)|(14<<8)|0 */
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))

#endif /* __SHIM_LINUX_VERSION_H__ */