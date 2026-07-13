/**
 * stats_view.c
 * Statistics View Implementation
 */

#include "../../include/views/stats_view.h"

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/common/ui_common.h"
#include "../../include/core/device_manager.h"
#include "../../include/modes/bbssd_mode.h"
#include "../../include/modes/hyssd_mode.h"
#include "../../include/modes/multi_mode.h"
#include "../../include/modes/znssd_mode.h"

// Forward declarations of private functions
static int stats_view_init(void);
static int stats_view_activate(void);
static int stats_view_deactivate(void);
static int stats_view_render(WINDOW *win, device_manager_t *manager, int width,
                             int height);
static int stats_view_handle_input(int ch, device_manager_t *manager);
static void stats_view_cleanup(void);

// Local variables
static int stats_view_initialized = 0;
static int stats_view_show_detail =
    0;  // Toggle between summary and detailed view

// View interface implementation
view_interface_t stats_view = {
    .name = "Statistics",
    .description = "Detailed statistics and distribution view",
    .key = "2",
    .init = stats_view_init,
    .activate = stats_view_activate,
    .deactivate = stats_view_deactivate,
    .render = stats_view_render,
    .handle_input = stats_view_handle_input,
    .cleanup = stats_view_cleanup};

/**
 * Draw HYSSD zone statistics panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param device Device context
 * @return Next row after the panel
 */
static int draw_hyssd_zone_stats(WINDOW *win, int row, int col,
                                 device_context_t *device) {
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;

  int rzone_count = 0, szone_count = 0;
  int open_rzones = 0, full_rzones = 0, empty_rzones = 0;
  int open_szones = 0, full_szones = 0, empty_szones = 0;

  // Count zones by type and state
  for (int i = 0; i < data->num_zones; i++) {
    hyssd_zone_info_t *zone = &data->zone_info[i];

    if (zone->rnd) {
      // Random Zone (BBSSD)
      rzone_count++;

      // Check zone state
      if (zone->w_ptr > zone->d.zslba &&
          zone->w_ptr < (zone->d.zslba + zone->d.zcap)) {
        open_rzones++;
      } else if (zone->w_ptr >= (zone->d.zslba + zone->d.zcap)) {
        full_rzones++;
      } else {
        empty_rzones++;
      }
    } else {
      // Sequential Zone (ZNS)
      szone_count++;

      // Check zone state based on ZNS state field (zs)
      uint8_t state = zone->d.zs >> 4;  // Extract state from the zs field

      if (state == 2 || state == 3) {  // Open state (implicit or explicit)
        open_szones++;
      } else if (state == 14) {  // Full state
        full_szones++;
      } else if (state == 1) {  // Empty state
        empty_szones++;
      }
    }
  }

  // Draw the box
  draw_box_with_title(win, row, col, 13, 72, "HYSSD Zone Statistics",
                      COLOR_PAIR_NORMAL);

  // Draw the stats
  row += 2;
  col += 2;

  // Zone counts
  mvwprintw(win, row, col, "Random Zones (BBSSD): %d", rzone_count);
  mvwprintw(win, row, col + 36, "Sequential Zones (ZNS): %d", szone_count);

  row += 2;
  draw_separator(win, row, col, 68);
  row++;

  // RZone statistics
  wattron(win, COLOR_PAIR_BLUE);
  mvwprintw(win, row, col, "Random Zone States:");
  wattroff(win, COLOR_PAIR_BLUE);
  row++;

  mvwprintw(win, row, col, "  Empty: %d (%.1f%%)", empty_rzones,
            rzone_count > 0 ? (100.0 * empty_rzones / rzone_count) : 0);
  mvwprintw(win, row, col + 36, "  Open: %d (%.1f%%)", open_rzones,
            rzone_count > 0 ? (100.0 * open_rzones / rzone_count) : 0);
  row++;

  mvwprintw(win, row, col, "  Full: %d (%.1f%%)", full_rzones,
            rzone_count > 0 ? (100.0 * full_rzones / rzone_count) : 0);

  // SZone statistics
  row += 2;
  wattron(win, COLOR_PAIR_CYAN);
  mvwprintw(win, row, col, "Sequential Zone States:");
  wattroff(win, COLOR_PAIR_CYAN);
  row++;

  mvwprintw(win, row, col, "  Empty: %d (%.1f%%)", empty_szones,
            szone_count > 0 ? (100.0 * empty_szones / szone_count) : 0);
  mvwprintw(win, row, col + 36, "  Open: %d (%.1f%%)", open_szones,
            szone_count > 0 ? (100.0 * open_szones / szone_count) : 0);
  row++;

  mvwprintw(win, row, col, "  Full: %d (%.1f%%)", full_szones,
            szone_count > 0 ? (100.0 * full_szones / szone_count) : 0);

  // Add zone utilization info
  row += 2;
  draw_separator(win, row, col, 68);
  row++;

  // Calculate utilization for both zone types
  uint64_t total_rzone_capacity = 0, used_rzone_capacity = 0;
  uint64_t total_szone_capacity = 0, used_szone_capacity = 0;

  for (int i = 0; i < data->num_zones; i++) {
    hyssd_zone_info_t *zone = &data->zone_info[i];
    uint64_t capacity = zone->d.zcap;
    uint64_t used = 0;

    if (zone->w_ptr > zone->d.zslba) {
      used = zone->w_ptr - zone->d.zslba;
    }

    if (zone->rnd) {
      total_rzone_capacity += capacity;
      used_rzone_capacity += used;
    } else {
      total_szone_capacity += capacity;
      used_szone_capacity += used;
    }
  }

  double rzone_util = (total_rzone_capacity > 0)
                          ? (100.0 * used_rzone_capacity / total_rzone_capacity)
                          : 0;
  double szone_util = (total_szone_capacity > 0)
                          ? (100.0 * used_szone_capacity / total_szone_capacity)
                          : 0;

  mvwprintw(win, row, col, "Random Zone Utilization: %.1f%%", rzone_util);
  mvwprintw(win, row, col + 36, "Sequential Zone Utilization: %.1f%%",
            szone_util);

  return row + 5;  // Include margin
}

