// The big screen: a page served by the round screen itself on the home network (http://maze.local, or its
// address), and a WebSocket (/ws) that sends it the level when it asks ({"t":"level"}, as it opens) and where the
// ball is about 30 times a second. The page only draws. No software on the computer, just a browser.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts the server on port 80 and announces maze.local. Again while running: ESP_OK. */
esp_err_t web_start(void);
bool web_running(void);
/** The level message every page gets when it connects, and now to those already there. */
void web_set_level(const char *json);
/** The latest state for every page (dropped when the previous one is still being sent). Any task. */
void web_publish(const char *json);
/** Pages connected (as of the last send). */
int web_clients(void);
/** States sent, states dropped (the previous one still going), the slowest send since the last call (us). */
void web_stats(uint32_t *sent, uint32_t *dropped, uint32_t *send_us_max);

#ifdef __cplusplus
}
#endif
