/**
 * znssd_mode.c
 * znssd Mode Implementation
 */

#include "../../include/modes/znssd_mode.h"

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/common/history.h"
#include "../../include/common/ui_common.h"
#include "../../include/core/nvme_cmd.h"
#include "../../include/views/debug_view.h"

/**
 * Initialize znssd mode for a device
 *
 * @param device Device context to initialize
 * @return 0 on success, non-zero on failure
 */
int znssd_mode_init(device_context_t *device) {
  znssd_data_t *data;

  // Allocate device data
  data = (znssd_data_t *)malloc(sizeof(znssd_data_t));
  if (!data) {
    debug_log_add("Failed to allocate znssd data structure");
    return -1;
  }

  // Initialize data
  memset(data, 0, sizeof(znssd_data_t));
  data->znssd_data = NULL;

  // Store data in device context
  device->device_data = data;
  device->data_size = sizeof(znssd_data_t);

  debug_log_add("ZNSSD mode initialized for %s", device->device_path);
  return 0;
}

/**
 * Clean up znssd mode resources
 *
 * @param device Device context to clean up
 */
void znssd_mode_cleanup(device_context_t *device) {
  znssd_data_t *data = (znssd_data_t *)device->device_data;

  if (data) {
    // Free line management info
    if (data->znssd_data) {
      free(data->znssd_data);
      data->znssd_data = NULL;
    }

    // Free device data
    free(data);
    device->device_data = NULL;
  }
}

/**
 * Get znssd data from device
 *
 * @param device Device context
 * @return 0 on success, non-zero on failure
 */
int znssd_mode_get_data(device_context_t *device) {
  znssd_data_t *data = (znssd_data_t *)device->device_data;
  int ret;

  // Get data directly from the device using the refactored function
  ret = nvme_get_znssd_data(data, device->device_path, device->device_info);

  if (ret == 0) {
    debug_log_add("Successfully retrieved znssd data: %d zones",
                  data->num_zones);
  } else {
    debug_log_add("Failed to get znssd data from device, error: %d", ret);
  }

  return ret;
}

/**
 * Update znssd history data
 *
 * @param device Device context
 */
void znssd_mode_update_history(device_context_t *device) {
  znssd_data_t *data = (znssd_data_t *)device->device_data;
  history_set_t *histories = device->histories;

  //    int open_zones = data->current_open_zones;
  int full_zones = 0;
  int empty_zones = 0;

  // Make sure we have valid data
  if (!data || !data->znssd_data) {
    debug_log_add("Cannot update history: no valid data available");
    return;
  }

  for (int i = 0; i < data->num_zones; i++) {
    if (data->znssd_data[i].d.zs == 2) {  // Full
      full_zones++;
    } else if (data->znssd_data[i].d.zs == 0) {  // Empty
      empty_zones++;
    }
  }

  // Update history data
  update_history(&histories->free_line_history, empty_zones, "Empty Zones");
  update_history(&histories->full_line_history, full_zones, "Full Zones");

  uint64_t avg_wp = 0;
  if (data->num_zones > 0) {
    uint64_t total_wp = 0;
    for (int i = 0; i < data->num_zones; i++) {
      total_wp += data->znssd_data[i].w_ptr - data->znssd_data[i].d.zslba;
    }
    avg_wp = total_wp / data->num_zones;
  }
}

/**
 * Render znssd-specific UI elements
 *
 * @param win Window to render to
 * @param device Device context
 * @param row Starting row
 * @param col Starting column
 * @param width Available width
 * @param height Available height
 * @return Next row after rendered elements
 */
int znssd_mode_render(WINDOW *win, device_context_t *device, int row, int col,
                      int width, int height) {
  znssd_data_t *data = (znssd_data_t *)device->device_data;
  int next_row = row;

  // Count zones by state
  //    int open_zones = data->current_open_zones;
  int full_zones = 0;
  int empty_zones = 0;

  for (int i = 0; i < data->num_zones; i++) {
    if (data->znssd_data[i].d.zs == 2) {  // Full
      full_zones++;
    } else if (data->znssd_data[i].d.zs == 0) {  // Empty
      empty_zones++;
    }
  }

  // Draw ZNSSD status box
  draw_box_with_title(win, next_row, col, 7, width, "ZNSSD Status",
                      COLOR_PAIR_NORMAL);

  // Zone information
  mvwprintw(win, next_row + 2, col + 2,
            "Total Zones: %lu  Zone Size: %llu sectors", data->num_zones,
            data->znssd_data[0].d.zcap);

  // JM: MAX OPEN ZONE 정보도 고려해볼 것.

  // Zone counts
  mvwprintw(win, next_row + 3, col + 2, "Empty Zones: ");
  draw_bar(win, next_row + 3, col + 15, empty_zones, data->num_zones, 30,
           COLOR_PAIR_GREEN);

  //    mvwprintw(win, next_row + 4, col + 2, "Open Zones: ");
  //    draw_bar(win, next_row + 4, col + 15, open_zones, data->max_zones, 30,
  //             COLOR_PAIR_YELLOW);

  mvwprintw(win, next_row + 5, col + 2, "Full Zones: ");
  draw_bar(win, next_row + 5, col + 15, full_zones, data->num_zones, 30,
           COLOR_PAIR_RED);

  next_row += 7;  // Include margin

  return next_row;
}