/**
 * Draw HYSSD basic statistics panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param device Device context
 * @return Next row after the panel
 */
static int draw_hyssd_basic_stats(WINDOW *win, int row, int col,
                                  device_context_t *device) {
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;
  hyssd_line_mgmt_info_t *info = data->lm_info;
  int rzone_count = 0, active_rzone = 0, active_szone = 0;
  int i;
  int total_active_lines = 0;
  int total_ipc = 0, total_vpc = 0;
  double avg_ipc = 0.0, avg_vpc = 0.0;
  int active_percent = 0;

  // Count RZones and active zones, calculate totals
  for (i = 0; i < info->tt_lines; i++) {
    // Count invalid and valid pages
    total_ipc += info->lines[i].ipc;
    total_vpc += info->lines[i].vpc;

    // Count active lines
    if (info->lines[i].ipc > 0 || info->lines[i].vpc > 0) {
      total_active_lines++;
    }

    // Count zone types
    if (info->lines[i].is_rnd) {
      rzone_count++;
      if (info->lines[i].ipc > 0 || info->lines[i].vpc > 0) {
        active_rzone++;
      }
    } else {
      if (info->lines[i].ipc > 0 || info->lines[i].vpc > 0) {
        active_szone++;
      }
    }
  }

  // Calculate averages and percentages
  if (total_active_lines > 0) {
    avg_ipc = (double)total_ipc / total_active_lines;
    avg_vpc = (double)total_vpc / total_active_lines;
  }

  if (info->tt_lines > 0) {
    active_percent = (total_active_lines * 100) / info->tt_lines;
  }

  // Draw the box
  draw_box_with_title(win, row, col, 13, 72, "HYSSD Statistics",
                      COLOR_PAIR_NORMAL);

  // Draw the stats
  row += 2;
  col += 2;

  // Line counts
  mvwprintw(win, row, col, "Total Lines:       %d", info->tt_lines);
  mvwprintw(win, row, col + 36, "Free Lines:        %d", info->free_line_cnt);

  row++;
  mvwprintw(win, row, col, "Victim Lines:      %d", info->victim_line_cnt);
  mvwprintw(win, row, col + 36, "Full Lines:        %d", info->full_line_cnt);

  row++;
  mvwprintw(win, row, col, "Total Active Lines: %d (%d%%)", total_active_lines,
            active_percent);

  // Zone information
  row += 2;
  draw_separator(win, row, col, 68);
  row++;

  mvwprintw(win, row, col, "Random Zones:      %d", rzone_count);
  mvwprintw(win, row, col + 36, "Active RZones:      %d", active_rzone);

  row++;
  mvwprintw(win, row, col, "Sequential Zones:  %d",
            info->tt_lines - rzone_count);
  mvwprintw(win, row, col + 36, "Active SZones:      %d", active_szone);

  // IPC/VPC information
  row += 2;
  draw_separator(win, row, col, 68);
  row++;

  mvwprintw(win, row, col, "Total IPC:         %d", total_ipc);
  mvwprintw(win, row, col + 36, "Avg IPC/Active:     %.2f", avg_ipc);

  row++;
  mvwprintw(win, row, col, "Total VPC:         %d", total_vpc);
  mvwprintw(win, row, col + 36, "Avg VPC/Active:     %.2f", avg_vpc);

  // 추가: zone 통계 패널 호출
  row = draw_hyssd_zone_stats(win, row + 2, col - 2, device);

  return row;  // Include margin
}

