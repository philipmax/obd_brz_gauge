#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* USB Serial/JTAG bridge used by the browser flasher for SD boot-media upload. */
esp_err_t bootmedia_serial_start(void);

#ifdef __cplusplus
}
#endif
