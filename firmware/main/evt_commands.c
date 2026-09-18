#include "evt_commands.h"
#include "mc100_format.h"
#include <string.h>

bool mc100_evt_can_start(bool storage_enabled, bool audio_ready, bool fault, bool storage_ready)
{
    return audio_ready && !fault && (!storage_enabled || storage_ready);
}

bool mc100_evt_safe_to_release(bool done, mc100_result_t result)
{
    return done && result == MC100_OK;
}

static bool number(const char *s, uint64_t *out)
{
    uint64_t n = 0;
    if (!*s) return false;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        unsigned digit = (unsigned)(*s++ - '0');
        if (n > (UINT64_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *out = n;
    return true;
}

bool mc100_evt_parse(const char *s, mc100_evt_command_t *c)
{
    char copy[MC100_EVT_LINE_BYTES + 1];
    char *tokens[5];
    size_t size = 0, count = 0;
    if (!s || !c) return false;
    while (s[size]) {
        if (size == MC100_EVT_LINE_BYTES || (unsigned char)s[size] < 32 || (unsigned char)s[size] > 126) return false;
        copy[size] = s[size];
        ++size;
    }
    copy[size] = 0;
    char *p = copy;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (count == 5) return false;
        tokens[count++] = p;
        while (*p && *p != ' ') ++p;
        if (*p) *p++ = 0;
    }
    memset(c, 0, sizeof(*c));
    if (count == 1 && !strcmp(tokens[0], "status")) { c->kind = EVT_STATUS; return true; }
    if (count == 1 && !strcmp(tokens[0], "list")) { c->kind = EVT_LIST; return true; }
    uint64_t a, b;
    if (count == 2 && !strcmp(tokens[0], "sync") && number(tokens[1], &a) && a <= UINT32_MAX) {
        c->kind = EVT_SYNC; c->token = (uint32_t)a; return true;
    }
    if (count == 2 && !strcmp(tokens[0], "capture") && number(tokens[1], &a) && a >= 3 && a <= 60) {
        c->kind = EVT_CAPTURE; c->seconds = (uint32_t)a; return true;
    }
    if (count == 2 && !strcmp(tokens[0], "record") && number(tokens[1], &a) && a >= 3 && a <= 600) {
        c->kind = EVT_RECORD; c->seconds = (uint32_t)a; return true;
    }
    if (count == 4 && !strcmp(tokens[0], "read") && mc100_path_valid(tokens[1]) &&
        number(tokens[2], &a) && number(tokens[3], &b) && b && b <= MC100_EVT_READ_BYTES && a <= UINT64_MAX - b) {
        size_t n = strlen(tokens[1]);
        if (n < 4 || (strcmp(tokens[1] + n - 4, ".wav") && strcmp(tokens[1] + n - 4, ".idx"))) return false;
        memcpy(c->path, tokens[1], n + 1);
        c->kind = EVT_READ; c->offset = a; c->count = (uint32_t)b; return true;
    }
    return false;
}

int mc100_evt_line_feed(mc100_evt_line_t *l, unsigned char b, mc100_evt_command_t *c)
{
    if (!l || !c) return -1;
    if (b == '\n') {
        if (l->used && l->bytes[l->used - 1] == '\r') --l->used;
        l->bytes[l->used] = 0;
        bool valid = !l->discard && mc100_evt_parse(l->bytes, c);
        l->used = 0; l->discard = false;
        return valid ? 1 : -1;
    }
    if (l->discard) return 0;
    if ((b != '\r' && (b < 32 || b > 126)) || l->used == MC100_EVT_LINE_BYTES) l->discard = true;
    else l->bytes[l->used++] = (char)b;
    return 0;
}

bool mc100_evt_duration(uint32_t s, uint64_t f, uint64_t *n, uint64_t *e)
{
    if (!n || !e || s < 3 || s > 600) return false;
    uint64_t frames = (uint64_t)s * 50;
    /* The writer reserves source-sample room for a complete segment at begin. */
    if (f > (UINT64_MAX - MC100_MAX_PCM_BYTES / 2) / 320 ||
        f > (UINT64_MAX - 320) / 320 - (frames - 1)) return false;
    *n = frames; *e = f + frames - 1;
    return true;
}
