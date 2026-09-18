#include <string.h>

#include "mc100_format.h"
#include "format_bytes.h"

static bool record_valid(const mc100_index_record_t *r)
{
    if (r->type < MC100_INDEX_BLOCK || r->type > MC100_INDEX_INCIDENT ||
        r->generation == 0 || r->journal_seq >= MC100_INDEX_MAX_RECORDS ||
        r->pcm_offset > MC100_MAX_PCM_BYTES || (r->pcm_offset & 1u)) return false;

    if (r->type == MC100_INDEX_BLOCK) {
        return r->valid_bytes >= 2 && r->valid_bytes <= MC100_PCM_BLOCK_BYTES &&
               (r->valid_bytes & 1u) == 0 &&
               r->valid_bytes <= MC100_MAX_PCM_BYTES - r->pcm_offset &&
               r->first_source_sample <= UINT64_MAX - r->valid_bytes / 2 &&
               r->flags == 0 && r->detail == 0;
    }
    if (r->valid_bytes != 0 || r->payload_crc32 != 0) return false;
    if (r->type == MC100_INDEX_INCIDENT) {
        return r->flags == MC100_INDEX_INCOMPLETE &&
               r->detail >= MC100_INCIDENT_MIC_IO &&
               r->detail <= MC100_INCIDENT_LOW_BAT_INTERRUPTED;
    }
    return r->flags == 0 && r->detail == 0;
}

mc100_result_t mc100_index_encode(uint8_t out[64], const mc100_index_record_t *r)
{
    if (out == NULL || r == NULL || !record_valid(r)) return MC100_INVALID;
    memset(out, 0, MC100_INDEX_RECORD_BYTES);
    memcpy(out, "MCR1", 4);
    mc100_put_u16(out + 4, 1);
    mc100_put_u16(out + 6, r->type);
    mc100_put_u64(out + 8, r->journal_seq);
    mc100_put_u64(out + 16, r->pcm_offset);
    mc100_put_u64(out + 24, r->first_source_sample);
    mc100_put_u32(out + 32, r->valid_bytes);
    mc100_put_u32(out + 36, r->payload_crc32);
    mc100_put_u64(out + 40, r->generation);
    mc100_put_u32(out + 48, r->flags);
    mc100_put_u32(out + 52, r->detail);
    mc100_put_u32(out + 60, mc100_crc32(out, 60));
    return MC100_OK;
}

mc100_result_t mc100_index_decode(const uint8_t in[64], mc100_index_record_t *out)
{
    if (in == NULL || out == NULL) return MC100_INVALID;
    if (memcmp(in, "MCR1", 4) != 0 || mc100_get_u16(in + 4) != 1 ||
        mc100_get_u32(in + 56) != 0 ||
        mc100_get_u32(in + 60) != mc100_crc32(in, 60)) return MC100_CORRUPT;
    mc100_index_record_t r = {
        .type = mc100_get_u16(in + 6),
        .journal_seq = mc100_get_u64(in + 8),
        .pcm_offset = mc100_get_u64(in + 16),
        .first_source_sample = mc100_get_u64(in + 24),
        .valid_bytes = mc100_get_u32(in + 32),
        .payload_crc32 = mc100_get_u32(in + 36),
        .generation = mc100_get_u64(in + 40),
        .flags = mc100_get_u32(in + 48),
        .detail = mc100_get_u32(in + 52)
    };
    if (!record_valid(&r)) return MC100_CORRUPT;
    *out = r;
    return MC100_OK;
}

static bool header_valid(const mc100_index_header_t *h)
{
    if (h->flags == MC100_INDEX_RESERVED) {
        return h->generation == 0 && h->segment_index == 0 && h->first_source_sample == 0;
    }
    return h->flags == MC100_INDEX_CLAIMED && h->generation != 0 &&
           h->first_source_sample <= UINT64_MAX - MC100_MAX_PCM_BYTES / 2;
}

mc100_result_t mc100_index_validation_init(mc100_index_validation_t *out,
                                            const mc100_index_header_t *h)
{
    if (out == NULL || h == NULL) return MC100_INVALID;
    if (!header_valid(h) || h->flags != MC100_INDEX_CLAIMED) return MC100_CORRUPT;

    mc100_index_validation_t validation;
    memset(&validation, 0, sizeof(validation));
    validation.generation = h->generation;
    validation.first_source_sample = h->first_source_sample;
    *out = validation;
    return MC100_OK;
}

