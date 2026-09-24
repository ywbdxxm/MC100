#ifndef MC100_RECORD_LOOP_H
#define MC100_RECORD_LOOP_H

enum { MC100_RECORD_STORAGE_STACK_BYTES = 16384 };

/* One boot session; retains the storage owner in IDLE after completion. */
void mc100_record_run(void);

#endif
