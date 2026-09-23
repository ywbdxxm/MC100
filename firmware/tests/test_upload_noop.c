#include "mc100_upload.h"
#include <assert.h>
#include <string.h>

static int spy_calls;
static char spy_last[128];
static void spy_notify(void *ctx, const char *name) {
    (void)ctx;
    ++spy_calls;
    strncpy(spy_last, name, sizeof(spy_last) - 1);
}

int main(void) {
    mc100_upload_sink_t noop = mc100_upload_noop();
    assert(noop.notify_closed != NULL);
    noop.notify_closed(noop.ctx, "abc_1_0.wav");

    mc100_upload_sink_t spy = { .notify_closed = spy_notify, .ctx = NULL };
    spy.notify_closed(spy.ctx, "abc_1_0.wav");
    assert(spy_calls == 1);
    assert(strcmp(spy_last, "abc_1_0.wav") == 0);
    return 0;
}
