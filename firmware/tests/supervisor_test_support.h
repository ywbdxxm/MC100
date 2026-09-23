#ifndef MC100_SUPERVISOR_TEST_SUPPORT_H
#define MC100_SUPERVISOR_TEST_SUPPORT_H

#include "mc100_fake_io.h"
#include "mc100_supervisor.h"
#include "mc100_upload.h"
#include "mc100_vad.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

typedef struct {
    mc100_fake_io_t *storage;
    mc100_io_t io;
    mc100_vad_fixed_t vad_state;
    uint8_t boot_id[16];
    uint64_t now;
    uint64_t advance_on_list_ms;
    unsigned recovery_called;
    bool battery_is_ready;
    bool driver_is_ready;
    unsigned battery_calls;
    unsigned driver_calls;
    bool readiness_order_ok;
} sup_fake_t;

static mc100_result_t sup_fake_open_exclusive(void *ctx, const char *path,
                                               mc100_file_t *file)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->open_exclusive(fake->storage, path, file);
}

static mc100_result_t sup_fake_open_read(void *ctx, const char *path,
                                          mc100_file_t *file)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->open_read(fake->storage, path, file);
}

static mc100_result_t sup_fake_open_update(void *ctx, const char *path,
                                            mc100_file_t *file)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->open_update(fake->storage, path, file);
}

static mc100_result_t sup_fake_read_at(void *ctx, mc100_file_t file,
                                        uint64_t offset, void *data, size_t size,
                                        size_t *actual)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->read_at(fake->storage, file, offset, data, size,
                                         actual);
}

static mc100_result_t sup_fake_write_at(void *ctx, mc100_file_t file,
                                         uint64_t offset, const void *data,
                                         size_t size, size_t *actual)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->write_at(fake->storage, file, offset, data, size,
                                          actual);
}

static mc100_result_t sup_fake_allocate(void *ctx, mc100_file_t file,
                                         uint64_t size, uint64_t *actual)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->allocate(fake->storage, file, size, actual);
}

static mc100_result_t sup_fake_sync(void *ctx, mc100_file_t file)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->sync(fake->storage, file);
}

static mc100_result_t sup_fake_truncate(void *ctx, mc100_file_t file,
                                         uint64_t size)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->truncate(fake->storage, file, size);
}

static mc100_result_t sup_fake_close(void *ctx, mc100_file_t file)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->close(fake->storage, file);
}

static mc100_result_t sup_fake_rename(void *ctx, const char *from,
                                       const char *to)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->rename_no_replace(fake->storage, from, to);
}

static mc100_result_t sup_fake_stat(void *ctx, const char *path, uint64_t *size)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->stat(fake->storage, path, size);
}

static mc100_result_t sup_fake_space(void *ctx, uint64_t *total,
                                      uint64_t *free_bytes)
{
    sup_fake_t *fake = ctx;
    return mc100_fake_io_ops()->space(fake->storage, total, free_bytes);
}

static mc100_result_t sup_fake_list(void *ctx, mc100_io_visit_fn visit,
                                    void *visit_ctx)
{
    sup_fake_t *fake = ctx;
    if (fake->recovery_called == 0)
        ++fake->recovery_called;
    mc100_result_t result = mc100_fake_io_ops()->list(fake->storage, visit,
                                                       visit_ctx);
    fake->now += fake->advance_on_list_ms;
    return result;
}

static uint64_t sup_fake_now(void *ctx)
{
    return ((sup_fake_t *)ctx)->now;
}

static bool sup_fake_battery_ready(void *ctx)
{
    sup_fake_t *fake = ctx;
    ++fake->battery_calls;
    if (fake->recovery_called != 1 || mc100_fake_io_log_count(fake->storage) < 2)
        fake->readiness_order_ok = false;
    return fake->battery_is_ready;
}

static bool sup_fake_driver_ready(void *ctx)
{
    sup_fake_t *fake = ctx;
    ++fake->driver_calls;
    if (fake->battery_calls != 1)
        fake->readiness_order_ok = false;
    return fake->driver_is_ready;
}

static mc100_result_t sup_fake_pcm_read(void *ctx, uint8_t *buffer, size_t cap,
                                         size_t *count, uint32_t timeout_ms)
{
    (void)ctx;
    (void)buffer;
    (void)cap;
    (void)timeout_ms;
    *count = 0;
    return MC100_OK;
}

static void sup_fake_init(sup_fake_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->storage = mc100_fake_io_create(UINT64_C(67108864));
    assert(fake->storage != NULL);
    for (size_t i = 0; i < sizeof(fake->boot_id); ++i)
        fake->boot_id[i] = (uint8_t)i;
    fake->battery_is_ready = true;
    fake->driver_is_ready = true;
    fake->readiness_order_ok = true;
    fake->io = (mc100_io_t){
        sup_fake_open_exclusive, sup_fake_open_read, sup_fake_open_update,
        sup_fake_read_at, sup_fake_write_at, sup_fake_allocate, sup_fake_sync,
        sup_fake_truncate, sup_fake_close, sup_fake_rename, sup_fake_stat,
        sup_fake_space, sup_fake_list
    };
}

static void sup_fake_destroy(sup_fake_t *fake)
{
    mc100_fake_io_destroy(fake->storage);
    fake->storage = NULL;
}

static mc100_supervisor_deps_t sup_fake_deps(sup_fake_t *fake)
{
    mc100_supervisor_deps_t deps = {
        .io = &fake->io,
        .io_ctx = fake,
        .now_ms = sup_fake_now,
        .clock_ctx = fake,
        .battery_ready = sup_fake_battery_ready,
        .battery_ctx = fake,
        .driver_ready = sup_fake_driver_ready,
        .driver_ctx = fake,
        .pcm_read = sup_fake_pcm_read,
        .pcm_ctx = fake,
        .vad = mc100_vad_fixed(&fake->vad_state, UINT64_MAX),
        .upload = mc100_upload_noop(),
        .boot_id = fake->boot_id,
    };
    return deps;
}

#endif
