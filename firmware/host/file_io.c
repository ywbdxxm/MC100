#define WIN32_LEAN_AND_MEAN
#include "mc100_file_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
enum {
  FILE_HANDLES = 16,
  ROOT_BYTES = 768,
  FULL_BYTES = ROOT_BYTES + MC100_PATH_BYTES
};
struct mc100_file {
  HANDLE handle;
  bool writable, used;
};
struct mc100_file_io {
  char root[ROOT_BYTES];
  struct mc100_file files[FILE_HANDLES];
};
static mc100_result_t error_result(DWORD code) {
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
      code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)
    return MC100_NOT_READY;
  if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL)
    return MC100_FULL;
  return MC100_IO;
}
static bool path(mc100_file_io_t *f, const char *name, char full[FULL_BYTES]) {
  if (!mc100_path_valid(name))
    return false;
  int n = snprintf(full, FULL_BYTES, "%s\\%s", f->root, name);
  return n > 0 && n < FULL_BYTES;
}
static mc100_result_t opening(void *ctx, const char *name, mc100_file_t *out,
                              DWORD disposition, bool writable) {
  mc100_file_io_t *f = ctx;
  char full[FULL_BYTES];
  if (!out || !path(f, name, full))
    return MC100_INVALID;
  struct mc100_file *slot = NULL;
  for (size_t i = 0; i < FILE_HANDLES; ++i)
    if (!f->files[i].used) {
      slot = &f->files[i];
      break;
    }
  if (!slot)
    return MC100_IO;
  HANDLE h =
      CreateFileA(full, GENERIC_READ | (writable ? GENERIC_WRITE : 0),
                  FILE_SHARE_READ, NULL, disposition,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return error_result(GetLastError());
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info) ||
      (info.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    CloseHandle(h);
    return MC100_INVALID;
  }
  slot->used = true;
  slot->writable = writable;
  slot->handle = h;
  *out = slot;
  return MC100_OK;
}
static mc100_result_t exclusive(void *c, const char *p, mc100_file_t *h) {
  return opening(c, p, h, CREATE_NEW, true);
}
static mc100_result_t reading(void *c, const char *p, mc100_file_t *h) {
  return opening(c, p, h, OPEN_EXISTING, false);
}
static mc100_result_t updating(void *c, const char *p, mc100_file_t *h) {
  return opening(c, p, h, OPEN_EXISTING, true);
}
static mc100_result_t seek(mc100_file_t h, uint64_t offset) {
  if (!h || !h->used || offset > INT64_MAX)
    return MC100_INVALID;
  LARGE_INTEGER n;
  n.QuadPart = (LONGLONG)offset;
  return SetFilePointerEx(h->handle, n, NULL, FILE_BEGIN)
             ? MC100_OK
             : error_result(GetLastError());
}
static mc100_result_t readat(void *c, mc100_file_t h, uint64_t off, void *b,
                             size_t n, size_t *actual) {
  (void)c;
  if (!actual || n > MAXDWORD)
    return MC100_INVALID;
  *actual = 0;
  mc100_result_t r = seek(h, off);
  if (r)
    return r;
  DWORD count = 0;
  if (!ReadFile(h->handle, b, (DWORD)n, &count, NULL))
    return error_result(GetLastError());
  *actual = count;
  return MC100_OK;
}
static mc100_result_t writeat(void *c, mc100_file_t h, uint64_t off,
                              const void *b, size_t n, size_t *actual) {
  (void)c;
  if (!actual || !h || !h->writable || n > MAXDWORD)
    return MC100_INVALID;
  *actual = 0;
  mc100_result_t r = seek(h, off);
  if (r)
    return r;
  DWORD count = 0;
  if (!WriteFile(h->handle, b, (DWORD)n, &count, NULL))
    return error_result(GetLastError());
  *actual = count;
  return MC100_OK;
}
static mc100_result_t truncfile(void *c, mc100_file_t h, uint64_t n) {
  (void)c;
  if (!h || !h->writable)
    return MC100_INVALID;
  mc100_result_t r = seek(h, n);
  if (r)
    return r;
  return SetEndOfFile(h->handle) ? MC100_OK : error_result(GetLastError());
}
static mc100_result_t allocate(void *c, mc100_file_t h, uint64_t n,
                               uint64_t *actual) {
  if (!actual)
    return MC100_INVALID;
  *actual = 0;
  mc100_result_t r = truncfile(c, h, n);
  if (r)
    return r;
  LARGE_INTEGER size;
  if (!GetFileSizeEx(h->handle, &size))
    return error_result(GetLastError());
  *actual = (uint64_t)size.QuadPart;
  return MC100_OK;
}
static mc100_result_t syncfile(void *c, mc100_file_t h) {
  (void)c;
  if (!h || !h->used || !h->writable)
    return MC100_INVALID;
  return FlushFileBuffers(h->handle) ? MC100_OK : error_result(GetLastError());
}
static mc100_result_t closefile(void *c, mc100_file_t h) {
  (void)c;
  if (!h || !h->used)
    return MC100_INVALID;
  if (!CloseHandle(h->handle))
    return error_result(GetLastError());
  h->used = false;
  return MC100_OK;
}
static mc100_result_t renamefile(void *c, const char *a, const char *b) {
  mc100_file_io_t *f = c;
  char old[FULL_BYTES], next[FULL_BYTES];
  if (!path(f, a, old) || !path(f, b, next))
    return MC100_INVALID;
  return MoveFileExA(old, next, MOVEFILE_WRITE_THROUGH)
             ? MC100_OK
             : error_result(GetLastError());
}
static mc100_result_t statfile(void *c, const char *p, uint64_t *size) {
  char full[FULL_BYTES];
  if (!size || !path(c, p, full))
    return MC100_INVALID;
  WIN32_FILE_ATTRIBUTE_DATA info;
  if (!GetFileAttributesExA(full, GetFileExInfoStandard, &info))
    return error_result(GetLastError());
  if (info.dwFileAttributes &
      (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
    return MC100_INVALID;
  *size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
  return MC100_OK;
}
static mc100_result_t space(void *c, uint64_t *total, uint64_t *free_bytes) {
  mc100_file_io_t *f = c;
  ULARGE_INTEGER available, capacity, free_total;
  if (!total || !free_bytes)
    return MC100_INVALID;
  if (!GetDiskFreeSpaceExA(f->root, &available, &capacity, &free_total))
    return error_result(GetLastError());
  *total = capacity.QuadPart;
  *free_bytes = available.QuadPart;
  return MC100_OK;
}
static mc100_result_t list(void *c, mc100_io_visit_fn visit, void *v) {
  mc100_file_io_t *f = c;
  char pattern[FULL_BYTES];
  if (!visit)
    return MC100_INVALID;
  (void)snprintf(pattern, sizeof(pattern), "%s\\*", f->root);
  WIN32_FIND_DATAA data;
  HANDLE h = FindFirstFileA(pattern, &data);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND ? MC100_OK : error_result(e);
  }
  mc100_result_t r = MC100_OK;
  do {
    if (!(data.dwFileAttributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        mc100_path_valid(data.cFileName)) {
      r = visit(v, data.cFileName);
      if (r)
        break;
    }
  } while (FindNextFileA(h, &data));
  DWORD e = GetLastError();
  FindClose(h);
  return r ? r : e == ERROR_NO_MORE_FILES ? MC100_OK : error_result(e);
}
static const mc100_io_t ops = {
    exclusive, reading,   updating,   readat,   writeat, allocate, syncfile,
    truncfile, closefile, renamefile, statfile, space,   list};
mc100_file_io_t *mc100_file_io_create(const char *directory) {
  if (!directory)
    return NULL;
  mc100_file_io_t *f = calloc(1, sizeof(*f));
  if (!f)
    return NULL;
  DWORD n = GetFullPathNameA(directory, ROOT_BYTES, f->root, NULL);
  DWORD a = n && n < ROOT_BYTES ? GetFileAttributesA(f->root)
                                : INVALID_FILE_ATTRIBUTES;
  if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) ||
      (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
    free(f);
    return NULL;
  }
  return f;
}
void mc100_file_io_destroy(mc100_file_io_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < FILE_HANDLES; ++i)
    if (f->files[i].used)
      CloseHandle(f->files[i].handle);
  free(f);
}
const mc100_io_t *mc100_file_io_ops(void) { return &ops; }
