#ifndef BOOT_VIDEO_INTEGRATION_H
#define BOOT_VIDEO_INTEGRATION_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Boot video source type
 */
typedef enum {
    BOOT_VIDEO_SOURCE_SD = 0,
    BOOT_VIDEO_SOURCE_LEGACY,
    BOOT_VIDEO_SOURCE_BUILTIN
} boot_video_source_t;

/**
 * Initialize boot video integration
 * Determines which source to use: SD > Legacy > Builtin
 */
esp_err_t boot_video_integration_init(void);

/**
 * Start playing boot video/animation
 */
esp_err_t boot_video_integration_play(void);

/**
 * Stop boot video playback
 */
void boot_video_integration_stop(void);

/**
 * Get current boot video source
 */
boot_video_source_t boot_video_integration_get_source(void);

#ifdef __cplusplus
}
#endif

#endif // BOOT_VIDEO_INTEGRATION_H
