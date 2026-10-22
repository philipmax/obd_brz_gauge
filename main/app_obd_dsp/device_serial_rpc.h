#pragma once
#include <stdbool.h>
/* Dispatch a verified USB management JSON request. Caller frees the response.
 * A reboot request is acknowledged before the serial worker restarts. */
char *device_serial_rpc(const char *json, bool *restart);
