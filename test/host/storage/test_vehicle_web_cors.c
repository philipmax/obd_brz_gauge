#include <assert.h>
#include <stdio.h>
#include "app_obd_dsp/vehicle_web_cors.h"

int main(void)
{
    const char *allowed[] = {
        "/api/v1/vehicles", "/api/v1/vehicles/activate", "/api/v1/reboot",
        "/api/v1/vehicles?test=1",
    };
    const char *denied[] = {
        NULL, "", "/", "/api/v1/vehicles/", "/api/v1/vehicles/remove",
        "/api/v1/vehicles/obd2.generic", "/api/v1/vehicles/activate-extra",
        "/api/v1/reboot/", "/api/v1/fs/list", "/api/v1/config", "/api/v1/time",
        "/api/v1/theme/sidecar", "/api/v1/vehicles-malicious", "/api/v1/vehicles%2factivate",
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
        assert(vehicle_web_cors_path(allowed[i]));
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i)
        assert(!vehicle_web_cors_path(denied[i]));
    puts("vehicle web CORS allowlist OK");
    return 0;
}
