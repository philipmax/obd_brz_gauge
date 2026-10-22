#pragma once

/* Boot-lifetime LVGL task. Pass the already-created lvgl_mux as task argument.
 * This task owns default direct-notification slot 0; never delete/rebind it.
 * Worker wakeups contain no session/view pointer and coalesce into one bit. */
void lvgl_runtime_task(void *mux);

/* Task context only; harmless before LVGL starts. Does not call LVGL. */
void lvgl_runtime_wake(void);
