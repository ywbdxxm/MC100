#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "evt_commands.h"
#include "mc100_audio.h"

/* Catches using the trigger sequence as ARM's min_seq (which trims all
 * pre-roll), duration excluding pre-roll, and stale frames on a second take. */
static void exact_sessions(void)
{
    mc100_audio_t *audio = mc100_audio_create();
    assert(audio);
    for (uint64_t generation = 1; generation <= 2; ++generation) {
        uint64_t first = generation == 1 ? 0 : 150;
        uint64_t frames, last;
        assert(mc100_evt_duration(3, first, &frames, &last));
        assert(mc100_audio_arm(audio, generation, first) == MC100_OK);
        mc100_frame_t frame = {0};
        for (uint64_t seq = first; seq <= last; ++seq) {
            frame.seq = seq;
            frame.pcm[0] = (int16_t)generation;
            assert(mc100_audio_push(audio, &frame, seq == first + 100) == MC100_OK);
        }
        uint64_t cutoff;
        assert(mc100_audio_stop(audio, generation, &cutoff) == MC100_OK && cutoff == last);
        mc100_snapshot_t snapshot;
        assert(mc100_audio_snapshot(audio, generation, &snapshot) == MC100_OK);
        assert(snapshot.count == 100 && snapshot.first_seq == first);
        for (uint16_t i = 0; i < 100; ++i) {
            assert(mc100_audio_snapshot_frame(audio, &snapshot, i, &frame) == MC100_OK);
            assert(frame.seq == first + i && frame.pcm[0] == (int16_t)generation);
        }
        assert(mc100_audio_snapshot_release(audio, generation) == MC100_OK);
        mc100_packet_t packet;
        for (uint64_t seq = first + 100; seq <= last; ++seq) {
            assert(mc100_audio_pop(audio, &packet) == MC100_OK);
            assert(packet.generation == generation && packet.frame.seq == seq);
        }
        assert(mc100_audio_pop(audio, &packet) == MC100_NOT_READY);
        assert(mc100_audio_release(audio, generation) == MC100_OK);
    }
    mc100_audio_destroy(audio);
}

int main(void)
{
    mc100_evt_command_t c;
    assert(mc100_evt_parse("sync 4294967295", &c) && c.kind == EVT_SYNC && c.token == UINT32_MAX);
    assert(mc100_evt_parse("sync 0", &c) && c.kind == EVT_SYNC && c.token == 0);
    assert(!mc100_evt_parse("sync 4294967296", &c));
    assert(!mc100_evt_parse("sync 1 extra", &c));
    assert(!mc100_evt_parse("sync -1", &c));
    assert(mc100_evt_parse("capture 3", &c) && c.kind == EVT_CAPTURE && c.seconds == 3);
    assert(mc100_evt_parse("capture 60", &c) && c.kind == EVT_CAPTURE && c.seconds == 60);
    assert(!mc100_evt_parse("capture 2", &c));
    assert(!mc100_evt_parse("capture 61", &c));
    assert(!mc100_evt_parse("capture -3", &c));
    assert(!mc100_evt_parse("capture 3 extra", &c));
    assert(mc100_evt_can_start(false, true, false, false));
    assert(!mc100_evt_can_start(true, true, false, false));
    assert(mc100_evt_can_start(true, true, false, true));
    assert(!mc100_evt_can_start(false, true, true, false));
    assert(!mc100_evt_can_start(false, false, false, false));
    assert(!mc100_evt_safe_to_release(true, MC100_IO));
    assert(!mc100_evt_safe_to_release(false, MC100_OK));
    assert(mc100_evt_safe_to_release(true, MC100_OK));
    assert(mc100_evt_parse("status", &c) && c.kind == EVT_STATUS);
    assert(mc100_evt_parse(" list ", &c) && c.kind == EVT_LIST);
    assert(mc100_evt_parse("record 3", &c) && c.seconds == 3);
    assert(mc100_evt_parse("record 600", &c) && c.seconds == 600);
    const char *bad[] = {"record 2", "record 601", "record -3", "record +3", "record 3x", "record 3 extra", "record 18446744073709551616", "status x", "", "format", "record\t3"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) assert(!mc100_evt_parse(bad[i], &c));
    assert(mc100_evt_parse("read 0123456789abcdef0123456789abcdef_1_0.wav 0 1024", &c));
    assert(c.kind == EVT_READ && c.offset == 0 && c.count == 1024);
    assert(!mc100_evt_parse("read 0123456789abcdef0123456789abcdef_1_0.wav.part 0 2", &c));
    assert(!mc100_evt_parse("read ../secret.wav 0 2", &c));
    assert(!mc100_evt_parse("read 0123456789abcdef0123456789abcdef_1_0.idx 0 1025", &c));
    assert(!mc100_evt_parse("read 0123456789abcdef0123456789abcdef_1_0.idx 18446744073709551615 1", &c));
    uint64_t n, last;
    assert(mc100_evt_duration(3, 0, &n, &last) && n == 150 && last == 149);
    assert(mc100_evt_duration(60, 150, &n, &last) && n == 3000 && last == 3149);
    assert(mc100_evt_duration(600, 150, &n, &last) && n == 30000 && last == 30149);
    assert(!mc100_evt_duration(3, UINT64_MAX - 10, &n, &last));
    assert(!mc100_evt_duration(3, UINT64_MAX / 320 - 100, &n, &last));
    mc100_evt_line_t line = {0};
    const char *valid = "record 3\r\n";
    for (size_t i = 0; valid[i]; ++i) assert(mc100_evt_line_feed(&line, (unsigned char)valid[i], &c) == (valid[i] == '\n' ? 1 : 0));
    for (unsigned i = 0; i < 193; ++i) assert(mc100_evt_line_feed(&line, 'a', &c) == 0);
    assert(mc100_evt_line_feed(&line, '\n', &c) == -1);
    assert(mc100_evt_line_feed(&line, 0, &c) == 0);
    valid = "status\n";
    for (size_t i = 0; valid[i]; ++i) assert(mc100_evt_line_feed(&line, (unsigned char)valid[i], &c) == (valid[i] == '\n' ? -1 : 0));
    for (size_t i = 0; valid[i]; ++i) assert(mc100_evt_line_feed(&line, (unsigned char)valid[i], &c) == (valid[i] == '\n' ? 1 : 0));
    exact_sessions();
    puts("EVT command/framing/duration/two-session pre-roll tests PASS");
    return 0;
}
