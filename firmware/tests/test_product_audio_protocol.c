#include "mc100_product_audio_protocol.h"

#include <assert.h>
#include <stdio.h>

static void late_ack_does_not_match_new_command(void)
{
    mc100_product_audio_protocol_t protocol = {0};
    uint32_t first = mc100_product_audio_begin(&protocol);
    protocol.acknowledged_sequence = first;
    assert(mc100_product_audio_ack_matches(&protocol, first));

    uint32_t second = mc100_product_audio_begin(&protocol);
    assert(second != first);
    assert(!mc100_product_audio_ack_matches(&protocol, first));
    protocol.acknowledged_sequence = second;
    assert(mc100_product_audio_ack_matches(&protocol, second));
}

static void shutdown_timeout_requires_quarantine(void)
{
    assert(mc100_product_audio_shutdown_requires_quarantine(false));
    assert(!mc100_product_audio_shutdown_requires_quarantine(true));
}

int main(void)
{
    late_ack_does_not_match_new_command();
    shutdown_timeout_requires_quarantine();
    puts("product_audio_protocol: stale ACK and shutdown policy PASS");
    return 0;
}