mc100_result_t mc100_index_validation_accept(mc100_index_validation_t *validation,
                                              const mc100_index_record_t *r)
{
    if (validation == NULL || r == NULL) return MC100_INVALID;
    if (!record_valid(r) || validation->terminal ||
        validation->record_count >= MC100_INDEX_MAX_RECORDS ||
        r->journal_seq != validation->record_count ||
        r->generation != validation->generation ||
        r->pcm_offset != validation->pcm_bytes ||
        r->pcm_offset / 2 > UINT64_MAX - validation->first_source_sample ||
        r->first_source_sample != validation->first_source_sample + r->pcm_offset / 2) {
        return MC100_CORRUPT;
    }

    mc100_index_validation_t next = *validation;
    if (r->type == MC100_INDEX_BLOCK) {
        if (r->valid_bytes > MC100_MAX_PCM_BYTES - next.pcm_bytes) return MC100_CORRUPT;
        next.pcm_bytes += r->valid_bytes;
    } else if (r->type == MC100_INDEX_FINAL || r->type == MC100_INDEX_INCIDENT) {
        next.terminal = true;
    }
    ++next.record_count;
    *validation = next;
    return MC100_OK;
}

mc100_result_t mc100_index_header_encode(uint8_t out[512], const mc100_index_header_t *h)
{
    if (out == NULL || h == NULL || !header_valid(h)) return MC100_INVALID;
    memset(out, 0, MC100_INDEX_HEADER_BYTES);
    memcpy(out, "MC100IDX", 8);
    mc100_put_u16(out + 8, 1);
    mc100_put_u16(out + 10, MC100_INDEX_HEADER_BYTES);
    mc100_put_u32(out + 12, h->flags);
    memcpy(out + 16, h->boot_id, 16);
    mc100_put_u64(out + 32, h->generation);
    mc100_put_u32(out + 40, h->segment_index);
    mc100_put_u32(out + 44, MC100_SAMPLE_RATE);
    mc100_put_u16(out + 48, 16);
    mc100_put_u16(out + 50, 1);
    mc100_put_u16(out + 52, MC100_FRAME_SAMPLES);
    mc100_put_u64(out + 56, h->first_source_sample);
    mc100_put_u32(out + 64, MC100_MAX_PCM_BYTES);
    mc100_put_u16(out + 68, MC100_INDEX_RECORD_BYTES);
    mc100_put_u32(out + 72, MC100_INDEX_MAX_RECORDS);
    mc100_put_u32(out + 508, mc100_crc32(out, 508));
    return MC100_OK;
}

mc100_result_t mc100_index_header_decode(const uint8_t in[512], mc100_index_header_t *out)
{
    if (in == NULL || out == NULL) return MC100_INVALID;
    if (memcmp(in, "MC100IDX", 8) != 0 || mc100_get_u16(in + 8) != 1 ||
        mc100_get_u16(in + 10) != MC100_INDEX_HEADER_BYTES ||
        mc100_get_u32(in + 44) != MC100_SAMPLE_RATE || mc100_get_u16(in + 48) != 16 ||
        mc100_get_u16(in + 50) != 1 || mc100_get_u16(in + 52) != MC100_FRAME_SAMPLES ||
        mc100_get_u16(in + 54) != 0 || mc100_get_u32(in + 64) != MC100_MAX_PCM_BYTES ||
        mc100_get_u16(in + 68) != MC100_INDEX_RECORD_BYTES || mc100_get_u16(in + 70) != 0 ||
        mc100_get_u32(in + 72) != MC100_INDEX_MAX_RECORDS ||
        mc100_get_u32(in + 508) != mc100_crc32(in, 508)) return MC100_CORRUPT;
    for (size_t i = 76; i < 508; ++i) if (in[i] != 0) return MC100_CORRUPT;
    mc100_index_header_t h = {
        .flags = mc100_get_u32(in + 12),
        .generation = mc100_get_u64(in + 32),
        .segment_index = mc100_get_u32(in + 40),
        .first_source_sample = mc100_get_u64(in + 56)
    };
    memcpy(h.boot_id, in + 16, 16);
    if (!header_valid(&h)) return MC100_CORRUPT;
    *out = h;
    return MC100_OK;
}
