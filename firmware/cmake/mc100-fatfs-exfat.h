#ifndef MC100_FATFS_EXFAT_H
#define MC100_FATFS_EXFAT_H

/* Project-local overlay: read the locked SDK configuration once, then change
 * only exFAT. PUBLIC compiler inclusion applies this before ff.h in FatFs and
 * each consumer, so FIL/FATFS/FSIZE_t have the same ABI everywhere. */
#ifdef __cplusplus
extern "C" {
#endif
#include "ffconf.h"
#ifdef __cplusplus
}
#endif

#if FFCONF_DEF != 80386
#error "Review the MC100 exFAT overlay for this FatFs revision"
#endif
#if FF_FS_EXFAT != 0
#error "SDK exFAT default changed; review and remove or adapt the overlay"
#endif
#if FF_USE_LFN < 1 || FF_MAX_LFN < 128
#error "MC100 exFAT needs enabled long filenames of at least 128 characters"
#endif
#if FF_LBA64 != 0 || FF_USE_EXPAND != 0 || FF_USE_DYN_BUFFER != 0
#error "MC100 FatFs storage options changed; review the exFAT ABI contract"
#endif

#undef FF_FS_EXFAT
#define FF_FS_EXFAT 1

/* The SDK maps FF_USE_LABEL to a Kconfig boolean that is undefined when off.
 * exFAT dir_read evaluates it as C (not only #if), so preserve disabled label
 * support as an explicit zero without changing SDK files or enabling labels. */
#ifndef CONFIG_FATFS_USE_LABEL
#undef FF_USE_LABEL
#define FF_USE_LABEL 0
#endif

#endif
