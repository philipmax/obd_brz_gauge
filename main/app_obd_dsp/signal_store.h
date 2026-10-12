#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * signal_store — unified OBD / system / vehicle signal table (P3)
 *
 * Layout (flat index, 64 slots total):
 *   0-11   obd.*  — 12 standard OBD signals matching editor contract
 *   12-31  sys.*  — system signals (reserved for future use)
 *   32-63  veh.*  — per-vehicle custom signals (dynamic, up to 32)
 *
 * Each slot stores:
 *   value  int32_t  — raw signal value (scaled; see comments per signal)
 *   valid  bool     — false when the value is unknown / not yet received
 *   ts_ms  uint32_t — esp_timer ms timestamp of last valid write
 *   seq    uint16_t — monotone counter; incremented on every write
 *
 * Sentinel-to-invalid mapping (obd_data_cache wrappers call these):
 *   rpm         (any)         → always valid when written
 *   coolant/intake -40        → invalid (initial "not yet received" value)
 *   oil_temp    -100          → invalid
 *   load_pct    -1            → invalid
 *   tps         -1            → invalid
 *   bat_mv      -1            → invalid
 *   oil_pres    -1 or >200    → invalid (range guard in setter)
 *   boost       -32768        → invalid
 *   gear        127           → invalid
 *   afr_x100    -1            → invalid
 * -------------------------------------------------------------------------- */

/* ---- slot index constants ---- */
#define SIG_OBD_RPM           0
#define SIG_OBD_SPEED         1
#define SIG_OBD_COOLANT_TEMP  2
#define SIG_OBD_OIL_TEMP      3
#define SIG_OBD_INTAKE_TEMP   4
#define SIG_OBD_LOAD          5
#define SIG_OBD_TPS           6
#define SIG_OBD_BAT_MV        7
#define SIG_OBD_OIL_PRESSURE  8   /* ×10, 0.1 bar */
#define SIG_OBD_BOOST         9   /* ×10, 0.1 bar, signed */
#define SIG_OBD_GEAR          10  /* -1=R, 0=N, 1-8=fwd */
#define SIG_OBD_AFR           11  /* ×100, e.g. 1470 = 14.70:1 */
#define SIG_OBD_COUNT         12

#define SIG_SYS_BASE          12
#define SIG_SYS_COUNT         20  /* reserved for sys.* */

#define SIG_VEH_BASE          32
#define SIG_VEH_COUNT         32  /* per-vehicle custom (veh.*) */

#define SIGNAL_TOTAL          64

/* ---- per-signal entry ---- */
typedef struct {
    int32_t  value;
    uint32_t ts_ms;  /* timestamp of last valid write (esp_timer ms) */
    uint16_t seq;    /* incremented on every write (valid or invalid) */
    bool     valid;
} signal_entry_t;

/**
 * @brief Initialise the signal store (zero all entries, mark all invalid).
 *        Call once before any other signal_store_* function.
 */
void signal_store_init(void);

/**
 * @brief Write a value.  Sets valid=true and updates ts_ms / seq.
 * @param idx  Signal slot index (0..SIGNAL_TOTAL-1).
 * @param val  Value to write.
 */
void signal_store_set(uint8_t idx, int32_t val);

/**
 * @brief Mark a slot as invalid (value kept, valid=false, seq incremented).
 *        Use when the source reports "no data" or a sentinel.
 */
void signal_store_invalidate(uint8_t idx);

/**
 * @brief Read a single slot.
 * @param idx  Signal slot index.
 * @param out  Destination; written atomically.
 */
void signal_store_get(uint8_t idx, signal_entry_t *out);

/**
 * @brief Read only the value; returns 0 when the slot is invalid.
 */
int32_t signal_store_value(uint8_t idx);

/**
 * @brief Returns true when the slot holds a valid (not-yet-invalidated) value.
 */
bool signal_store_is_valid(uint8_t idx);

/**
 * @brief 64-bit bitmask: bit N is set when slot N is currently valid.
 *        Lets callers quickly test "any OBD signal valid?" without iterating.
 */
uint64_t signal_store_valid_mask(void);

/**
 * @brief Reset all slots to invalid (keeps seq counts).
 *        Called when ELM327 disconnects or the profile is changed.
 */
void signal_store_reset_all(void);

/**
 * @brief Allocate a veh.* slot and return its index, or -1 when full.
 *        Slots are allocated in order from SIG_VEH_BASE.
 */
int signal_store_veh_alloc(void);

#ifdef __cplusplus
}
#endif
