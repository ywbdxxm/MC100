#ifndef MC100_UPLOAD_H
#define MC100_UPLOAD_H

#include "mc100_types.h"

/* Phase 6 wireless offload seam. V1 is a notification-only no-op. */
typedef struct {
    void (*notify_closed)(void *ctx, const char *final_name);
    void *ctx;
} mc100_upload_sink_t;

mc100_upload_sink_t mc100_upload_noop(void);

#endif
