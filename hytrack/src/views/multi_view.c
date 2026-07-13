/**
 * multi_view.c
 * Multi-device View Implementation
 */

#include "../../include/views/multi_view.h"

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/common/ui_common.h"
#include "../../include/core/device_manager.h"
#include "../../include/modes/bbssd_mode.h"
#include "../../include/modes/multi_mode.h"
#include "../../include/modes/znssd_mode.h"

// Forward declarations of private functions
static int multi_view_init(void);
static int multi_view_activate(void);
static int multi_view_deactivate(void);
static int multi_view_render(WINDOW *win, device_manager_t *manager, int width,
                             int height);
static int multi_view_handle_input(int ch, device_manager_t *manager);
static void multi_view_cleanup(void);

// Local variables
static int multi_view_initialized = 0;
static int multi_view_selected_device = 0;

// View interface implementation
view_interface_t multi_view = {
    .name = "Multi-Dev",
    .description = "Multi-device overview for BBSSD+ZNSSD mode",
    .key = "6",
    .init = multi_view_init,
    .activate = multi_view_activate,
    .deactivate = multi_view_deactivate,
    .render = multi_view_render,
    .handle_input = multi_view_handle_input,
    .cleanup = multi_view_cleanup};

/**
 * Draw combined device overview panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Panel width
 * @param height Panel height
 * @param manager Device manager
 * @return Next row after the panel
 */
static int draw_combined_overview(WINDOW *win, int row, int col, int width,
                                  int height, device_manager_t *manager) {
  device_context_t *bbssd_device = &manager->devices[0];
  device_context_t *znssd_device = &manager->devices[1];
  bbssd_data_t *bbssd_data = (bbssd_data_t *)bbssd_device->device_data;
  znssd_data_t *znssd_data = (znssd_data_t *)znssd_device->device_data;

  // Draw box with title
  draw_box_with_title(win, row, col, height, width,
                      "BBSSD+ZNSSD Combined Overview", COLOR_PAIR_NORMAL);

  row += 2;
  col += 2;

  // Device names and status
  mvwprintw(win, row, col, "BBSSD Device: %s", bbssd_device->device_path);
  mvwprintw(win, row, col + width / 2, "ZNSSD Device: %s",
            znssd_device->device_path);
  row++;

  mvwprintw(win, row, col, "Status: %s",
            bbssd_device->status == 0 ? "OK" : "Error");
  mvwprintw(win, row, col + width / 2, "Status: %s",
            znssd_device->status == 0 ? "OK" : "Error");
  row += 2;

  // Draw separator
  draw_separator(win, row, col, width - 4);
  row++;

  // BBSSD Key stats
  mvwprintw(win, row, col, "BBSSD Key Statistics:");
  row++;

  // bbssd_flash_info_t *flash_info = &bbssd_data->flash_info;
  // bbssd_gc_info_t *gc_info = &bbssd_data->gc_info;

  // int total_blocks =
  //     flash_info->num_channels * flash_info->num_luns *
  //     flash_info->num_blocks;
  // int free_blocks = gc_info->free_blk_count;

  // mvwprintw(win, row, col, "Free Blocks: %d/%d (%.1f%%)", free_blocks,
  //           total_blocks,
  //           (total_blocks > 0) ? (100.0 * free_blocks / total_blocks) : 0);
  // row++;

  // mvwprintw(win, row, col, "GC Count: %d, Mode: %s", gc_info->gc_count,
  //           gc_info->gc_mode == 0
  //               ? "No GC"
  //               : gc_info->gc_mode == 1 ? "Passive" : "Aggressive");
  // row += 2;

  // ZNSSD Key stats
  mvwprintw(win, row, col, "ZNSSD Key Statistics:");
  row++;

  // Count zones by state
  // int open_zones = znssd_data->current_open_zones;
  // int full_zones = 0;
  // int empty_zones = 0;

  // for (int i = 0; i < znssd_data->zone_count; i++) {
  //   if (znssd_data->zone_info[i].state == 2) {  // Full
  //     full_zones++;
  //   } else if (znssd_data->zone_info[i].state == 0) {  // Empty
  //     empty_zones++;
  //   }
  // }

  // mvwprintw(win, row, col, "Zones: %d empty, %d open, %d full of %d total",
  //           empty_zones, open_zones, full_zones, znssd_data->max_zones);
  // row++;

  // // Calculate utilization
  // uint64_t total_capacity = znssd_data->zone_size * znssd_data->max_zones;
  // uint64_t used_capacity = 0;

  // for (int i = 0; i < znssd_data->zone_count; i++) {
  //   znssd_zone_info_t *zone = &znssd_data->zone_info[i];
  //   if (zone->state == 1) {  // Open
  //     used_capacity += (zone->wp - zone->start_lba);
  //   } else if (zone->state == 2) {  // Full
  //     used_capacity += zone->capacity;
  //   }
  // }

  // double utilization =
  //     (total_capacity > 0) ? (100.0 * used_capacity / total_capacity) : 0;

  // mvwprintw(win, row, col, "Utilization: %.1f%% (%llu of %llu sectors)",
  //           utilization, used_capacity, total_capacity);
  // row += 2;

  // Draw hybrid operation info
  draw_separator(win, row, col, width - 4);
  row++;

  mvwprintw(win, row, col, "Hybrid SSD Operation Mode: BBSSD+ZNSSD");
  row++;

  // Here you would add specific hybrid operation metrics that show how
  // the two devices are working together

  // Help text
  mvwprintw(win, row + 3, col, "Press TAB to switch between devices");

  return row + height - 3;  // Include margin
}

