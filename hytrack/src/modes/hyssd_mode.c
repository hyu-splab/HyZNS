/**
 * hyssd_mode.c
 * HYSSD Mode Implementation
 */

#include "../../include/modes/hyssd_mode.h"

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/common/history.h"
#include "../../include/common/ui_common.h"
#include "../../include/core/nvme_cmd.h"
#include "../../include/views/debug_view.h"

/**
 * Initialize HYSSD mode for a device
 *
 * @param device Device context to initialize
 * @return 0 on success, non-zero on failure
 */
int hyssd_mode_init(device_context_t *device) {
  hyssd_data_t *data;

  // Allocate device data
  data = (hyssd_data_t *)malloc(sizeof(hyssd_data_t));
  if (!data) {
    debug_log_add("Failed to allocate HYSSD data structure");
    return -1;
  }

  // Initialize data
  memset(data, 0, sizeof(hyssd_data_t));
  data->lm_info = NULL;
  data->zone_info = NULL;
  data->num_zones = 0;
  data->allocated_zones = 0;

  // Store data in device context
  device->device_data = data;
  device->data_size = sizeof(hyssd_data_t);

  debug_log_add("HYSSD mode initialized for %s", device->device_path);
  return 0;
}

/**
 * Clean up HYSSD mode resources
 *
 * @param device Device context to clean up
 */
void hyssd_mode_cleanup(device_context_t *device) {
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;

  if (data) {
    // Free line management info
    if (data->lm_info) {
      free(data->lm_info);
      data->lm_info = NULL;
    }

    // Free zone info
    if (data->zone_info) {
      free(data->zone_info);
      data->zone_info = NULL;
    }

    // Free device data
    free(data);
    device->device_data = NULL;
  }
}

/**
 * Get HYSSD data from device
 *
 * @param device Device context
 * @return 0 on success, non-zero on failure
 */
int hyssd_mode_get_data(device_context_t *device) {
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;
  int ret;

  // Get data directly from the device using device_info for exact buffer size
  ret = nvme_get_hyssd_data(data, device->device_path, device->device_info);

  if (ret == 0) {
    //  debug_log_add("Successfully retrieved HYSSD data: %d lines, %d zones",
    //                data->lm_info->tt_lines, data->num_zones);
  } else {
    debug_log_add("Failed to get HYSSD data from device, error: %d", ret);
  }

  return ret;
}

/**
 * Update HYSSD history data
 *
 * @param device Device context
 */
void hyssd_mode_update_history(device_context_t *device) {
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;
  history_set_t *histories = device->histories;

  // Make sure we have valid data
  if (!data || !data->lm_info) {
    debug_log_add("Cannot update history: no valid data available");
    return;
  }

  int rzone_count = 0;
  int active_rzone = 0;
  int active_szone = 0;
  int i;

  // Count RZones and active zones
  for (i = 0; i < data->lm_info->tt_lines; i++) {
    if (data->lm_info->lines[i].is_rnd) {
      rzone_count++;
      if (data->lm_info->lines[i].ipc > 0 || data->lm_info->lines[i].vpc > 0) {
        active_rzone++;
      }
    } else {
      if (data->lm_info->lines[i].ipc > 0 || data->lm_info->lines[i].vpc > 0) {
        active_szone++;
      }
    }
  }

  // Update history data
  update_history(&histories->free_line_history, data->lm_info->free_line_cnt,
                 NULL);
  update_history(&histories->victim_line_history,
                 data->lm_info->victim_line_cnt, NULL);
  update_history(&histories->full_line_history, data->lm_info->full_line_cnt,
                 NULL);
  update_history(&histories->rzone_history, rzone_count, NULL);
  update_history(&histories->active_rzone_history, active_rzone, NULL);
  update_history(&histories->active_szone_history, active_szone, NULL);

  // Update HYSSD specific history
  update_history(&histories->mode.hyssd.hyssd_wp_history,
                 data->wp_info.curline_id, "Write Pointer Line");

  // Count ZNS zones and update history
  int total_zones = data->num_zones;  // Use the actual zone count
  int open_zones = 0;
  int full_zones = 0;
  int rand_zones = 0;

  // Count through the zones
  for (i = 0; i < total_zones; i++) {
    if (data->zone_info[i].rnd) {
      rand_zones++;
    }

    // Zone is open if write pointer is not at start or end
    if (data->zone_info[i].w_ptr > data->zone_info[i].d.zslba &&
        data->zone_info[i].w_ptr <
            (data->zone_info[i].d.zslba + data->zone_info[i].d.zcap)) {
      open_zones++;
    }

    // Zone is full if write pointer reached the end
    if (data->zone_info[i].w_ptr >=
        (data->zone_info[i].d.zslba + data->zone_info[i].d.zcap)) {
      full_zones++;
    }
  }

  // Assuming these history types are already defined in the history structure
  if (histories->mode.hyssd.zns_total_zones_history.data) {
    update_history(&histories->mode.hyssd.zns_total_zones_history, total_zones,
                   "Total Zones");
    update_history(&histories->mode.hyssd.zns_open_zones_history, open_zones,
                   "Open Zones");
    update_history(&histories->mode.hyssd.zns_full_zones_history, full_zones,
                   "Full Zones");
    update_history(&histories->mode.hyssd.zns_rand_zones_history, rand_zones,
                   "Random Zones");
  }
}

