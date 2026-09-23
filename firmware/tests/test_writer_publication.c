#include "mc100_fake_io.h"
#include "mc100_writer.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void append_frame(mc100_writer_t *writer, mc100_generation_t generation,
                         uint64_t sequence)
{
    mc100_packet_t packet = {0};
    packet.generation = generation;
    packet.frame.seq = sequence;
    assert(mc100_writer_append(writer, &packet) == MC100_OK);
}

static void rotation_and_close_publish_writer_owned_names(void)
{
    static const char expected_segment_zero[] =
        "03030303030303030303030303030303_7_0.wav";
    static const char expected_segment_one[] =
        "03030303030303030303030303030303_7_1.wav";
    const mc100_generation_t generation = 7;
    const uint64_t first_sequence = 100;
    uint8_t boot_id[16];
    memset(boot_id, 3, sizeof(boot_id));

    mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(1000000000));
    assert(fake != NULL);
    mc100_writer_t *writer =
        mc100_writer_create(mc100_fake_io_ops(), fake, boot_id);
    assert(writer != NULL);
    assert(mc100_writer_prepare(writer) == MC100_OK);
    assert(mc100_writer_begin(writer, generation, first_sequence) == MC100_OK);

    /* The packet after a full segment publishes segment zero while opening
     * segment one.  The caller must be able to consume that record without
     * reconstructing the writer's filename. */
    for (uint64_t i = 0; i < MC100_SEGMENT_FRAMES + 1; ++i)
        append_frame(writer, generation, first_sequence + i);

    mc100_writer_publication_t publication = {0};
    assert(mc100_writer_publication_pop(writer, &publication) == MC100_OK);
    assert(publication.generation == generation);
    assert(publication.segment_index == 0);
    assert(strcmp(publication.name, expected_segment_zero) == 0);
    assert(mc100_writer_publication_pop(writer, &publication) == MC100_NOT_READY);

    assert(mc100_writer_close_through(writer, generation,
                                      first_sequence + MC100_SEGMENT_FRAMES,
                                      0) == MC100_OK);
    assert(mc100_writer_publication_pop(writer, &publication) == MC100_OK);
    assert(publication.generation == generation);
    assert(publication.segment_index == 1);
    assert(strcmp(publication.name, expected_segment_one) == 0);
    assert(mc100_writer_publication_pop(writer, &publication) == MC100_NOT_READY);

    mc100_writer_destroy(writer);
    mc100_fake_io_destroy(fake);
}

static void publication_queue_is_bounded_and_fifo(void)
{
    uint8_t boot_id[16];
    memset(boot_id, 4, sizeof(boot_id));
    mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(1000000000));
    assert(fake != NULL);
    mc100_writer_t *writer =
        mc100_writer_create(mc100_fake_io_ops(), fake, boot_id);
    assert(writer != NULL);
    assert(mc100_writer_prepare(writer) == MC100_OK);

    for (mc100_generation_t generation = 1;
         generation <= MC100_WRITER_PUBLICATION_CAPACITY; ++generation) {
        assert(mc100_writer_begin(writer, generation, 0) == MC100_OK);
        append_frame(writer, generation, 0);
        assert(mc100_writer_close_through(writer, generation, 0, 0) ==
               MC100_OK);
    }

    const mc100_generation_t overflow_generation =
        MC100_WRITER_PUBLICATION_CAPACITY + 1;
    assert(mc100_writer_begin(writer, overflow_generation, 0) == MC100_OK);
    append_frame(writer, overflow_generation, 0);
    size_t before_close = mc100_fake_io_log_count(fake);
    assert(mc100_writer_close_through(writer, overflow_generation, 0, 0) ==
           MC100_FULL);
    /* Backpressure is reported before finalization I/O or either rename; no
     * durable file can exist without its publication record. */
    assert(mc100_fake_io_log_count(fake) == before_close);

    mc100_writer_publication_t publication = {0};
    for (mc100_generation_t generation = 1;
         generation <= MC100_WRITER_PUBLICATION_CAPACITY; ++generation) {
        assert(mc100_writer_publication_pop(writer, &publication) == MC100_OK);
        assert(publication.generation == generation);
        assert(publication.segment_index == 0);
    }
    assert(mc100_writer_publication_pop(writer, &publication) ==
           MC100_NOT_READY);

    assert(mc100_writer_release_handles(writer) == MC100_OK);
    mc100_writer_destroy(writer);
    mc100_fake_io_destroy(fake);
}

int main(void)
{
    rotation_and_close_publish_writer_owned_names();
    publication_queue_is_bounded_and_fifo();
    return 0;
}
