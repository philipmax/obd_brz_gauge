#pragma once
/*
 * espnow_v6.h — ESP-NOW v6 protocol (三连表数据同步)
 *
 * Changes from v5:
 *  - Add vehicle profile ID sync (4 bytes)
 *  - Add signal validity flags (bitmask)
 *  - Remove retired brake temp field (offset 23)
 *  - Add lifecycle state (1 byte)
 *  - Protocol version = 6
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_V6_PROTOCOL_VERSION 6
#define ESPNOW_V6_MAX_PEERS 8

/* ESP-NOW v6 data packet (primary → secondary gauges) */
typedef struct __attribute__((packed)) {
    uint8_t version;              /* Protocol version = 6 */
    uint8_t seq;                  /* Sequence number */
    uint8_t lifecycle_state;      /* Lifecycle state (from lifecycle_state.h) */
    uint8_t reserved_3;           /* Reserved (was brake_temp in v5 offset 23) */

    /* Core signals */
    uint16_t rpm;                 /* Engine RPM */
    uint8_t speed;                /* Vehicle speed (km/h) */
    int16_t oil_temp;             /* Oil temp (°C * 10) */
    int16_t coolant_temp;         /* Coolant temp (°C * 10) */
    uint8_t throttle;             /* Throttle position (%) */
    int8_t gear;                  /* Current gear */
    uint16_t boost;               /* Boost pressure (kPa * 10) */
    uint16_t afr;                 /* Air-fuel ratio (* 100) */
    uint8_t load;                 /* Engine load (%) */
    uint16_t battery_mv;          /* Battery voltage (mV) */
    uint16_t oil_pressure;        /* Oil pressure (kPa * 10) */

    /* Vehicle profile ID (for profile sync) */
    uint32_t vehicle_profile_crc; /* CRC32 of active profile name */

    /* Signal validity flags (bitmask, 1 = valid) */
    uint32_t valid_flags;         /* Bit per signal */

    /* Extended data */
    int16_t intake_temp;          /* Intake air temp (°C * 10) */
    uint8_t fuel_level;           /* Fuel level (%) */

    uint8_t crc8;                 /* CRC8 checksum */
} espnow_v6_data_t;

/* Signal validity bit positions */
#define ESPNOW_V6_VALID_RPM           (1 << 0)
#define ESPNOW_V6_VALID_SPEED         (1 << 1)
#define ESPNOW_V6_VALID_OIL_TEMP      (1 << 2)
#define ESPNOW_V6_VALID_COOLANT       (1 << 3)
#define ESPNOW_V6_VALID_THROTTLE      (1 << 4)
#define ESPNOW_V6_VALID_GEAR          (1 << 5)
#define ESPNOW_V6_VALID_BOOST         (1 << 6)
#define ESPNOW_V6_VALID_AFR           (1 << 7)
#define ESPNOW_V6_VALID_LOAD          (1 << 8)
#define ESPNOW_V6_VALID_BATTERY       (1 << 9)
#define ESPNOW_V6_VALID_OIL_PRESSURE  (1 << 10)
#define ESPNOW_V6_VALID_INTAKE_TEMP   (1 << 11)
#define ESPNOW_V6_VALID_FUEL          (1 << 12)

/**
 * Initialize ESP-NOW v6.
 */
esp_err_t espnow_v6_init(void);

/**
 * Add peer device (secondary gauge).
 */
esp_err_t espnow_v6_add_peer(const uint8_t mac[6]);

/**
 * Remove peer device.
 */
esp_err_t espnow_v6_remove_peer(const uint8_t mac[6]);

/**
 * Send data to all peers (called from primary gauge).
 */
esp_err_t espnow_v6_broadcast(const espnow_v6_data_t *data);

/**
 * Receive callback (called on secondary gauges).
 */
typedef void (*espnow_v6_recv_cb_t)(const espnow_v6_data_t *data, const uint8_t src_mac[6]);

/**
 * Register receive callback.
 */
esp_err_t espnow_v6_register_recv_callback(espnow_v6_recv_cb_t cb);

/**
 * Get statistics.
 */
typedef struct {
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t tx_errors;
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t rx_crc_errors;
    uint32_t rx_version_errors;
} espnow_v6_stats_t;

esp_err_t espnow_v6_get_stats(espnow_v6_stats_t *stats);

/**
 * Calculate CRC8 for packet.
 */
uint8_t espnow_v6_calc_crc8(const espnow_v6_data_t *data);

#ifdef __cplusplus
}
#endif
