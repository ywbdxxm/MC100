#include "mc100_fake_io.h"
#include <stdlib.h>
#include <string.h>
enum { FILES = 64, HANDLES = 128 };
typedef struct {
  char path[MC100_PATH_BYTES];
  uint8_t *bytes;
  size_t size;
} node_t;
struct mc100_file {
  node_t *node;
  bool writable, used;
};
struct mc100_fake_io {
  node_t nodes[FILES];
  struct mc100_file handles[HANDLES];
  size_t count;
  uint64_t total, free_bytes;
  mc100_fake_op_t *log;
  size_t logs, capacity;
  size_t fault;
  mc100_result_t result;
  bool short_transfer, short_now;
};
static mc100_result_t op(mc100_fake_io_t *f, char kind, const char *path,
                         uint64_t offset, size_t n) {
  if (f->logs == f->capacity) {
    size_t cap = f->capacity ? f->capacity * 2 : 256;
    void *p = realloc(f->log, cap * sizeof(*f->log));
    if (!p)
      return MC100_IO;
    f->log = p;
    f->capacity = cap;
  }
  mc100_fake_op_t *e = &f->log[f->logs++];
  memset(e, 0, sizeof(*e));
  e->kind = kind;
  if (path)
    memcpy(e->path, path, strlen(path) + 1);
  e->offset = offset;
  e->length = n;
  f->short_now = f->fault == f->logs && f->short_transfer;
  return f->fault == f->logs ? f->result : MC100_OK;
}
static node_t *find(mc100_fake_io_t *f, const char *p) {
  for (size_t i = 0; i < f->count; ++i)
    if (!strcmp(f->nodes[i].path, p))
      return &f->nodes[i];
  return NULL;
}
static mc100_result_t opening(void *ctx, const char *p, mc100_file_t *out,
                              int mode) {
  mc100_fake_io_t *f = ctx;
  if (!mc100_path_valid(p) || !out)
    return MC100_INVALID;
  mc100_result_t r = op(f, mode == 0 ? 'X' : mode == 1 ? 'O' : 'U', p, 0, 0);
  if (r)
    return r;
  node_t *n = find(f, p);
  if ((mode == 0 && n) || (mode != 0 && !n))
    return MC100_NOT_READY;
  if (!n) {
    if (f->count == FILES)
      return MC100_FULL;
    n = &f->nodes[f->count++];
    memcpy(n->path, p, strlen(p) + 1);
  }
  for (size_t i = 0; i < HANDLES; ++i)
    if (!f->handles[i].used) {
      f->handles[i].used = true;
      f->handles[i].writable = mode != 1;
      f->handles[i].node = n;
      *out = &f->handles[i];
      return MC100_OK;
    }
  return MC100_IO;
}
static mc100_result_t exclusive(void *c, const char *p, mc100_file_t *f) {
  return opening(c, p, f, 0);
}
static mc100_result_t reading(void *c, const char *p, mc100_file_t *f) {
  return opening(c, p, f, 1);
}
static mc100_result_t updating(void *c, const char *p, mc100_file_t *f) {
  return opening(c, p, f, 2);
}
static mc100_result_t resize(mc100_fake_io_t *f, mc100_file_t h,
                             uint64_t size) {
  if (!h || !h->used || !h->writable || size > SIZE_MAX)
    return MC100_INVALID;
  node_t *n = h->node;
  if (size > n->size && size - n->size > f->free_bytes)
    return MC100_FULL;
  if (size > n->size) {
    void *p = realloc(n->bytes, (size_t)size);
    if (!p)
      return MC100_IO;
    n->bytes = p;
    memset(n->bytes + n->size, 0xa5, (size_t)size - n->size);
    f->free_bytes -= size - n->size;
  } else
    f->free_bytes += n->size - size;
  n->size = (size_t)size;
  return MC100_OK;
}
static mc100_result_t readat(void *c, mc100_file_t h, uint64_t off, void *b,
                             size_t n, size_t *a) {
  mc100_fake_io_t *f = c;
  if (!h || !h->used || !a)
    return MC100_INVALID;
  *a = 0;
  mc100_result_t r = op(f, 'R', h->node->path, off, n);
  if (r)
    return r;
  if (off > h->node->size)
    return MC100_OK;
  if (n > h->node->size - (size_t)off)
    n = h->node->size - (size_t)off;
  if (f->short_now && n)
    --n;
  if (n)
    memcpy(b, h->node->bytes + (size_t)off, n);
  *a = n;
  return MC100_OK;
}
static mc100_result_t writeat(void *c, mc100_file_t h, uint64_t off,
                              const void *b, size_t n, size_t *a) {
  mc100_fake_io_t *f = c;
  if (!h || !h->used || !h->writable || !a)
    return MC100_INVALID;
  *a = 0;
  mc100_result_t r = op(f, 'W', h->node->path, off, n);
  if (r)
    return r;
  if (f->short_now && n)
    --n;
  if (off > UINT64_MAX - n)
    return MC100_INVALID;
  if (off + n > h->node->size) {
    r = resize(f, h, off + n);
    if (r)
      return r;
  }
  if (n)
    memcpy(h->node->bytes + (size_t)off, b, n);
  *a = n;
  return MC100_OK;
}
static mc100_result_t allocate(void *c, mc100_file_t h, uint64_t n,
                               uint64_t *a) {
  mc100_fake_io_t *f = c;
  mc100_result_t r = op(f, 'A', h->node->path, 0, (size_t)n);
  if (r)
    return r;
  if (f->short_now && n)
    --n;
  r = resize(f, h, n);
  *a = h->node->size;
  return r;
}
static mc100_result_t syncfile(void *c, mc100_file_t h) {
  return op(c, 'S', h->node->path, 0, 0);
}
static mc100_result_t truncfile(void *c, mc100_file_t h, uint64_t n) {
  mc100_result_t r = op(c, 'T', h->node->path, 0, (size_t)n);
  return r ? r : resize(c, h, n);
}
static mc100_result_t closefile(void *c, mc100_file_t h) {
  mc100_result_t r = op(c, 'C', h->node->path, 0, 0);
  if (!r)
    h->used = false;
  return r;
}
static mc100_result_t renamefile(void *c, const char *a, const char *b) {
  mc100_fake_io_t *f = c;
  if (!mc100_path_valid(a) || !mc100_path_valid(b))
    return MC100_INVALID;
  mc100_result_t r = op(f, 'N', a, 0, 0);
  if (r)
    return r;
  node_t *n = find(f, a);
  if (!n || find(f, b))
    return MC100_NOT_READY;
  memcpy(n->path, b, strlen(b) + 1);
  return MC100_OK;
}
static mc100_result_t statfile(void *c, const char *p, uint64_t *s) {
  mc100_fake_io_t *f = c;
  mc100_result_t r = op(f, 'F', p, 0, 0);
  if (r)
    return r;
  node_t *n = find(f, p);
  if (!n)
    return MC100_NOT_READY;
  *s = n->size;
  return MC100_OK;
}
static mc100_result_t space(void *c, uint64_t *t, uint64_t *b) {
  mc100_fake_io_t *f = c;
  mc100_result_t r = op(f, 'D', NULL, 0, 0);
  *t = f->total;
  *b = f->free_bytes;
  return r;
}
static mc100_result_t list(void *c, mc100_io_visit_fn visit, void *v) {
  mc100_fake_io_t *f = c;
  mc100_result_t r = op(f, 'L', NULL, 0, 0);
  if (r)
    return r;
  for (size_t i = 0; i < f->count; ++i) {
    r = visit(v, f->nodes[i].path);
    if (r)
      return r;
  }
  return MC100_OK;
}
static const mc100_io_t ops = {
    exclusive, reading,   updating,   readat,   writeat, allocate, syncfile,
    truncfile, closefile, renamefile, statfile, space,   list};
