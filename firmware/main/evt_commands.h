#ifndef MC100_EVT_COMMANDS_H
#define MC100_EVT_COMMANDS_H
#include "mc100_io.h"
enum { MC100_EVT_LINE_BYTES = 192, MC100_EVT_READ_BYTES = 1024 };
typedef enum { EVT_NONE, EVT_STATUS, EVT_LIST, EVT_RECORD, EVT_READ, EVT_CAPTURE, EVT_SYNC } mc100_evt_kind_t;
typedef struct {
    mc100_evt_kind_t kind;
    uint32_t seconds, count, token;
    uint64_t offset;
    char path[MC100_PATH_BYTES];
} mc100_evt_command_t;
typedef struct { char bytes[MC100_EVT_LINE_BYTES + 1]; size_t used; bool discard; } mc100_evt_line_t;
/* 0 incomplete, 1 valid command, -1 rejected whole line. */
int mc100_evt_line_feed(mc100_evt_line_t *, unsigned char, mc100_evt_command_t *);
bool mc100_evt_parse(const char *, mc100_evt_command_t *);
bool mc100_evt_duration(uint32_t seconds, uint64_t first_seq, uint64_t *frames, uint64_t *last_seq);
bool mc100_evt_safe_to_release(bool worker_done, mc100_result_t stop_result);
bool mc100_evt_can_start(bool storage_enabled, bool audio_ready, bool fault, bool storage_ready);
#endif
