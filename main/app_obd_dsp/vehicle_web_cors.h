#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Browser community installs need only these POST endpoints. Never expose
 * filesystem/config operations through a blanket device-API CORS policy.
 * Authentication remains mandatory on the actual POST request. */
static inline bool vehicle_web_cors_path(const char *uri)
{
    if (!uri) return false;
    static const char *const paths[] = {
        "/api/v1/vehicles", "/api/v1/vehicles/activate", "/api/v1/reboot",
    };
    size_t len = strcspn(uri, "?#");
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (strlen(paths[i]) == len && strncmp(uri, paths[i], len) == 0) return true;
    }
    return false;
}
