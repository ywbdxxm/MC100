#include "mc100_platform.h"
#include "mc100_driver_config.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <limits.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if FF_USE_LFN != 2 || FF_MAX_LFN < 128 || FF_FS_LOCK < 9
#error "MC100 requires stack LFN >=128 and at least 9 FatFs lock slots"
#endif
#if FF_USE_DYN_BUFFER || FF_FS_TINY
#error "MC100 bounded handles require static per-file FatFs buffers"
#endif
#if !FF_FS_EXFAT
#error "MC100 requires the project-local FAT32/exFAT configuration"
#endif
_Static_assert(sizeof(FSIZE_t) == 8, "FatFs ABI must include exFAT for every consumer");

#define MOUNT_POINT "/sdcard"
struct mc100_file { FIL fil; bool used, dirty, writable; };
static struct {
    sdmmc_card_t *card;
    TaskHandle_t owner;
    char drive[4];
    bool poisoned;
    struct mc100_file files[MC100_DRIVER_FILE_HANDLES];
} storage;

static mc100_result_t result(FRESULT fr)
{
    switch (fr) {
    case FR_OK: return MC100_OK;
    case FR_NO_FILE: case FR_NO_PATH: case FR_EXIST: case FR_NOT_READY: return MC100_NOT_READY;
    case FR_DENIED: case FR_TOO_MANY_OPEN_FILES: case FR_NOT_ENOUGH_CORE: return MC100_FULL;
    case FR_TIMEOUT: return MC100_TIMEOUT;
    case FR_INVALID_NAME: case FR_INVALID_PARAMETER: case FR_INVALID_OBJECT: return MC100_INVALID;
    default: return MC100_IO;
    }
}
static bool owned(void) { return storage.card && storage.owner == xTaskGetCurrentTaskHandle(); }
static bool ready(void *ctx) { return ctx == &storage && owned() && !storage.poisoned; }
static struct mc100_file *file_handle(void *ctx, mc100_file_t handle)
{
    if (ctx != &storage || !owned()) return NULL;
    for (size_t i = 0; i < MC100_DRIVER_FILE_HANDLES; ++i)
        if (handle == &storage.files[i] && handle->used) return handle;
    return NULL;
}
static bool path_make(const char *name, char out[MC100_PATH_BYTES + 4])
{
    if (!mc100_path_valid(name)) return false;
    int length = snprintf(out, MC100_PATH_BYTES + 4, "%s/%s", storage.drive, name);
    return length > 0 && length < MC100_PATH_BYTES + 4;
}
static mc100_result_t open_file(void *ctx, const char *name, mc100_file_t *out, BYTE mode)
{
    if (out) *out = NULL;
    if (!out) return MC100_INVALID;
    if (!ready(ctx)) return MC100_NOT_READY;
    char path[MC100_PATH_BYTES + 4];
    if (!path_make(name, path)) return MC100_INVALID;
    for (size_t i = 0; i < MC100_DRIVER_FILE_HANDLES; ++i) {
        struct mc100_file *f = &storage.files[i];
        if (f->used) continue;
        FRESULT fr = f_open(&f->fil, path, mode);
        if (fr != FR_OK) return result(fr);
        f->used = true;
        f->dirty = (mode & FA_CREATE_NEW) != 0;
        f->writable = (mode & FA_WRITE) != 0;
        *out = f;
        return MC100_OK;
    }
    return MC100_FULL;
}
static mc100_result_t open_exclusive(void *ctx, const char *name, mc100_file_t *out)
{ return open_file(ctx, name, out, FA_READ | FA_WRITE | FA_CREATE_NEW); }
static mc100_result_t open_read(void *ctx, const char *name, mc100_file_t *out)
{ return open_file(ctx, name, out, FA_READ | FA_OPEN_EXISTING); }
static mc100_result_t open_update(void *ctx, const char *name, mc100_file_t *out)
{ return open_file(ctx, name, out, FA_READ | FA_WRITE | FA_OPEN_EXISTING); }

