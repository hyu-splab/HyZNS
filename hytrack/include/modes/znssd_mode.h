/**
 * znssd_mode.h
 * znssd-device Mode (BBSSD+ZNSSD) Definitions and Functions
 */

#ifndef znssd_MODE_H
#define znssd_MODE_H

#include <ncurses.h>

#include "../core/data_types.h"

// ZNSSD zone descriptor information structure
typedef struct {
  uint8_t zt;     /* Zone Type */
  uint8_t zs;     /* Zone State */
  uint8_t za;     /* Zone Attributes */
  uint64_t zcap;  /* Zone Capacity */
  uint64_t zslba; /* Zone Start LBA */
  uint64_t wp;    /* Write Pointer */
} znssd_zone_descr_info_t;

// Updated ZNSSD zone information structure
typedef struct {
  znssd_zone_descr_info_t d;
  uint64_t w_ptr; /* Write pointer */
} znssd_zone_info_t;
// znssd-device monitor data structure

typedef struct {
  uint64_t num_zones;
  uint64_t zr_count;
  znssd_zone_info_t *znssd_data; /* ZNSSD device data */
} znssd_data_t;

/**
 * Initialize znssd-device mode
 *
 * @param manager Device manager to initialize
 * @return 0 on success, non-zero on failure
 */
int znssd_mode_init(device_context_t *manager);

/**
 * Clean up znssd-device mode resources
 *
 * @param manager Device manager to clean up
 */
void znssd_mode_cleanup(device_context_t *manager);

/**
 * Get znssd data from device
 *
 * @param device Device context
 * @return 0 on success, non-zero on failure
 */
int znssd_mode_get_data(device_context_t *device);

/**
 * Update ZNSSD history data
 *
 * @param device Device context
 */
void znssd_mode_update_history(device_context_t *device);

/**
 * Render ZNSSD-specific UI elements
 *
 * @param win Window to render to
 * @param device Device context
 * @param row Starting row
 * @param col St arting column
 * @param width Available width
 * @param height Available height
 * @return Next row after rendered elements
 */
int znssd_mode_render(WINDOW *win, device_context_t *device, int row, int col,
                      int width, int height);

#endif /* znssd_MODE_H */