mc100_fake_io_t *mc100_fake_io_create(uint64_t t) {
  mc100_fake_io_t *f = calloc(1, sizeof(*f));
  if (f) {
    f->total = t;
    f->free_bytes = t;
  }
  return f;
}
void mc100_fake_io_destroy(mc100_fake_io_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < f->count; ++i)
    free(f->nodes[i].bytes);
  free(f->log);
  free(f);
}
const mc100_io_t *mc100_fake_io_ops(void) { return &ops; }
void mc100_fake_io_fault(mc100_fake_io_t *f, size_t relative, mc100_result_t r,
                         bool s) {
  f->fault = relative ? f->logs + relative : 0;
  f->result = r;
  f->short_transfer = s;
}
void mc100_fake_io_free_bytes(mc100_fake_io_t *f, uint64_t n) {
  f->free_bytes = n;
}
size_t mc100_fake_io_count(const mc100_fake_io_t *f) { return f->count; }
size_t mc100_fake_io_log_count(const mc100_fake_io_t *f) { return f->logs; }
const mc100_fake_op_t *mc100_fake_io_log(const mc100_fake_io_t *f, size_t i) {
  return i < f->logs ? &f->log[i] : NULL;
}
const uint8_t *mc100_fake_io_bytes(const mc100_fake_io_t *f, const char *p,
                                   size_t *n) {
  for (size_t i = 0; i < f->count; ++i)
    if (!strcmp(f->nodes[i].path, p)) {
      *n = f->nodes[i].size;
      return f->nodes[i].bytes;
    }
  return NULL;
}
const char *mc100_fake_io_path(const mc100_fake_io_t *f, size_t i) {
  return i < f->count ? f->nodes[i].path : NULL;
}
