#ifndef MC100_IO_H
#define MC100_IO_H
#include "mc100_types.h"
enum { MC100_PATH_BYTES = 128 };
typedef struct mc100_file *mc100_file_t;
typedef mc100_result_t (*mc100_io_visit_fn)(void *, const char *);
/* Paths are single MC100 filenames, never directories. NOT_READY means missing
 * source or existing exclusive destination; FULL means exhaustion, IO failure.
 * rename_no_replace must durably publish directory changes before returning OK.
 * close releases handles without implicit sync/write. list propagates visitor
 * errors. */
typedef struct {
  mc100_result_t (*open_exclusive)(void *, const char *, mc100_file_t *);
  mc100_result_t (*open_read)(void *, const char *, mc100_file_t *);
  mc100_result_t (*open_update)(void *, const char *, mc100_file_t *);
  mc100_result_t (*read_at)(void *, mc100_file_t, uint64_t, void *, size_t,
                            size_t *);
  mc100_result_t (*write_at)(void *, mc100_file_t, uint64_t, const void *,
                             size_t, size_t *);
  mc100_result_t (*allocate)(void *, mc100_file_t, uint64_t, uint64_t *);
  mc100_result_t (*sync)(void *, mc100_file_t);
  mc100_result_t (*truncate)(void *, mc100_file_t, uint64_t);
  mc100_result_t (*close)(void *, mc100_file_t);
  mc100_result_t (*rename_no_replace)(void *, const char *, const char *);
  mc100_result_t (*stat)(void *, const char *, uint64_t *);
  mc100_result_t (*space)(void *, uint64_t *, uint64_t *);
  mc100_result_t (*list)(void *, mc100_io_visit_fn, void *);
} mc100_io_t;
bool mc100_path_valid(const char *path);
#endif
