#include "mc100_upload.h"

static void noop_notify(void *ctx, const char *final_name) {
    (void)ctx;
    (void)final_name;
}

mc100_upload_sink_t mc100_upload_noop(void) {
    mc100_upload_sink_t sink = { noop_notify, (void *)0 };
    return sink;
}