static mc100_result_t seek_exact(struct mc100_file *f, uint64_t offset)
{
    if (offset > UINT32_MAX) return MC100_INVALID; /* MC100 files remain bounded, including on exFAT. */
    FRESULT fr = f_lseek(&f->fil, (FSIZE_t)offset);
    if (fr != FR_OK) return result(fr);
    return f_tell(&f->fil) == offset ? MC100_OK : MC100_FULL;
}
static mc100_result_t read_at(void *ctx, mc100_file_t handle, uint64_t offset,
                              void *data, size_t length, size_t *actual)
{
    if (actual) *actual = 0;
    if (!actual || (!data && length) || length > UINT_MAX) return MC100_INVALID;
    if (!ready(ctx)) return MC100_NOT_READY;
    struct mc100_file *f = file_handle(ctx, handle);
    if (!f) return MC100_INVALID;
    if (offset > f_size(&f->fil)) return MC100_OK; /* EOF, never extend via read. */
    mc100_result_t r = seek_exact(f, offset);
    if (r != MC100_OK) return r;
    UINT count = 0;
    FRESULT fr = f_read(&f->fil, data, (UINT)length, &count);
    *actual = count;
    return result(fr);
}
static mc100_result_t write_at(void *ctx, mc100_file_t handle, uint64_t offset,
                               const void *data, size_t length, size_t *actual)
{
    if (actual) *actual = 0;
    if (!actual || (!data && length) || length > UINT_MAX || offset > UINT32_MAX ||
        length > UINT32_MAX - offset) return MC100_INVALID;
    if (!ready(ctx)) return MC100_NOT_READY;
    struct mc100_file *f = file_handle(ctx, handle);
    if (!f || !f->writable) return MC100_INVALID;
    f->dirty = true; /* Seek may allocate, including on a short/failing operation. */
    mc100_result_t r = seek_exact(f, offset);
    if (r != MC100_OK) return r;
    UINT count = 0;
    FRESULT fr = f_write(&f->fil, data, (UINT)length, &count);
    *actual = count;
    return fr != FR_OK ? result(fr) : count == length ? MC100_OK : MC100_FULL;
}
static mc100_result_t allocate(void *ctx, mc100_file_t handle, uint64_t requested, uint64_t *actual)
{
    if (actual) *actual = 0;
    if (!actual || requested > UINT32_MAX) return MC100_INVALID;
    if (!ready(ctx)) return MC100_NOT_READY;
    struct mc100_file *f = file_handle(ctx, handle);
    if (!f || !f->writable) return MC100_INVALID;
    f->dirty = true;
    mc100_result_t r = seek_exact(f, requested);
    *actual = f_size(&f->fil);
    if (r != MC100_OK) return r;
    return *actual == requested ? MC100_OK : MC100_INVALID;
}
static mc100_result_t sync_file(void *ctx, mc100_file_t handle)
{
    if (!ready(ctx)) return MC100_NOT_READY;
    struct mc100_file *f = file_handle(ctx, handle);
    if (!f) return MC100_INVALID;
    FRESULT fr = f_sync(&f->fil);
    if (fr == FR_OK) f->dirty = false;
    return result(fr);
}
static mc100_result_t truncate_file(void *ctx, mc100_file_t handle, uint64_t length)
{
    if (!ready(ctx)) return MC100_NOT_READY;
    struct mc100_file *f = file_handle(ctx, handle);
    if (!f || !f->writable || length > f_size(&f->fil)) return MC100_INVALID;
    f->dirty = true;
    mc100_result_t r = seek_exact(f, length);
    return r == MC100_OK ? result(f_truncate(&f->fil)) : r;
}
static mc100_result_t close_file(void *ctx, mc100_file_t handle)
{
    struct mc100_file *f = file_handle(ctx, handle);
    if (!f) return MC100_INVALID;
    mc100_result_t r = MC100_OK;
    if (f->dirty || storage.poisoned) {
        /* f_close would call f_sync. Discard this handle and forbid further
         * filesystem operations until owner unmount discards cache/lock state.
         * Static FIL buffers own no allocation; no SDK-private flags touched. */
        storage.poisoned = true;
    } else {
        r = result(f_close(&f->fil)); /* Already synced/read-only: no write. */
        if (r != MC100_OK) storage.poisoned = true;
    }
    memset(f, 0, sizeof(*f));
    return r;
}
static mc100_result_t rename_no_replace(void *ctx, const char *old_name, const char *new_name)
{
    if (!ready(ctx)) return MC100_NOT_READY;
    char old_path[MC100_PATH_BYTES + 4], new_path[MC100_PATH_BYTES + 4];
    if (!path_make(old_name, old_path) || !path_make(new_name, new_path)) return MC100_INVALID;
    FILINFO info;
    FRESULT fr = f_stat(new_path, &info);
    if (fr == FR_OK) return MC100_NOT_READY;
    if (fr != FR_NO_FILE) return result(fr);
    /* Single owner prevents check/rename races; f_rename itself refuses an
     * existing destination and calls sync_fs before returning FR_OK. */
    return result(f_rename(old_path, new_path));
}
static mc100_result_t stat_file(void *ctx, const char *name, uint64_t *size)
{
    if (!size) return MC100_INVALID;
    *size = 0;
    if (!ready(ctx)) return MC100_NOT_READY;
    char path[MC100_PATH_BYTES + 4];
    if (!path_make(name, path)) return MC100_INVALID;
    FILINFO info;
    FRESULT fr = f_stat(path, &info);
    if (fr != FR_OK) return result(fr);
    if (info.fattrib & AM_DIR) return MC100_INVALID;
    *size = info.fsize;
    return MC100_OK;
}
static mc100_result_t space(void *ctx, uint64_t *total, uint64_t *free_bytes)
{
    if (!total || !free_bytes) return MC100_INVALID;
    *total = *free_bytes = 0;
    if (!ready(ctx)) return MC100_NOT_READY;
    DWORD free_clusters;
    FATFS *fs;
    FRESULT fr = f_getfree(storage.drive, &free_clusters, &fs);
    if (fr != FR_OK) return result(fr);
    uint64_t cluster_bytes = (uint64_t)fs->csize * storage.card->csd.sector_size;
    *total = (uint64_t)(fs->n_fatent - 2) * cluster_bytes;
    *free_bytes = (uint64_t)free_clusters * cluster_bytes;
    return MC100_OK;
}
static mc100_result_t list(void *ctx, mc100_io_visit_fn visit, void *arg)
{
    if (!visit) return MC100_INVALID;
    if (!ready(ctx)) return MC100_NOT_READY;
    FF_DIR directory;
    FRESULT fr = f_opendir(&directory, storage.drive);
    if (fr != FR_OK) return result(fr);
    mc100_result_t r = MC100_OK;
    FILINFO info;
    for (;;) {
        fr = f_readdir(&directory, &info);
        if (fr != FR_OK) { r = result(fr); break; }
        if (!info.fname[0]) break;
        if (!(info.fattrib & AM_DIR) && mc100_path_valid(info.fname)) {
            r = visit(arg, info.fname);
            if (r != MC100_OK) break;
            if (!ready(ctx)) { r = MC100_NOT_READY; break; }
        }
    }
    mc100_result_t closed = result(f_closedir(&directory));
    return r != MC100_OK ? r : closed;
}
static const mc100_io_t io = {
    .open_exclusive = open_exclusive, .open_read = open_read, .open_update = open_update,
    .read_at = read_at, .write_at = write_at, .allocate = allocate, .sync = sync_file,
    .truncate = truncate_file, .close = close_file, .rename_no_replace = rename_no_replace,
    .stat = stat_file, .space = space, .list = list
};
const mc100_io_t *mc100_platform_storage_io(void) { return &io; }
void *mc100_platform_storage_context(void) { return &storage; }