/**
 * Draw BBSSD basic statistics panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param device Device context
 * @return Next row after the panel
 */
static int draw_bbssd_basic_stats(WINDOW *win, int row, int col,
                                  device_context_t *device) {
  bbssd_data_t *data = (bbssd_data_t *)device->device_data;
  bbssd_line_mgmt_info_t *info = data->lm_info;

  int rzone_count = info->tt_lines, active_rzone = 0;
  int i;
  int total_active_lines = 0;
  int total_ipc = 0, total_vpc = 0;
  double avg_ipc = 0.0, avg_vpc = 0.0;
  int active_percent = 0;

  // Count RZones and active zones, calculate totals
  for (i = 0; i < info->tt_lines; i++) {
    // Count invalid and valid pages
    total_ipc += info->lines[i].ipc;
    total_vpc += info->lines[i].vpc;

    // Count active lines
    if (info->lines[i].ipc > 0 || info->lines[i].vpc > 0) {
      total_active_lines++;
    }
  }

  // Calculate averages and percentages
  if (total_active_lines > 0) {
    avg_ipc = (double)total_ipc / total_active_lines;
    avg_vpc = (double)total_vpc / total_active_lines;
  }

  if (info->tt_lines > 0) {
    active_percent = (total_active_lines * 100) / info->tt_lines;
  }

  // Draw the box
  draw_box_with_title(win, row, col, 13, 72, "BBSSD Statistics",
                      COLOR_PAIR_NORMAL);

  // Draw the stats
  row += 2;
  col += 2;

  // Line counts
  mvwprintw(win, row, col, "Total Lines:       %d", info->tt_lines);
  mvwprintw(win, row, col + 36, "Free Lines:        %d", info->free_line_cnt);

  row++;
  mvwprintw(win, row, col, "Victim Lines:      %d", info->victim_line_cnt);
  mvwprintw(win, row, col + 36, "Full Lines:        %d", info->full_line_cnt);

  row++;
  mvwprintw(win, row, col, "Total Active Lines: %d (%d%%)", total_active_lines,
            active_percent);

  // Zone information
  row += 2;
  draw_separator(win, row, col, 68);
  row++;

  mvwprintw(win, row, col, "Random Zones:      %d", rzone_count);
  mvwprintw(win, row, col + 36, "Active RZones:      %d", active_rzone);

  // IPC/VPC information
  row += 2;
  draw_separator(win, row, col, 68);
  row++;

  mvwprintw(win, row, col, "Total IPC:         %d", total_ipc);
  mvwprintw(win, row, col + 36, "Avg IPC/Active:     %.2f", avg_ipc);

  row++;
  mvwprintw(win, row, col, "Total VPC:         %d", total_vpc);
  mvwprintw(win, row, col + 36, "Avg VPC/Active:     %.2f", avg_vpc);

  // 추가: zone 통계 패널 호출
  row = draw_hyssd_zone_stats(win, row + 2, col - 2, device);

  return row;  // Include margin
}

