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
    unsigned close_calls;
    bool readiness_order_ok;
    bool fail_close_once;
    mc100_result_t fail_close_result;
    bool generate_pcm;
    uint64_t pcm_seq;
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
    ++fake->close_calls;
    if (fake->fail_close_once) {
        fake->fail_close_once = false;
        return fake->fail_close_result ? fake->fail_close_result : MC100_IO;
    }
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
    if (fake->recovery_called != 0 || mc100_fake_io_log_count(fake->storage) != 0)
        fake->readiness_order_ok = false;
    return fake->battery_is_ready;
}

static bool sup_fake_driver_ready(void *ctx)
{
    sup_fake_t *fake = ctx;
    ++fake->driver_calls;
    if (fake->battery_calls != 1 || fake->recovery_called != 1 ||
        mc100_fake_io_log_count(fake->storage) < 2)
        fake->readiness_order_ok = false;
    return fake->driver_is_ready;
}

static mc100_result_t sup_fake_pcm_read(void *ctx, uint8_t *buffer, size_t cap,
                                         size_t *count, uint32_t timeout_ms)
{
    sup_fake_t *fake = ctx;
    (void)timeout_ms;
    if (!buffer || !count || cap < MC100_FRAME_SAMPLES * sizeof(int16_t))
        return MC100_INVALID;
    if (!fake->generate_pcm) {
        *count = 0;
        return MC100_OK;
    }
    for (size_t i = 0; i < MC100_FRAME_SAMPLES; ++i) {
        int16_t sample = (int16_t)fake->pcm_seq;
        buffer[i * 2] = (uint8_t)sample;
        buffer[i * 2 + 1] = (uint8_t)((uint16_t)sample >> 8);
    }
    ++fake->pcm_seq;
    *count = MC100_FRAME_SAMPLES * sizeof(int16_t);
    return MC100_OK;
}

static void sup_fake_set_vad_trigger(sup_fake_t *fake, uint64_t sequence)
{
    fake->generate_pcm = true;
    fake->pcm_seq = 0;
    fake->vad_state = (mc100_vad_fixed_t){0};
    fake->vad_state.trigger_seq = sequence;
}

static const uint8_t *sup_fake_active_wav(const sup_fake_t *fake, size_t *size)
{
    for (size_t i = 0; i < mc100_fake_io_count(fake->storage); ++i) {
        const char *path = mc100_fake_io_path(fake->storage, i);
        if (path && strstr(path + 33, "reserve_") == NULL &&
            strstr(path, ".wav.part") != NULL)
            return mc100_fake_io_bytes(fake->storage, path, size);
    }
    if (size)
        *size = 0;
    return NULL;
}

static uint16_t sup_fake_u16le(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint64_t sup_fake_written_frames(const sup_fake_t *fake)
{
    /* Count committed PCM writes rather than scanning the preallocated file.
     * The writer may retain a tail in its 32 KiB staging buffer, so an
     * allocation-sentinel scan would under-count a valid preroll until the
     * next checkpoint.  Header writes are at offset zero; every PCM flush is
     * an exact write at or after the 512-byte WAV header. */
    uint64_t bytes = 0;
    for (size_t i = 0; i < mc100_fake_io_log_count(fake->storage); ++i) {
        const mc100_fake_op_t *op = mc100_fake_io_log(fake->storage, i);
        if (op != NULL && op->kind == 'W' && op->offset >= 512 &&
            strstr(op->path, ".wav.part") != NULL)
            bytes += op->length;
    }
    return bytes / 640;
}

static void sup_fake_assert_preroll(const sup_fake_t *fake,
                                    const mc100_writer_status_t *status,
                                    uint64_t first_sequence,
                                    uint64_t frame_count)
{
    assert(status != NULL);
    assert(status->active);
    assert(status->accepted_bytes == frame_count * 640u);
    assert(status->last_seq == first_sequence + frame_count - 1u);
    size_t size = 0;
    const uint8_t *bytes = sup_fake_active_wav(fake, &size);
    assert(bytes != NULL);
    assert(size >= 512 + 640);
    assert(sup_fake_u16le(bytes + 512) == (uint16_t)first_sequence);
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
    fake->fail_close_result = MC100_IO;
    fake->vad_state.trigger_seq = UINT64_MAX;
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
        .vad = mc100_vad_fixed(&fake->vad_state, fake->vad_state.trigger_seq),
        .upload = mc100_upload_noop(),
        .boot_id = fake->boot_id,
    };
    return deps;
}

#endif