/* Failed-mount diagnostics only: initialize the same card and read at most
 * sector 0 plus four primary partition boot sectors. Never writes or mounts. */
static void diagnose_mount(const sdmmc_host_t *host, const sdmmc_slot_config_t *slot)
{
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) { printf("SD_PROBE host=%d\n", (int)err); return; }
    sdmmc_card_t card = {0};
    uint8_t *sector = heap_caps_malloc(512, MALLOC_CAP_DMA);
    err = sector ? sdmmc_host_init_slot(host->slot, slot) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = sdmmc_card_init(host, &card);
    printf("SD_PROBE init=%d sectors=%lu sector_bytes=%u\n", (int)err,
           (unsigned long)card.csd.capacity, (unsigned)card.csd.sector_size);
    if (err == ESP_OK && card.csd.sector_size == 512) {
        err = sdmmc_read_sectors(&card, sector, 0, 1);
        if (err == ESP_OK) {
            uint32_t starts[4] = {0};
            printf("SD_PROBE lba=0 signature=%02x%02x exfat_label=%u fat32_label=%u\n",
                   sector[510], sector[511], (unsigned)(memcmp(sector + 3, "EXFAT   ", 8) == 0),
                   (unsigned)(memcmp(sector + 82, "FAT32   ", 8) == 0));
            if (sector[510] == 0x55 && sector[511] == 0xaa) {
                for (unsigned i = 0; i < 4; ++i) {
                    const uint8_t *entry = sector + 446 + 16 * i;
                    starts[i] = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8) |
                                ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
                    printf("SD_PROBE partition=%u type=%02x start=%lu\n", i,
                           entry[4], (unsigned long)starts[i]);
                    if (!entry[4]) starts[i] = 0;
                }
                for (unsigned i = 0; i < 4; ++i) {
                    if (!starts[i] || starts[i] >= card.csd.capacity) continue;
                    esp_err_t read = sdmmc_read_sectors(&card, sector, starts[i], 1);
                    printf("SD_PROBE lba=%lu read=%d", (unsigned long)starts[i], (int)read);
                    if (read == ESP_OK)
                        printf(" signature=%02x%02x exfat_label=%u fat32_label=%u",
                               sector[510], sector[511], (unsigned)(memcmp(sector + 3, "EXFAT   ", 8) == 0),
                               (unsigned)(memcmp(sector + 82, "FAT32   ", 8) == 0));
                    putchar('\n');
                }
            }
        } else printf("SD_PROBE read0=%d\n", (int)err);
    }
    free(sector);
    printf("SD_PROBE deinit=%d readonly=1\n", (int)sdmmc_host_deinit());
}

