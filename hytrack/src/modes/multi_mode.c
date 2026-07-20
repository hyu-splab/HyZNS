/**
 * multi_mode.c
 * Multi-device Mode (BBSSD+ZNSSD) Implementation
 */

#include "../../include/modes/multi_mode.h"

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/common/history.h"
#include "../../include/common/ui_common.h"
#include "../../include/core/nvme_cmd.h"
#include "../../include/views/debug_view.h"

/**
 * Initialize multi-device mode
 *
 * @param manager Device manager to initialize
 * @return 0 on success, non-zero on failure
 */
int multi_mode_init(device_manager_t *manager) {
  // Need at least 2 devices
  if (manager->device_count < 2) {
    debug_log_add("Multi-device mode requires at least 2 devices");
    return -1;
  }

  // First device is already initialized as BBSSD in device_manager_init

  // Initialize second device as ZNSSD
  device_context_t *znssd_device = &manager->devices[1];
  if (znssd_mode_init(znssd_device) != 0) {
    debug_log_add("Failed to initialize ZNSSD mode for device %s",
                  znssd_device->device_path);
    return -1;
  }
  debug_log_add("Multi-device mode initialized");
  return 0;
}

/**
 * Clean up multi-device resources
 *
 * @param manager Device manager to clean up
 */
void multi_mode_cleanup(device_manager_t *manager) {
  // Clean up BBSSD device
  bbssd_mode_cleanup(&manager->devices[0]);
  // Clean up ZNSSD device
  znssd_mode_cleanup(&manager->devices[1]);
}

/**
 * Update multi-device history data
 *
 * @param device Device context
 */
void multi_mode_update_history(device_manager_t *manager) {
  bbssd_mode_update_history(&manager->devices[0]);
  znssd_mode_update_history(&manager->devices[1]);
}

/**
 * Render multi-device overview
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param row Starting row
 * @param col Starting column
 * @param width Available width
 * @param height Available height
 * @return Next row after rendered elements
 */
int multi_mode_render_overview(WINDOW *win, device_manager_t *manager, int row,
                               int col, int width, int height) {
  int next_row = row;

  // Draw overview box
  draw_box_with_title(win, next_row, col, 5, width,
                      "Multi-Device Mode Overview", COLOR_PAIR_NORMAL);

  // Show basic info about both devices
  mvwprintw(win, next_row + 2, col + 2, "BBSSD Device: %s",
            manager->devices[0].device_path);
  mvwprintw(win, next_row + 3, col + 2, "ZNSSD Device: %s",
            manager->devices[1].device_path);

  next_row += 5;  // Include margin

  // Draw BBSSD and ZNSSD status boxes side by side if there's enough space
  if (width >= 100) {
    int half_width = width / 2 - 2;

    // Draw BBSSD status on the left
    bbssd_mode_render(win, &manager->devices[0], next_row, col, half_width,
                      height - next_row);

    // Draw ZNSSD status on the right
    znssd_mode_render(win, &manager->devices[1], next_row, col + half_width + 2,
                      half_width, height - next_row);

    // Calculate next row based on the larger of the two
    int bbssd_next_row = next_row + 7;  // From bbssd_mode_render
    int znssd_next_row = next_row + 7;  // From znssd_mode_render
    next_row =
        (bbssd_next_row > znssd_next_row) ? bbssd_next_row : znssd_next_row;
  } else {
    // Not enough space, draw one after the other
    next_row = bbssd_mode_render(win, &manager->devices[0], next_row, col,
                                 width, height - next_row);
    next_row = znssd_mode_render(win, &manager->devices[1], next_row, col,
                                 width, height - next_row);
  }

  return next_row;
}