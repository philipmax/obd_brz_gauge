#ifndef ELM327_INTEGRATION_H
#define ELM327_INTEGRATION_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize ELM327 integration with vp_scheduler
 */
void elm327_integration_init(void);

/**
 * Called from poll task: tick scheduler, send requests, sync signals
 */
void elm327_integration_tick(void);

/**
 * Called when ELM327 response arrives: feed to scheduler
 */
void elm327_integration_on_response(const char *response);

#ifdef __cplusplus
}
#endif

#endif // ELM327_INTEGRATION_H
