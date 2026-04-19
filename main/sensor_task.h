#pragma once
#include "opensense.h"

/* 10Hz sampling loop. Reads all 5 ports and distributes data to
 * shared state (for telemetry) and g_log_queue (for SD logging).
 * Stack: 5120  Priority: 4 */
void sensor_task(void *arg);