/**
 * Render HYSSD-specific UI elements
 *
 * @param win Window to render to
 * @param device Device context
 * @param row Starting row
 * @param col Starting column
 * @param width Available width
 * @param height Available height
 * @return Next row after rendered elements
 */
int hyssd_mode_render(WINDOW *win, device_context_t *device, int row, int col,
                      int width, int height) {
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;
  int next_row = row;

  // Check for valid data
  if (!data || !data->lm_info) {
    mvwprintw(win, next_row + 2, col + 2, "No HYSSD data available");
    return next_row + 4;
  }

  hyssd_line_mgmt_info_t *info = data->lm_info;
  hyssd_write_pointer_info_t *wp_info = &data->wp_info;

  // Draw HYSSD status box
  draw_box_with_title(win, next_row, col, 7, width, "HYSSD Status",
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

  // Count RZones
  int rzone_count = 0;
  for (int i = 0; i < info->tt_lines; i++) {
    if (info->lines[i].is_rnd) {
      rzone_count++;
    }
  }

  mvwprintw(win, next_row + 4, col + 2, "RZone Lines: %d", rzone_count);
  mvwprintw(win, next_row + 4, col + 30, "SZone Lines: %d",
            info->tt_lines - rzone_count);

  // Write pointer info
  mvwprintw(win, next_row + 5, col + 2,
            "Write Pointer: Line %d (Ch:%d Lun:%d Pg:%d Blk:%d Pl:%d)",
            wp_info->curline_id, wp_info->ch, wp_info->lun, wp_info->pg,
            wp_info->blk, wp_info->pl);

  next_row += 7;  // Include margin

  // Draw ZNS status box if we have height available
  if (next_row + 7 <= row + height && data->num_zones > 0) {
    // Count zones
    int total_zones = data->num_zones;
    int open_zones = 0;
    int full_zones = 0;
    int rand_zones = 0;

    // Count through the zones
    for (int i = 0; i < total_zones; i++) {
      if (data->zone_info[i].rnd) {
        rand_zones++;
      }

      // Zone is open if write pointer is not at start or end
      if (data->zone_info[i].w_ptr > data->zone_info[i].d.zslba &&
          data->zone_info[i].w_ptr <
              (data->zone_info[i].d.zslba + data->zone_info[i].d.zcap)) {
        open_zones++;
      }

      // Zone is full if write pointer reached the end
      if (data->zone_info[i].w_ptr >=
          (data->zone_info[i].d.zslba + data->zone_info[i].d.zcap)) {
        full_zones++;
      }
    }

    draw_box_with_title(win, next_row, col, 7, width, "ZNS Status",
                        COLOR_PAIR_NORMAL);

    // Zone counts
    mvwprintw(win, next_row + 2, col + 2, "Total Zones: %d", total_zones);
    mvwprintw(win, next_row + 2, col + 30, "Open Zones: ");
    draw_bar(win, next_row + 2, col + 42, open_zones, total_zones, 20,
             COLOR_PAIR_GREEN);

    mvwprintw(win, next_row + 3, col + 2, "Random Zones: %d", rand_zones);
    mvwprintw(win, next_row + 3, col + 30, "Full Zones: ");
    draw_bar(win, next_row + 3, col + 42, full_zones, total_zones, 20,
             COLOR_PAIR_RED);

    // Display info about most recently active zone if available
    if (total_zones > 0) {
      // Find the zone with highest write pointer value (most recently active)
      int most_active_idx = 0;
      uint64_t highest_wp = 0;

      for (int i = 0; i < total_zones; i++) {
        if (data->zone_info[i].w_ptr > highest_wp) {
          highest_wp = data->zone_info[i].w_ptr;
          most_active_idx = i;
        }
      }

      // Display active zone info
      hyssd_zone_info_t *active_zone = &data->zone_info[most_active_idx];
      mvwprintw(win, next_row + 4, col + 2,
                "Active Zone: %d (SLBA: 0x%lx, Cap: %lu, Type: %s)",
                most_active_idx, active_zone->d.zslba, active_zone->d.zcap,
                active_zone->rnd ? "Random" : "Sequential");

      // Calculate zone usage percentage
      uint64_t zone_used = 0;
      if (active_zone->w_ptr > active_zone->d.zslba) {
        zone_used = active_zone->w_ptr - active_zone->d.zslba;
      }

      double usage_pct = 0;
      if (active_zone->d.zcap > 0) {
        usage_pct = ((double)zone_used / active_zone->d.zcap) * 100.0;
      }

      mvwprintw(win, next_row + 5, col + 2,
                "Zone Usage: %.2f%% (WP: 0x%lx, Used: %lu / %lu)", usage_pct,
                active_zone->w_ptr, zone_used, active_zone->d.zcap);
    }

    next_row += 7;  // Include margin
  }

  return next_row;
}