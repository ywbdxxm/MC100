/* Optional target compile/link probe. This never mounts or accesses a device.
 * Link with the project's FatFs component; caller supplies an already-owned FIL
 * only if explicitly running the probe on a disposable test filesystem. */
#include "ff.h"
#include <stdint.h>

FRESULT mc100_probe_fatfs_allocate(FIL *file, uint32_t requested, FSIZE_t *actual)
{
    if (!file || !actual) return FR_INVALID_PARAMETER;
    FRESULT result = f_lseek(file, requested);
    *actual = f_size(file);
    if (result != FR_OK) return result;
    if (f_tell(file) != requested || *actual != requested) return FR_DENIED;
    return f_sync(file);
}