/**
 * Draw ZNSSD basic statistics panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param device Device context
 * @return Next row after the panel
 */
static int draw_znssd_basic_stats(WINDOW *win, int row, int col,
                                  device_context_t *device) {
  znssd_data_t *data = (znssd_data_t *)device->device_data;

  // Count zones by state
  int open_zones = 0;
  int full_zones = 0;
  int empty_zones = 0;
  int other_zones = 0;

  for (int i = 0; i < data->num_zones; i++) {
    if (data->znssd_data[i].d.zs == 0x0E) {  // Full
      full_zones++;
    } else if (data->znssd_data[i].d.zs == 1) {  // Empty
      empty_zones++;
    } else if (data->znssd_data[i].d.zs == 2 ||
               data->znssd_data[i].d.zs == 3) {  // Open
      open_zones++;
    } else {
      other_zones++;
    }
  }

  // Draw the box
  draw_box_with_title(win, row, col, 13, 72, "ZNSSD Statistics",
                      COLOR_PAIR_NORMAL);

  // Draw the stats
  row += 2;
  col += 2;

  // Zone configuration
  mvwprintw(win, row, col, "Total Zones:   %d", data->num_zones);
  mvwprintw(win, row, col + 36, "Zone Size:     %llu sectors",
            data->znssd_data[0].d.zcap);
  row++;
  // mvwprintw(win, row, col, "Active Zones:  %d", data->active_zones);
  // mvwprintw(win, row, col + 36, "Max Open Zones: %d", data->max_open_zones);

  // row += 2;
  draw_separator(win, row, col, 68);
  row++;

  // Zone status
  mvwprintw(
      win, row, col, "Empty Zones:   %d (%.1f%%)", empty_zones,
      (data->num_zones > 0) ? (100.0 * empty_zones / data->num_zones) : 0);
  mvwprintw(win, row, col + 36, "Full Zones:    %d (%.1f%%)", full_zones,
            (data->num_zones > 0) ? (100.0 * full_zones / data->num_zones) : 0);
  row++;
  mvwprintw(win, row, col, "Open Zones:    %d (%.1f%%)", open_zones,
            (data->num_zones > 0) ? (100.0 * open_zones / data->num_zones) : 0);
  mvwprintw(win, row, col + 36, "Other States:  %d", other_zones);

  // Zone utilization calculation
  row += 2;
  draw_separator(win, row, col, 68);
  row++;

  uint64_t total_capacity = data->znssd_data[0].d.zcap * data->num_zones;
  uint64_t used_capacity = 0;

  for (int i = 0; i < data->num_zones; i++) {
    znssd_zone_info_t *zone = &data->znssd_data[i];
    if (zone->d.zs == 2) {  // Full
      used_capacity += zone->d.zcap;
    } else {
      used_capacity += (zone->d.wp - zone->d.zslba);
    }
  }

  double utilization =
      (total_capacity > 0) ? (100.0 * used_capacity / total_capacity) : 0;

  mvwprintw(win, row, col, "Total Capacity: %llu sectors", total_capacity);
  mvwprintw(win, row, col + 36, "Used Capacity:  %llu sectors", used_capacity);
  row++;
  mvwprintw(win, row, col, "Utilization:    %.1f%%", utilization);

  return row + 5;  // Include margin
}

/**
 * Draw HYSSD line type distribution panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Width of the panel
 * @param device Device context
 * @return Next row after the panel
 */