mc100_result_t mc100_platform_storage_mount(void)
{
    if (storage.card) return MC100_NOT_READY;
    storage.owner = xTaskGetCurrentTaskHandle();
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = MC100_DRIVER_SD_KHZ;
    host.command_timeout_ms = 1000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = MC100_DRIVER_SD_WIDTH;
    slot.clk = MC100_GPIO_SD_CLK;
    slot.cmd = MC100_GPIO_SD_CMD;
    slot.d0 = MC100_GPIO_SD_D0;
    /* CD polarity is unverified: Monitor handles it; do not gate controller. */
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false, .max_files = MC100_DRIVER_FILE_HANDLES,
        .allocation_unit_size = 0
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &config, &storage.card);
    if (err != ESP_OK) {
        storage.owner = NULL; storage.card = NULL;
        diagnose_mount(&host, &slot);
        return MC100_IO;
    }
    BYTE drive = ff_diskio_get_pdrv_card(storage.card);
    if (drive > 9) { (void)mc100_platform_storage_unmount(); return MC100_IO; }
    (void)snprintf(storage.drive, sizeof(storage.drive), "%u:", (unsigned)drive);
    storage.poisoned = false;
    FATFS *fs;
    DWORD available;
    FRESULT fr = f_getfree(storage.drive, &available, &fs);
    if (fr != FR_OK || (fs->fs_type != FS_FAT32 && fs->fs_type != FS_EXFAT)) {
        (void)mc100_platform_storage_unmount(); return MC100_CORRUPT;
    }
    uint64_t cluster_bytes = (uint64_t)fs->csize * storage.card->csd.sector_size;
    printf("SD_MOUNT filesystem=%s total=%" PRIu64 " free=%" PRIu64 " readonly_probe=1\n",
           fs->fs_type == FS_EXFAT ? "exFAT" : "FAT32",
           (uint64_t)(fs->n_fatent - 2) * cluster_bytes, (uint64_t)available * cluster_bytes);
    return MC100_OK;
}
mc100_result_t mc100_platform_storage_unmount(void)
{
    if (!storage.card) return MC100_OK;
    if (!owned()) return MC100_INVALID;
    for (size_t i = 0; i < MC100_DRIVER_FILE_HANDLES; ++i)
        if (storage.files[i].used) return MC100_NOT_READY;
    /* SDK f_mount(NULL) detaches and clears lock/cache state without sync. */
    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, storage.card);
    if (err != ESP_OK) { storage.poisoned = true; return MC100_IO; }
    memset(&storage, 0, sizeof(storage));
    return MC100_OK;
}
