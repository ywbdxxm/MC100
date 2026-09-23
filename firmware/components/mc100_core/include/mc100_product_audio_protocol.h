#ifndef MC100_PRODUCT_AUDIO_PROTOCOL_H
#define MC100_PRODUCT_AUDIO_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t next_sequence;
    uint32_t pending_sequence;
    uint32_t acknowledged_sequence;
} mc100_product_audio_protocol_t;

static inline uint32_t mc100_product_audio_begin(
    mc100_product_audio_protocol_t *protocol)
{
    uint32_t sequence = ++protocol->next_sequence;
    if (sequence == 0) sequence = ++protocol->next_sequence;
    protocol->pending_sequence = sequence;
    return sequence;
}

static inline bool mc100_product_audio_ack_matches(
    mc100_product_audio_protocol_t *protocol, uint32_t sequence)
{
    return protocol->acknowledged_sequence == sequence &&
           protocol->pending_sequence == sequence;
}

static inline bool mc100_product_audio_shutdown_requires_quarantine(
    bool acknowledged)
{
    return !acknowledged;
}

#endif
