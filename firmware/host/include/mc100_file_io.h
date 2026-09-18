#ifndef MC100_FILE_IO_H
#define MC100_FILE_IO_H
#include "mc100_io.h"
typedef struct mc100_file_io mc100_file_io_t;
/* Existing caller-owned directory only; no delete, directory creation or
 * format. Native Windows test adapter. Destroy closes OS handles without
 * writing/syncing. */
mc100_file_io_t *mc100_file_io_create(const char *directory);
void mc100_file_io_destroy(mc100_file_io_t *);
const mc100_io_t *mc100_file_io_ops(void);
#endif