static int draw_hyssd_line_distribution(WINDOW *win, int row, int col,
                                        int width, device_context_t *device) {
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;
  hyssd_line_mgmt_info_t *info = data->lm_info;
  int i;
  int bar_width = width - 25;
  int tt_lines = info->tt_lines;
  int rzone_count = 0;
  int full_rzone = 0, full_szone = 0;
  int victim_rzone = 0, victim_szone = 0;
  int free_rzone = 0, free_szone = 0;

  // Analyze line distribution
  for (i = 0; i < info->tt_lines; i++) {
    if (info->lines[i].is_rnd) {
      rzone_count++;

      // Check status
      if (info->lines[i].vpc > 0 && info->lines[i].ipc == 0) {
        full_rzone++;
      } else if (info->lines[i].vpc > 0 && info->lines[i].ipc > 0) {
        victim_rzone++;
      } else {
        free_rzone++;
      }
    } else {
      // Check status
      if (info->lines[i].vpc > 0 && info->lines[i].ipc == 0) {
        full_szone++;
      } else if (info->lines[i].vpc > 0 && info->lines[i].ipc > 0) {
        victim_szone++;
      } else {
        free_szone++;
      }
    }
  }

  // Draw box with title
  draw_box_with_title(win, row, col, 15, width, "Line Type Distribution",
                      COLOR_PAIR_NORMAL);

  // Random vs Sequential zones
  row += 2;
  col += 2;

  mvwprintw(win, row, col, "Random Zones:");
  draw_bar(win, row, col + 15, rzone_count, tt_lines, bar_width,
           COLOR_PAIR_BLUE);

  row++;
  mvwprintw(win, row, col, "Sequential Zones:");
  draw_bar(win, row, col + 15, tt_lines - rzone_count, tt_lines, bar_width,
           COLOR_PAIR_CYAN);

  // Add separator
  row += 2;
  draw_separator(win, row, col, width - 4);
  row++;

  // Free line distribution
  mvwprintw(win, row, col, "Free RZones:");
  draw_bar(win, row, col + 15, free_rzone, tt_lines, bar_width,
           COLOR_PAIR_BLUE);

  row++;
  mvwprintw(win, row, col, "Free SZones:");
  draw_bar(win, row, col + 15, free_szone, tt_lines, bar_width,
           COLOR_PAIR_CYAN);

  // Victim line distribution
  row += 2;
  mvwprintw(win, row, col, "Victim RZones:");
  draw_bar(win, row, col + 15, victim_rzone, tt_lines, bar_width,
           COLOR_PAIR_BLUE);

  row++;
  mvwprintw(win, row, col, "Victim SZones:");
  draw_bar(win, row, col + 15, victim_szone, tt_lines, bar_width,
           COLOR_PAIR_CYAN);

  // Full line distribution
  row += 2;
  mvwprintw(win, row, col, "Full RZones:");
  draw_bar(win, row, col + 15, full_rzone, tt_lines, bar_width,
           COLOR_PAIR_BLUE);

  row++;
  mvwprintw(win, row, col, "Full SZones:");
  draw_bar(win, row, col + 15, full_szone, tt_lines, bar_width,
           COLOR_PAIR_CYAN);

  return row + 4;  // Include margin
}

/**
 * Draw BBSSD block distribution panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Width of the panel
 * @param device Device context
 * @return Next row after the panel
 */
static int draw_bbssd_block_distribution(WINDOW *win, int row, int col,
                                         int width, device_context_t *device) {
  bbssd_data_t *data = (bbssd_data_t *)device->device_data;
  // bbssd_gc_info_t *gc_info = &data->gc_info;
  // bbssd_flash_info_t *flash_info = &data->flash_info;

  // // Calculate total blocks and stats
  // int total_blocks =
  //     flash_info->num_channels * flash_info->num_luns *
  //     flash_info->num_blocks;
  // int free_blocks = gc_info->free_blk_count;
  // int victim_blocks = gc_info->victim_blk_count;
  // int full_blocks = total_blocks - free_blocks - victim_blocks;
  // int bar_width = width - 25;

  // // Draw box with title
  // draw_box_with_title(win, row, col, 10, width, "Block Distribution",
  //                     COLOR_PAIR_NORMAL);

  // // Block status distribution
  // row += 2;
  // col += 2;

  // mvwprintw(win, row, col, "Free Blocks:");
  // draw_bar(win, row, col + 15, free_blocks, total_blocks, bar_width,
  //          COLOR_PAIR_GREEN);

  // row++;
  // mvwprintw(win, row, col, "Victim Blocks:");
  // draw_bar(win, row, col + 15, victim_blocks, total_blocks, bar_width,
  //          COLOR_PAIR_YELLOW);

  // row++;
  // mvwprintw(win, row, col, "Full Blocks:");
  // draw_bar(win, row, col + 15, full_blocks, total_blocks, bar_width,
  //          COLOR_PAIR_RED);

  // // Add separator
  // row += 2;
  // draw_separator(win, row, col, width - 4);
  // row++;

  // // Calculate P/E cycle distribution
  // int pe_buckets[6] = {0};  // 0-19, 20-39, 40-59, 60-79, 80-99, 100+
  // int max_pe = 0;

  // for (int i = 0; i < flash_info->block_info_count; i++) {
  //   int pe = flash_info->blocks[i].pe_cycles;

  //   if (pe < 20)
  //     pe_buckets[0]++;
  //   else if (pe < 40)
  //     pe_buckets[1]++;
  //   else if (pe < 60)
  //     pe_buckets[2]++;
  //   else if (pe < 80)
  //     pe_buckets[3]++;
  //   else if (pe < 100)
  //     pe_buckets[4]++;
  //   else
  //     pe_buckets[5]++;

  //   if (pe > max_pe) max_pe = pe;
  // }

  // // Draw P/E distribution
  // mvwprintw(win, row, col, "P/E Cycles Distribution (Max: %d):", max_pe);
  // row++;

  // const char *pe_labels[] = {"0-19",  "20-39", "40-59",
  //                            "60-79", "80-99", "100+"};
  // for (int i = 0; i < 6; i++) {
  //   mvwprintw(win, row, col, "%-6s:", pe_labels[i]);
  //   draw_bar(win, row, col + 15, pe_buckets[i], flash_info->block_info_count,
  //            bar_width, COLOR_PAIR_BLUE);
  //   row++;
  // }

  return row + 2;  // Include margin
}

