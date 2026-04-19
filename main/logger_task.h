#pragma once
#include "opensense.h"

/* Dequeues log_entry_t from g_log_queue and writes CSV rows to the SD card.
 * Writes a fresh header whenever config_version changes.
 * Stack: 5120  Priority: 2 */
void logger_task(void *arg);

/* Signal logger to rewrite the CSV header on the next entry */
void logger_reset_header(void);
