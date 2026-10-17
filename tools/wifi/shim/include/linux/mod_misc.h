#ifndef __SHIM_LINUX_MISC_H__
#define __SHIM_LINUX_MISC_H__

/* <linux/init.h> */
#define __init
#define __exit
#define module_init(x)
#define module_exit(x)

/* <linux/module.h> */
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define MODULE_PARM_DESC(v, x)
#define module_param(name, type, perm)
#define EXPORT_SYMBOL(x)
#define module_init_macro(fn)

/* <linux/utsname.h> */
struct new_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};
#define utsname() ((struct new_utsname*)0)

#endif /* __SHIM_LINUX_MISC_H__ */