/**
 * Draw ZNSSD zone distribution panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Width of the panel
 * @param device Device context
 * @return Next row after the panel
 */
static int draw_znssd_zone_distribution(WINDOW *win, int row, int col,
                                        int width, device_context_t *device) {
  znssd_data_t *data = (znssd_data_t *)device->device_data;
  int bar_width = width - 25;

  // Count zones by state
  int open_zones = 0;
  int full_zones = 0;
  int empty_zones = 0;
  int other_zones = 0;

  for (int i = 0; i < data->num_zones; i++) {
    if (data->znssd_data[i].d.zs == 0x0E) {  // Full
      full_zones++;
    } else if (data->znssd_data[i].d.zs == 1) {  // Empty
      empty_zones++;
    } else if (data->znssd_data[i].d.zs == 2 ||
               data->znssd_data[i].d.zs == 3) {  // Open
      open_zones++;
    } else {
      other_zones++;
    }
  }

  // Draw box with title
  draw_box_with_title(win, row, col, 10, width, "Zone Distribution",
                      COLOR_PAIR_NORMAL);

  // Block status distribution
  row += 2;
  col += 2;

  mvwprintw(win, row, col, "Empty Zones:");
  draw_bar(win, row, col + 15, empty_zones, data->num_zones, bar_width,
           COLOR_PAIR_GREEN);

  row++;
  mvwprintw(win, row, col, "Open Zones:");
  draw_bar(win, row, col + 15, open_zones, data->num_zones, bar_width,
           COLOR_PAIR_YELLOW);

  row++;
  mvwprintw(win, row, col, "Full Zones:");
  draw_bar(win, row, col + 15, full_zones, data->num_zones, bar_width,
           COLOR_PAIR_RED);

  row++;
  mvwprintw(win, row, col, "Other States:");
  draw_bar(win, row, col + 15, other_zones, data->num_zones, bar_width,
           COLOR_PAIR_BLUE);

  // Add separator
  row += 2;
  draw_separator(win, row, col, width - 4);
  row++;

  // Calculate write pointer distribution
  int wp_buckets[5] = {0};  // 0-19%, 20-39%, 40-59%, 60-79%, 80-100%

  for (int i = 0; i < data->num_zones; i++) {
    znssd_zone_info_t *zone = &data->znssd_data[i];

    uint64_t used = zone->d.wp - zone->d.zslba;
    int percent = (zone->d.zcap > 0) ? (used * 100 / zone->d.zcap) : 0;

    if (percent < 20)
      wp_buckets[0]++;
    else if (percent < 40)
      wp_buckets[1]++;
    else if (percent < 60)
      wp_buckets[2]++;
    else if (percent < 80)
      wp_buckets[3]++;
    else
      wp_buckets[4]++;
  }

  // Draw write pointer distribution
  if (open_zones > 0) {
    mvwprintw(win, row, col, "Write Pointer Distribution (Open Zones Only):");
    row++;

    const char *wp_labels[] = {"0-19%", "20-39%", "40-59%", "60-79%",
                               "80-100%"};
    for (int i = 0; i < 5; i++) {
      mvwprintw(win, row, col, "%-7s:", wp_labels[i]);
      draw_bar(win, row, col + 15, wp_buckets[i], open_zones, bar_width,
               COLOR_PAIR_BLUE);
      row++;
    }
  }

  return row + 2;  // Include margin
}

