#ifndef CGR_LOG_H
#define CGR_LOG_H

#include "cgreclaim.h"

/*
 * File-based logger for cgreclaim.
 * Writes timestamped entries to /home/root/cgreclaim.log.
 *
 * Usage:
 *   cgr_log_open();                    // optional eager open
 *   cfg.log_fn = cgr_log_file;         // pass as log callback
 *   ...
 *   cgr_log_close();                   // call at shutdown
 */

#define CGR_LOG_PATH	"/home/root/cgreclaim.log"

/*
 * Open the log file (append mode). Returns 0 on success, -1 on error.
 * Optional: cgr_log_file() will also try to open lazily on first write.
 */
int cgr_log_open(void);

/* Close the log file. */
void cgr_log_close(void);

/* Log callback compatible with cgr_config.log_fn */
void cgr_log_file(int level, const char *fmt, ...);

#endif /* CGR_LOG_H */
