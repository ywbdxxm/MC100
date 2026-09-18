#include "ff.h"
#if FF_FS_EXFAT != 1
#error "MC100 exFAT configuration must reach every FatFs consumer"
#endif
#if FF_USE_LFN < 1
#error "MC100 exFAT requires long filenames"
#endif
#ifdef __cplusplus
static_assert(sizeof(FSIZE_t) == 8, "MC100 FatFs file size ABI must be 64 bit");
#else
_Static_assert(sizeof(FSIZE_t) == 8, "MC100 FatFs file size ABI must be 64 bit");
#endif
/* Compiled into FatFs itself as well as exercised as a standalone consumer. */
void mc100_fatfs_exfat_abi_probe(void) {}
