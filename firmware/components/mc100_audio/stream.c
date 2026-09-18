#include "audio_internal.h"

mc100_result_t mc100_stream_push(mc100_stream_t *stream, mc100_generation_t generation,
                                 const mc100_frame_t *frame)
{
    if (stream->stats.current == MC100_STREAM_FRAMES) {
        /* The owning Audio session stops after this first rejected frame. */
        ++stream->stats.drops;
        return MC100_FULL;
    }
    uint16_t tail = (uint16_t)((stream->head + stream->stats.current) % MC100_STREAM_FRAMES);
    stream->packets[tail].generation = generation;
    stream->packets[tail].frame = *frame;
    ++stream->stats.current;
    if (stream->stats.current > stream->stats.peak) stream->stats.peak = stream->stats.current;
    return MC100_OK;
}

mc100_result_t mc100_audio_pop(mc100_audio_t *a, mc100_packet_t *packet)
{
    if (!a || !packet) return MC100_INVALID;
    if (!a->stream.stats.current) return MC100_NOT_READY;
    *packet = a->stream.packets[a->stream.head];
    a->stream.head = (uint16_t)((a->stream.head + 1) % MC100_STREAM_FRAMES);
    --a->stream.stats.current;
    return MC100_OK;
}

void mc100_stream_discard(mc100_stream_t *stream, mc100_generation_t generation)
{
    /* Caller quiesces/serializes consumer before compaction. Forward compaction
     * preserves the order of every packet belonging to the other generation. */
    uint16_t kept = 0;
    for (uint16_t i = 0; i < stream->stats.current; ++i) {
        uint16_t source = (uint16_t)((stream->head + i) % MC100_STREAM_FRAMES);
        if (stream->packets[source].generation != generation) {
            uint16_t target = (uint16_t)((stream->head + kept) % MC100_STREAM_FRAMES);
            if (target != source) stream->packets[target] = stream->packets[source];
            ++kept;
        }
    }
    stream->stats.current = kept;
}

mc100_result_t mc100_audio_stats(mc100_audio_t *a, mc100_audio_stats_t *stats)
{
    if (!a || !stats) return MC100_INVALID;
    *stats = a->stream.stats;
    return MC100_OK;
}
