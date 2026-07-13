/**
 * bbssd_mode.c
 * BBSSD Mode Implementation
 */

#include "../../include/modes/bbssd_mode.h"

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/common/history.h"
#include "../../include/common/ui_common.h"
#include "../../include/core/nvme_cmd.h"
#include "../../include/views/debug_view.h"

/**
 * Initialize BBSSD mode for a device
 *
 * @param device Device context to initialize
 * @return 0 on success, non-zero on failure
 */
int bbssd_mode_init(device_context_t *device) {
  bbssd_data_t *data;

  // Allocate device data
  data = (bbssd_data_t *)malloc(sizeof(bbssd_data_t));
  if (!data) {
    debug_log_add("Failed to allocate BBSSD data structure");
    return -1;
  }

  // Initialize data
  memset(data, 0, sizeof(bbssd_data_t));
  data->lm_info = NULL;

  // Store data in device context
  device->device_data = data;
  device->data_size = sizeof(bbssd_data_t);

  debug_log_add("BBSSD mode initialized for %s", device->device_path);
  return 0;
}

/**
 * Clean up BBSSD mode resources
 *
 * @param device Device context to clean up
 */
void bbssd_mode_cleanup(device_context_t *device) {
  bbssd_data_t *data = (bbssd_data_t *)device->device_data;

  if (data) {
    // Free line management info
    if (data->lm_info) {
      free(data->lm_info);
      data->lm_info = NULL;
    }

    // Free device data
    free(data);
    device->device_data = NULL;
  }
}

/**
 * Get BBSSD data from device
 *
 * @param device Device context
 * @return 0 on success, non-zero on failure
 */
int bbssd_mode_get_data(device_context_t *device) {
  bbssd_data_t *data = (bbssd_data_t *)device->device_data;
  int ret;

  // Get data directly from the device using the refactored function
  ret = nvme_get_bbssd_data(data, device->device_path, device->device_info);

  if (ret == 0) {
    //  debug_log_add("Successfully retrieved BBSSD data: %d lines",
    //                data->lm_info->tt_lines);
  } else {
    debug_log_add("Failed to get BBSSD data from device, error: %d", ret);
  }

  return ret;
}

/**
 * Update BBSSD history data
 *
 * @param device Device context
 */
void bbssd_mode_update_history(device_context_t *device) {
  bbssd_data_t *data = (bbssd_data_t *)device->device_data;
  history_set_t *histories = device->histories;

  // Make sure we have valid data
  if (!data || !data->lm_info) {
    debug_log_add("Cannot update history: no valid data available");
    return;
  }

  update_history(&histories->free_line_history, data->lm_info->free_line_cnt,
                 NULL);
  update_history(&histories->victim_line_history,
                 data->lm_info->victim_line_cnt, NULL);
  update_history(&histories->full_line_history, data->lm_info->full_line_cnt,
                 NULL);

  // Update BBSSD specific history
  update_history(&histories->mode.bbssd.bbssd_specific_history,
                 data->wp_info.curline_id, "Write Pointer Line");

  // Count blocks by type
  // int free_blocks = data->gc_info.free_blk_count;
  // int victim_blocks = data->gc_info.victim_blk_count;
  // int total_blocks = data->flash_info.num_channels *
  // data->flash_info.num_luns *
  //                    data->flash_info.num_blocks;
  // int full_blocks = total_blocks - free_blocks - victim_blocks;

  // // Update history data
  // update_history(&histories->free_line_history, free_blocks, "Free Blocks");
  // update_history(&histories->victim_line_history, victim_blocks,
  //                "Victim Blocks");
  // update_history(&histories->full_line_history, full_blocks, "Full Blocks");

  // // Update GC specific history
  // update_history(&histories->mode.bbssd.bbssd_specific_history,
  //                data->gc_info.gc_count, "GC Count");
}

/**
 * Render BBSSD-specific UI elements
 *
 * @param win Window to render to
 * @param device Device context
 * @param row Starting row
 * @param col Starting column
 * @param width Available width
 * @param height Available height
 * @return Next row after rendered elements
 */
int bbssd_mode_render(WINDOW *win, device_context_t *device, int row, int col,
                      int width, int height) {
  bbssd_data_t *data = (bbssd_data_t *)device->device_data;

  // Check for valid data
  if (!data || !data->lm_info) {
    mvwprintw(win, row + 2, col + 2, "No BBSSD data available");
    return row + 4;
  }

  bbssd_line_mgmt_info_t *info = data->lm_info;
  bbssd_write_pointer_info_t *wp_info = &data->wp_info;
  int next_row = row;

  // Draw BBSSD status box
  draw_box_with_title(win, next_row, col, 7, width, "BBSSD Status",
                      COLOR_PAIR_NORMAL);

  // Line counts
  mvwprintw(win, next_row + 2, col + 2, "Total Lines: %d", info->tt_lines);
  mvwprintw(win, next_row + 2, col + 30, "Free Lines: ");
  draw_bar(win, next_row + 2, col + 42, info->free_line_cnt, info->tt_lines, 20,
           COLOR_PAIR_GREEN);

  mvwprintw(win, next_row + 3, col + 2, "Victim Lines: %d",
            info->victim_line_cnt);
  mvwprintw(win, next_row + 3, col + 30, "Full Lines: ");
  draw_bar(win, next_row + 3, col + 42, info->full_line_cnt, info->tt_lines, 20,
           COLOR_PAIR_RED);

  // Write pointer info
  mvwprintw(win, next_row + 5, col + 2,
            "Write Pointer: Line %d (Ch:%d Lun:%d Pg:%d Blk:%d Pl:%d)",
            wp_info->curline_id, wp_info->ch, wp_info->lun, wp_info->pg,
            wp_info->blk, wp_info->pl);

  next_row += 7;  // Include margin

  return next_row;
}