/**
 * Initialize stats view
 *
 * @return 0 on success, non-zero on failure
 */
static int stats_view_init(void) {
  if (stats_view_initialized) {
    return 0;
  }

  // Do any initialization if needed

  stats_view_initialized = 1;
  return 0;
}

/**
 * Activate stats view
 *
 * @return 0 on success, non-zero on failure
 */
static int stats_view_activate(void) {
  // Nothing specific to do on activation
  return 0;
}

/**
 * Deactivate stats view
 *
 * @return 0 on success, non-zero on failure
 */
static int stats_view_deactivate(void) {
  // Nothing specific to do on deactivation
  return 0;
}

/**
 * Render stats view
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param width Window width
 * @param height Window height
 * @return 0 on success, non-zero on failure
 */
static int stats_view_render(WINDOW *win, device_manager_t *manager, int width,
                             int height) {
  device_context_t *device = &manager->devices[manager->current_device];
  int next_row;

  // Calculate layout
  width = width - 2;  // Adjust for padding
  next_row = 3;       // Start below the header

  // For multi-device mode, add an extra row for device selector
  if (manager->device_count > 1) {
    next_row = 4;
  }

  // Render different statistics based on device type and mode
  switch (manager->mode) {
    case MODE_HYSSD: {
      // Draw HYSSD basic statistics
      next_row = draw_hyssd_basic_stats(win, next_row, 1, device);

      // Draw line distribution or detailed stats based on mode
      if (stats_view_show_detail) {
        // Draw more detailed stats here if needed
      } else {
        next_row =
            draw_hyssd_line_distribution(win, next_row, 1, width, device);
      }
      break;
    }

    case MODE_BBSSD: {
      // Draw BBSSD basic statistics
      next_row = draw_bbssd_basic_stats(win, next_row, 1, device);

      // Draw block distribution
      next_row = draw_bbssd_block_distribution(win, next_row, 1, width, device);
      break;
    }

    case MODE_MULTI: {
      // In multi mode, show stats based on the current device type
      if (device->device_type == DEVICE_BBSSD) {
        next_row = draw_bbssd_basic_stats(win, next_row, 1, device);
        next_row =
            draw_bbssd_block_distribution(win, next_row, 1, width, device);
      } else if (device->device_type == DEVICE_ZNSSD) {
        next_row = draw_znssd_basic_stats(win, next_row, 1, device);
        next_row =
            draw_znssd_zone_distribution(win, next_row, 1, width, device);
      } else {
        mvwprintw(win, next_row, 1, "Unknown device type");
        next_row += 2;
      }
      break;
    }

    default:
      mvwprintw(win, next_row, 1, "Unknown mode");
      next_row += 2;
      break;
  }

  // Add help text at the bottom
  mvwprintw(win, height - 1, 1, "Press 'd' to toggle detailed statistics view");

  return 0;
}

/**
 * Handle input for stats view
 *
 * @param ch Input character
 * @param manager Device manager
 * @return 1 if the input was handled, 0 if not
 */
static int stats_view_handle_input(int ch, device_manager_t *manager) {
  // Handle view-specific input
  switch (ch) {
    case 'd':  // Toggle detail view
    case 'D':
      stats_view_show_detail = !stats_view_show_detail;
      return 1;

    case KEY_UP:
    case KEY_DOWN:
    case KEY_PPAGE:
    case KEY_NPAGE:
      // Handle scrolling in future enhancements
      return 1;
  }

  return 0;  // Input not handled
}

/**
 * Clean up resources used by stats view
 */
static void stats_view_cleanup(void) {
  // Clean up any resources
  stats_view_initialized = 0;
}