/**
 * Draw trends panel for multi-device view
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Panel width
 * @param height Panel height
 * @param manager Device manager
 * @return Next row after the panel
 */
static int draw_multi_trends(WINDOW *win, int row, int col, int width,
                             int height, device_manager_t *manager) {
  device_context_t *device = &manager->devices[multi_view_selected_device];
  history_set_t *histories = device->histories;

  // Draw box with title
  char title[128];
  snprintf(title, sizeof(title), "%s Device Trends",
           multi_view_selected_device == 0 ? "BBSSD" : "ZNSSD");
  draw_box_with_title(win, row, col, height, width, title, COLOR_PAIR_NORMAL);

  row += 2;
  col += 2;

  // Calculate graph dimensions
  int graph_height = 5;
  int graph_width = (width - 6) < HISTORY_SIZE ? width - 6 : HISTORY_SIZE;

  // Draw appropriate graphs based on selected device
  if (multi_view_selected_device == 0) {  // BBSSD
    // Free blocks trend
    row = draw_sparkline(win, row, col, &histories->free_line_history,
                         graph_height, graph_width, NULL, COLOR_PAIR_GREEN,
                         manager->view_state.show_grid);

    // Victim blocks trend
    row = draw_sparkline(win, row, col, &histories->victim_line_history,
                         graph_height, graph_width, NULL, COLOR_PAIR_YELLOW,
                         manager->view_state.show_grid);
  } else {  // ZNSSD
    // Empty zones trend
    row = draw_sparkline(win, row, col, &histories->free_line_history,
                         graph_height, graph_width, NULL, COLOR_PAIR_GREEN,
                         manager->view_state.show_grid);

    // Open zones trend
    row = draw_sparkline(win, row, col, &histories->active_rzone_history,
                         graph_height, graph_width, NULL, COLOR_PAIR_YELLOW,
                         manager->view_state.show_grid);

    // Zone utilization trend
    row = draw_sparkline(win, row, col, &histories->zns_slba_history,
                         graph_height, graph_width, NULL, COLOR_PAIR_BLUE,
                         manager->view_state.show_grid);
  }

  // Help text
  mvwprintw(win, row + 1, col,
            "Press Space to toggle grid, 's' to switch device trends");

  return row + 3;  // Include margin
}

/**
 * Initialize multi-device view
 *
 * @return 0 on success, non-zero on failure
 */
static int multi_view_init(void) {
  if (multi_view_initialized) {
    return 0;
  }

  multi_view_selected_device = 0;
  multi_view_initialized = 1;
  return 0;
}

/**
 * Activate multi-device view
 *
 * @return 0 on success, non-zero on failure
 */
static int multi_view_activate(void) {
  // Reset to the first device
  multi_view_selected_device = 0;
  return 0;
}

/**
 * Deactivate multi-device view
 *
 * @return 0 on success, non-zero on failure
 */
static int multi_view_deactivate(void) {
  // Nothing specific to do on deactivation
  return 0;
}

/**
 * Render multi-device view
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param width Window width
 * @param height Window height
 * @return 0 on success, non-zero on failure
 */
static int multi_view_render(WINDOW *win, device_manager_t *manager, int width,
                             int height) {
  int next_row;

  // Check if this is the correct mode
  if (manager->mode != MODE_MULTI) {
    mvwprintw(win, 3, 1,
              "Multi-device view is only available in BBSSD+ZNSSD mode");
    return 0;
  }

  // Calculate layout
  width = width - 2;  // Adjust for padding
  next_row = 3;       // Start below the header

  // For multi-device mode, add an extra row for device selector
  if (manager->device_count > 1) {
    next_row = 4;
  }

  // Draw combined overview panel
  int overview_height = 14;
  next_row =
      draw_combined_overview(win, next_row, 1, width, overview_height, manager);

  // Draw trends panel for the selected device
  int trends_height = height - next_row - 1;
  if (trends_height >= 12) {
    draw_multi_trends(win, next_row, 1, width, trends_height, manager);
  }

  return 0;
}

/**
 * Handle input for multi-device view
 *
 * @param ch Input character
 * @param manager Device manager
 * @return 1 if the input was handled, 0 if not
 */
static int multi_view_handle_input(int ch, device_manager_t *manager) {
  // Handle view-specific input
  switch (ch) {
    case ' ':  // Toggle grid
      manager->view_state.show_grid = !manager->view_state.show_grid;
      return 1;

    case 's':  // Switch device for trends
    case 'S':
      multi_view_selected_device =
          (multi_view_selected_device + 1) % manager->device_count;
      return 1;

    case KEY_UP:
    case KEY_DOWN:
    case KEY_PPAGE:
    case KEY_NPAGE:
      // Future scrolling support
      return 1;
  }

  return 0;  // Input not handled
}

/**
 * Clean up resources used by multi-device view
 */
static void multi_view_cleanup(void) {
  // Clean up any resources
  multi_view_initialized = 0;
}