/**
 * line_view.c
 * Line View Implementation for HYSSD Mode
 */

#include "../../include/views/line_view.h"

#include <limits.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/common/ui_common.h"
#include "../../include/core/device_manager.h"
#include "../../include/modes/hyssd_mode.h"

// Forward declarations of private functions
static int line_view_init(void);
static int line_view_activate(void);
static int line_view_deactivate(void);
static int line_view_render(WINDOW *win, device_manager_t *manager, int width,
                            int height);
static int line_view_handle_input(int ch, device_manager_t *manager);
static void line_view_cleanup(void);

// Sorting modes
enum {
  SORT_BY_ID = 0,
  SORT_BY_IPC,
  SORT_BY_VPC,
  SORT_BY_TOTAL,
  SORT_BY_RIDX,
  SORT_MODE_COUNT
};

// SZone sorting modes
enum {
  SORT_SZ_BY_ID = 0,
  SORT_SZ_BY_ZS,     // Zone State
  SORT_SZ_BY_ZSLBA,  // Zone Start LBA
  SORT_SZ_BY_WP,     // Write Pointer
  SORT_SZ_BY_DIRTY,  // Dirty flag
  SORT_SZ_MODE_COUNT
};

// Filter modes
enum {
  FILTER_ALL = 0,
  FILTER_RZONE,
  FILTER_SZONE,
  FILTER_ACTIVE,
  FILTER_FREE,
  FILTER_VICTIM,
  FILTER_FULL,
  FILTER_MODE_COUNT
};

// SZone filter modes
enum {
  FILTER_SZ_ALL = 0,
  FILTER_SZ_EMPTY,
  FILTER_SZ_OPEN,
  FILTER_SZ_CLOSED,
  FILTER_SZ_FULL,
  FILTER_SZ_READONLY,
  FILTER_SZ_OFFLINE,
  FILTER_SZ_DIRTY,
  FILTER_SZ_MODE_COUNT
};

// Local variables
static int line_view_initialized = 0;
static int line_view_top_line = 0;  // Top displayed line in the list
static int line_view_selected_line =
    0;  // Currently selected line index (in filtered list)
static int line_view_show_detail = 0;   // Toggle between list and detail view
static int line_view_active_panel = 0;  // 0 = RZone, 1 = SZone
static int line_view_rzone_sort = SORT_BY_ID;       // RZone sort mode
static int line_view_szone_sort = SORT_SZ_BY_ID;    // SZone sort mode
static int line_view_rzone_filter = FILTER_ALL;     // RZone filter mode
static int line_view_szone_filter = FILTER_SZ_ALL;  // SZone filter mode
static int line_view_szone_top_line = 0;            // Top displayed SZone line
static int line_view_szone_selected_line = 0;       // Selected SZone line
static int line_view_visible_rows = 20;             // Cached visible rows for page jump

// View interface implementation
view_interface_t line_view = {
    .name = "Lines",
    .description = "Detailed line information view for HYSSD",
    .key = "4",
    .init = line_view_init,
    .activate = line_view_activate,
    .deactivate = line_view_deactivate,
    .render = line_view_render,
    .handle_input = line_view_handle_input,
    .cleanup = line_view_cleanup};

/**
 * Compare function for sorting lines by ID
 */
static int compare_by_id(const void *a, const void *b) {
  const hyssd_line_info_t *line_a = *(const hyssd_line_info_t **)a;
  const hyssd_line_info_t *line_b = *(const hyssd_line_info_t **)b;
  return line_a->id - line_b->id;
}

/**
 * Compare function for sorting lines by IPC
 */
static int compare_by_ipc(const void *a, const void *b) {
  const hyssd_line_info_t *line_a = *(const hyssd_line_info_t **)a;
  const hyssd_line_info_t *line_b = *(const hyssd_line_info_t **)b;
  return line_b->ipc - line_a->ipc;  // Descending
}

/**
 * Compare function for sorting lines by VPC
 */
static int compare_by_vpc(const void *a, const void *b) {
  const hyssd_line_info_t *line_a = *(const hyssd_line_info_t **)a;
  const hyssd_line_info_t *line_b = *(const hyssd_line_info_t **)b;
  return line_b->vpc - line_a->vpc;  // Descending
}

/**
 * Compare function for sorting lines by total pages (IPC + VPC)
 */
static int compare_by_total(const void *a, const void *b) {
  const hyssd_line_info_t *line_a = *(const hyssd_line_info_t **)a;
  const hyssd_line_info_t *line_b = *(const hyssd_line_info_t **)b;
  int total_a = line_a->ipc + line_a->vpc;
  int total_b = line_b->ipc + line_b->vpc;
  return total_b - total_a;  // Descending
}

/**
 * Compare function for sorting lines by RIDX
 */
static int compare_by_ridx(const void *a, const void *b) {
  const hyssd_line_info_t *line_a = *(const hyssd_line_info_t **)a;
  const hyssd_line_info_t *line_b = *(const hyssd_line_info_t **)b;
  return line_a->ridx - line_b->ridx;
}

/**
 * Compare function for sorting SZones by ID
 */
static int compare_sz_by_id(const void *a, const void *b) {
  const hyssd_zone_info_t *zone_a = *(const hyssd_zone_info_t **)a;
  const hyssd_zone_info_t *zone_b = *(const hyssd_zone_info_t **)b;
  return (zone_a->d.zslba < zone_b->d.zslba)
             ? -1
             : (zone_a->d.zslba > zone_b->d.zslba) ? 1 : 0;
}

/**
 * Compare function for sorting SZones by Zone State
 */
static int compare_sz_by_zs(const void *a, const void *b) {
  const hyssd_zone_info_t *zone_a = *(const hyssd_zone_info_t **)a;
  const hyssd_zone_info_t *zone_b = *(const hyssd_zone_info_t **)b;
  uint8_t state_a = zone_a->d.zs >> 4;
  uint8_t state_b = zone_b->d.zs >> 4;
  return state_a - state_b;
}

/**
 * Compare function for sorting SZones by Zone Start LBA
 */
static int compare_sz_by_zslba(const void *a, const void *b) {
  const hyssd_zone_info_t *zone_a = *(const hyssd_zone_info_t **)a;
  const hyssd_zone_info_t *zone_b = *(const hyssd_zone_info_t **)b;
  return (zone_a->d.zslba < zone_b->d.zslba)
             ? -1
             : (zone_a->d.zslba > zone_b->d.zslba) ? 1 : 0;
}

/**
 * Compare function for sorting SZones by Write Pointer
 */
static int compare_sz_by_wp(const void *a, const void *b) {
  const hyssd_zone_info_t *zone_a = *(const hyssd_zone_info_t **)a;
  const hyssd_zone_info_t *zone_b = *(const hyssd_zone_info_t **)b;
  return (zone_a->w_ptr < zone_b->w_ptr)
             ? -1
             : (zone_a->w_ptr > zone_b->w_ptr) ? 1 : 0;
}

/**
 * Compare function for sorting SZones by Dirty flag
 */
static int compare_sz_by_dirty(const void *a, const void *b) {
  const hyssd_zone_info_t *zone_a = *(const hyssd_zone_info_t **)a;
  const hyssd_zone_info_t *zone_b = *(const hyssd_zone_info_t **)b;
  return zone_b->dirty - zone_a->dirty;  // Dirty first
}

/**
 * Get the appropriate compare function based on sort mode for RZones
 */
static int (*get_rzone_compare_function(int sort_mode))(const void *,
                                                        const void *) {
  switch (sort_mode) {
    case SORT_BY_ID:
      return compare_by_id;
    case SORT_BY_IPC:
      return compare_by_ipc;
    case SORT_BY_VPC:
      return compare_by_vpc;
    case SORT_BY_TOTAL:
      return compare_by_total;
    case SORT_BY_RIDX:
      return compare_by_ridx;
    default:
      return compare_by_id;
  }
}

/**
 * Get the appropriate compare function based on sort mode for SZones
 */
static int (*get_szone_compare_function(int sort_mode))(const void *,
                                                        const void *) {
  switch (sort_mode) {
    case SORT_SZ_BY_ID:
      return compare_sz_by_id;
    case SORT_SZ_BY_ZS:
      return compare_sz_by_zs;
    case SORT_SZ_BY_ZSLBA:
      return compare_sz_by_zslba;
    case SORT_SZ_BY_WP:
      return compare_sz_by_wp;
    case SORT_SZ_BY_DIRTY:
      return compare_sz_by_dirty;
    default:
      return compare_sz_by_id;
  }
}

/**
 * Check if a line passes the current RZone filter
 *
 * @param line Line info to check
 * @param filter_mode Current filter mode
 * @return 1 if line passes filter, 0 if not
 */
static int passes_rzone_filter(const hyssd_line_info_t *line, int filter_mode) {
  if (!line->is_rnd) {
    return 0;  // Only RZones pass the RZone filter
  }

  switch (filter_mode) {
    case FILTER_ALL:
      return 1;

    case FILTER_ACTIVE:
      return line->vpc > 0 || line->ipc > 0;

    case FILTER_FREE:
      return line->vpc == 0 && line->ipc == 0;

    case FILTER_VICTIM:
      return line->vpc > 0 && line->ipc > 0;

    case FILTER_FULL:
      return line->vpc > 0 && line->ipc == 0;

    default:
      return 1;
  }
}

/**
 * Check if a zone passes the current SZone filter
 *
 * @param zone Zone info to check
 * @param filter_mode Current filter mode
 * @return 1 if zone passes filter, 0 if not
 */
static int passes_szone_filter(const hyssd_zone_info_t *zone, int filter_mode) {
  if (zone->rnd) {
    return 0;  // Only SZones pass the SZone filter
  }

  uint8_t state = zone->d.zs >> 4;

  switch (filter_mode) {
    case FILTER_SZ_ALL:
      return 1;

    case FILTER_SZ_EMPTY:
      return state == 1;  // NVME_ZONE_STATE_EMPTY

    case FILTER_SZ_OPEN:
      return state == 2 || state == 3;  // IMPLICITLY_OPEN or EXPLICITLY_OPEN

    case FILTER_SZ_CLOSED:
      return state == 4;  // NVME_ZONE_STATE_CLOSED

    case FILTER_SZ_FULL:
      return state == 14;  // NVME_ZONE_STATE_FULL

    case FILTER_SZ_READONLY:
      return state == 13;  // NVME_ZONE_STATE_READ_ONLY

    case FILTER_SZ_OFFLINE:
      return state == 15;  // NVME_ZONE_STATE_OFFLINE

    case FILTER_SZ_DIRTY:
      return zone->dirty == 1;

    default:
      return 1;
  }
}

/**
 * Draw zone type-specific information
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param zone Zone info to display
 * @param is_rzone Whether this is a random zone (1) or sequential zone (0)
 * @return Next row after the zone info
 */
static int draw_zone_type_specific_info(WINDOW *win, int row, int col,
                                        hyssd_zone_info_t *zone, int is_rzone) {
  if (is_rzone) {
    // Random Zone (BBSSD) specific information
    wattron(win, COLOR_PAIR_BLUE);
    mvwprintw(win, row, col, "Random Zone (BBSSD) Information:");
    wattroff(win, COLOR_PAIR_BLUE);
    row++;

    // Display dirty flag
    mvwprintw(win, row, col, "  Dirty Flag: %s",
              zone->dirty ? "Yes (Written)" : "No (Clean)");
    row++;

    // Calculate utilization
    uint64_t capacity = zone->d.zcap;
    uint64_t used = 0;
    if (zone->w_ptr > zone->d.zslba) {
      used = zone->w_ptr - zone->d.zslba;
    }
    double utilization = (capacity > 0) ? (100.0 * used / capacity) : 0;

    mvwprintw(win, row, col, "  Utilization: %.1f%% (%lu / %lu sectors)",
              utilization, used, capacity);
    row++;

    // Add any BBSSD-specific statistics here
  } else {
    // Sequential Zone (ZNS) specific information
    wattron(win, COLOR_PAIR_CYAN);
    mvwprintw(win, row, col, "Sequential Zone (ZNS) Information:");
    wattroff(win, COLOR_PAIR_CYAN);
    row++;

    // Extract and show zone state
    uint8_t state = zone->d.zs >> 4;
    const char *state_str = "Unknown";
    switch (state) {
      case 1:
        state_str = "Empty";
        break;
      case 2:
        state_str = "Implicitly Open";
        break;
      case 3:
        state_str = "Explicitly Open";
        break;
      case 4:
        state_str = "Closed";
        break;
      case 13:
        state_str = "Read Only";
        break;
      case 14:
        state_str = "Full";
        break;
      case 15:
        state_str = "Offline";
        break;
    }

    mvwprintw(win, row, col, "  Zone State: %s (%d)", state_str, state);
    row++;

    // Calculate utilization
    uint64_t capacity = zone->d.zcap;
    uint64_t used = 0;
    if (zone->w_ptr > zone->d.zslba) {
      used = zone->w_ptr - zone->d.zslba;
    }
    double utilization = (capacity > 0) ? (100.0 * used / capacity) : 0;

    mvwprintw(win, row, col, "  Utilization: %.1f%% (%lu / %lu sectors)",
              utilization, used, capacity);
    row++;

    // Show if zone is dirty
    mvwprintw(win, row, col, "  Dirty Flag: %s",
              zone->dirty ? "Yes (Written)" : "No (Clean)");
    row++;

    // Add any ZNS-specific statistics here
  }

  return row + 1;  // Include margin
}

/**
 * Get zone state string
 */
static const char *get_zone_state_str(uint8_t state) {
  switch (state) {
    case 1:
      return "Empty";
    case 2:
      return "Imp.Open";
    case 3:
      return "Exp.Open";
    case 4:
      return "Closed";
    case 13:
      return "Read Only";
    case 14:
      return "Full";
    case 15:
      return "Offline";
    default:
      return "Unknown";
  }
}

/**
 * Draw line list
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Panel width
 * @param height Panel height
 * @param manager Device manager
 * @return Next row after the panel
 */
static int draw_line_list(WINDOW *win, int row, int col, int width, int height,
                          device_manager_t *manager) {
  device_context_t *device = &manager->devices[manager->current_device];
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;
  hyssd_line_mgmt_info_t *info = data->lm_info;

  int i, j;
  int visible_rows = height - 6;  // Account for header, footer, and margins
  line_view_visible_rows = visible_rows;  // Cache for page jump calculations

  // Filter and sort names
  const char *rzone_filter_names[] = {"All", "Active", "Free", "Victim",
                                      "Full"};
  const char *rzone_sort_names[] = {"ID", "IPC", "VPC", "Total", "RIDX"};

  const char *szone_filter_names[] = {"All",  "Empty",    "Open",    "Closed",
                                      "Full", "ReadOnly", "Offline", "Dirty"};
  const char *szone_sort_names[] = {"ID", "State", "SLBA", "WP", "Dirty"};

  // 각 영역별 너비 계산
  int panel_width = (width - 3) / 2;  // 중앙 구분선 고려
  int left_width = panel_width;
  int right_width = width - left_width - 3;

  // RZone 필터링 및 정렬
  hyssd_line_info_t **rzone_lines =
      malloc(info->tt_lines * sizeof(hyssd_line_info_t *));
  int rzone_count = 0;

  for (i = 0; i < info->tt_lines; i++) {
    if (passes_rzone_filter(&info->lines[i], line_view_rzone_filter)) {
      rzone_lines[rzone_count++] = &info->lines[i];
    }
  }

  // RZone 정렬
  qsort(rzone_lines, rzone_count, sizeof(hyssd_line_info_t *),
        get_rzone_compare_function(line_view_rzone_sort));

  // SZone 필터링 및 정렬
  hyssd_zone_info_t **szone_infos =
      malloc(data->num_zones * sizeof(hyssd_zone_info_t *));
  int szone_count = 0;

  for (i = 0; i < data->num_zones; i++) {
    if (passes_szone_filter(&data->zone_info[i], line_view_szone_filter)) {
      szone_infos[szone_count++] = &data->zone_info[i];
    }
  }

  // SZone 정렬
  qsort(szone_infos, szone_count, sizeof(hyssd_zone_info_t *),
        get_szone_compare_function(line_view_szone_sort));

  // 선택된 라인 범위 조정 (RZone)
  if (line_view_selected_line >= rzone_count) {
    line_view_selected_line = rzone_count > 0 ? rzone_count - 1 : 0;
  }

  if (line_view_top_line > line_view_selected_line) {
    line_view_top_line = line_view_selected_line;
  } else if (line_view_top_line + visible_rows <= line_view_selected_line) {
    line_view_top_line = line_view_selected_line - visible_rows + 1;
  }

  // 선택된 라인 범위 조정 (SZone)
  if (line_view_szone_selected_line >= szone_count) {
    line_view_szone_selected_line = szone_count > 0 ? szone_count - 1 : 0;
  }

  if (line_view_szone_top_line > line_view_szone_selected_line) {
    line_view_szone_top_line = line_view_szone_selected_line;
  } else if (line_view_szone_top_line + visible_rows <=
             line_view_szone_selected_line) {
    line_view_szone_top_line = line_view_szone_selected_line - visible_rows + 1;
  }

  // Draw box with title
  draw_box_with_title(win, row, col, height, width, "HYSSD Lines",
                      COLOR_PAIR_NORMAL);

  // 왼쪽 패널 - RZone 정보
  // Draw headers for RZone
  // wattron(win, COLOR_PAIR_BLUE);
  mvwprintw(win, row + 1, col + 2, "Random Zones (Filter: %s, Sort: %s)",
            rzone_filter_names[line_view_rzone_filter],
            rzone_sort_names[line_view_rzone_sort]);
  // wattroff(win, COLOR_PAIR_BLUE);

  // Highlight active panel
  if (line_view_active_panel == 0) {
    wattron(win, A_BOLD);
  }

  mvwprintw(win, row + 2, col + 2, "%-6s %-6s %-6s %-6s %-6s %-10s %-6s", "ID",
            "IPC", "VPC", "Total", "RIDX", "Status", "POS");

  if (line_view_active_panel == 0) {
    wattroff(win, A_BOLD);
  }

  draw_separator(win, row + 3, col + 2, left_width - 2);

  // Draw RZone information
  int rzone_visible = (rzone_count < visible_rows) ? rzone_count : visible_rows;
  for (i = 0; i < rzone_visible && i + line_view_top_line < rzone_count; i++) {
    hyssd_line_info_t *line = rzone_lines[i + line_view_top_line];
    int total = line->ipc + line->vpc;
    char status[16] = "Unknown";

    // Determine status
    if (line->vpc == 0 && line->ipc == 0) {
      strcpy(status, "Free");
    } else if (line->vpc > 0 && line->ipc == 0) {
      strcpy(status, "Full");
    } else if (line->vpc >= 0 && line->ipc > 0) {
      strcpy(status, "Victim");
    }

    // Highlight if this is the selected line (only if active panel is RZone)
    if (i + line_view_top_line == line_view_selected_line &&
        line_view_active_panel == 0) {
      wattron(win, A_REVERSE);
    }

    // Draw line data
    mvwprintw(win, row + 4 + i, col + 2, "%-6d %-6d %-6d %-6d %-6d %-10s %-6lu",
              line->id, line->ipc, line->vpc, total, line->ridx, status,
              line->pos);

    if (i + line_view_top_line == line_view_selected_line &&
        line_view_active_panel == 0) {
      wattroff(win, A_REVERSE);
    }
  }

  // 오른쪽 패널 - SZone 정보
  int right_col = col + left_width + 3;  // 중앙 구분선 다음 위치

  // Draw vertical separator
  for (i = row + 1; i < row + height - 1; i++) {
    mvwaddch(win, i, col + left_width + 1, ACS_VLINE);
  }

  // Draw headers for SZone
  // wattron(win, COLOR_PAIR_CYAN);
  mvwprintw(win, row + 1, right_col, "Sequential Zones (Filter: %s, Sort: %s)",
            szone_filter_names[line_view_szone_filter],
            szone_sort_names[line_view_szone_sort]);
  // wattroff(win, COLOR_PAIR_CYAN);

  // Highlight active panel
  if (line_view_active_panel == 1) {
    wattron(win, A_BOLD);
  }

  mvwprintw(win, row + 2, right_col, "%-6s %-10s %-14s %-14s %-6s", "ID",
            "State", "ZSLBA", "WP", "Dirty");

  if (line_view_active_panel == 1) {
    wattroff(win, A_BOLD);
  }

  draw_separator(win, row + 3, right_col, right_width - 2);

  // SZone 정보를 오른쪽에 표시
  int szone_visible = (szone_count < visible_rows) ? szone_count : visible_rows;
  for (i = 0; i < szone_visible && i + line_view_szone_top_line < szone_count;
       i++) {
    hyssd_zone_info_t *zone = szone_infos[i + line_view_szone_top_line];

    // Calculate zone ID from ZSLBA and capacity
    int zone_id = -1;
    if (zone->d.zcap > 0) {
      zone_id = zone->d.zslba / zone->d.zcap;
    }

    // Get zone state
    uint8_t state = zone->d.zs >> 4;
    const char *state_str = get_zone_state_str(state);

    // Highlight if this is the selected line (only if active panel is SZone)
    if (i + line_view_szone_top_line == line_view_szone_selected_line &&
        line_view_active_panel == 1) {
      wattron(win, A_REVERSE);
    }

    // Draw zone data
    mvwprintw(win, row + 4 + i, right_col, "%-6d %-10s 0x%-12lx 0x%-12lx %-6s",
              zone_id, state_str, zone->d.zslba, zone->w_ptr,
              zone->dirty ? "Yes" : "No");

    if (i + line_view_szone_top_line == line_view_szone_selected_line &&
        line_view_active_panel == 1) {
      wattroff(win, A_REVERSE);
    }
  }

  // Draw scroll indicators if needed for RZone panel
  if (line_view_top_line > 0) {
    mvwaddch(win, row + 1, col + left_width - 1, ACS_UARROW);
  }
  if (line_view_top_line + visible_rows < rzone_count) {
    mvwaddch(win, row + height - 2, col + left_width - 1, ACS_DARROW);
  }

  // Draw scroll indicators if needed for SZone panel
  if (line_view_szone_top_line > 0) {
    mvwaddch(win, row + 1, col + width - 3, ACS_UARROW);
  }
  if (line_view_szone_top_line + visible_rows < szone_count) {
    mvwaddch(win, row + height - 2, col + width - 3, ACS_DARROW);
  }

  // Draw scroll position and count for active panel
  if (line_view_active_panel == 0) {
    mvwprintw(win, row + height - 2, col + 2, "RZone %d of %d",
              rzone_count > 0 ? line_view_selected_line + 1 : 0, rzone_count);
  } else {
    mvwprintw(win, row + height - 2, right_col, "SZone %d of %d",
              szone_count > 0 ? line_view_szone_selected_line + 1 : 0,
              szone_count);
  }

  // Draw help text
  mvwprintw(
      win, row + height - 2, col + 20,
      "Tab: Switch | UP/DOWN: Navigate | s/f: Sort/Filter | Enter: Details");

  // Free allocated memory
  free(rzone_lines);
  free(szone_infos);

  return row + height;
}

/**
 * Draw detailed line information
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Panel width
 * @param height Panel height
 * @param manager Device manager
 * @return Next row after the panel
 */
static int draw_line_detail(WINDOW *win, int row, int col, int width,
                            int height, device_manager_t *manager) {
  device_context_t *device = &manager->devices[manager->current_device];
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;
  hyssd_line_mgmt_info_t *info = data->lm_info;

  int i;
  void *selected_item = NULL;
  int is_rzone = (line_view_active_panel == 0);
  char title[128];

  if (is_rzone) {
    // Find selected RZone line
    hyssd_line_info_t **rzone_lines =
        malloc(info->tt_lines * sizeof(hyssd_line_info_t *));
    int rzone_count = 0;

    for (i = 0; i < info->tt_lines; i++) {
      if (passes_rzone_filter(&info->lines[i], line_view_rzone_filter)) {
        rzone_lines[rzone_count++] = &info->lines[i];
      }
    }

    // Sort RZones
    qsort(rzone_lines, rzone_count, sizeof(hyssd_line_info_t *),
          get_rzone_compare_function(line_view_rzone_sort));

    if (line_view_selected_line < rzone_count) {
      selected_item = rzone_lines[line_view_selected_line];
    }

    free(rzone_lines);
  } else {
    // Find selected SZone
    hyssd_zone_info_t **szone_infos =
        malloc(data->num_zones * sizeof(hyssd_zone_info_t *));
    int szone_count = 0;

    for (i = 0; i < data->num_zones; i++) {
      if (passes_szone_filter(&data->zone_info[i], line_view_szone_filter)) {
        szone_infos[szone_count++] = &data->zone_info[i];
      }
    }

    // Sort SZones
    qsort(szone_infos, szone_count, sizeof(hyssd_zone_info_t *),
          get_szone_compare_function(line_view_szone_sort));

    if (line_view_szone_selected_line < szone_count) {
      selected_item = szone_infos[line_view_szone_selected_line];
    }

    free(szone_infos);
  }

  // If no item is selected, show message
  if (!selected_item) {
    snprintf(title, sizeof(title), "%s Detail", is_rzone ? "RZone" : "SZone");
    draw_box_with_title(win, row, col, height, width, title, COLOR_PAIR_NORMAL);
    mvwprintw(win, row + 2, col + 2,
              "No %s selected or no items match the current filter.",
              is_rzone ? "line" : "zone");
    return row + height;
  }

  if (is_rzone) {
    // Draw RZone details
    hyssd_line_info_t *line = (hyssd_line_info_t *)selected_item;

    snprintf(title, sizeof(title), "Line %d Details (RZone)", line->id);
    draw_box_with_title(win, row, col, height, width, title, COLOR_PAIR_BLUE);

    // Basic line properties
    mvwprintw(win, row + 2, col + 2, "Line ID:               %d", line->id);
    mvwprintw(win, row + 3, col + 2, "Type:                  Random Zone");
    mvwprintw(win, row + 4, col + 2, "Random Zone Index:      %d", line->ridx);

    // Page counts
    mvwprintw(win, row + 6, col + 2, "Valid Page Count:       %d", line->vpc);
    mvwprintw(win, row + 7, col + 2, "Invalid Page Count:     %d", line->ipc);
    mvwprintw(win, row + 8, col + 2, "Total Page Count:       %d",
              line->vpc + line->ipc);

    // Status determination
    char *status = "Unknown";
    if (line->vpc == 0 && line->ipc == 0) {
      status = "Free";
    } else if (line->vpc > 0 && line->ipc == 0) {
      status = "Full";
    } else if (line->vpc > 0 && line->ipc > 0) {
      status = "Victim";
    }

    mvwprintw(win, row + 10, col + 2, "Status:                %s", status);

    if (line->vpc > 0 || line->ipc > 0) {
      mvwprintw(win, row + 11, col + 2, "Position in Victim Queue: %llu",
                line->pos);
    }

    // Draw visual representation of valid/invalid pages
    int bar_width = width - 8;
    int total_pages = line->vpc + line->ipc;

    if (total_pages > 0) {
      int vpc_width = bar_width * line->vpc / total_pages;

      mvwprintw(win, row + 13, col + 2, "Page Distribution:");

      // Draw valid pages part
      wattron(win, COLOR_PAIR(COLOR_PAIR_GREEN));
      for (i = 0; i < vpc_width; i++) {
        mvwaddch(win, row + 14, col + 4 + i, ACS_BLOCK);
      }
      wattroff(win, COLOR_PAIR(COLOR_PAIR_GREEN));

      // Draw invalid pages part
      wattron(win, COLOR_PAIR(COLOR_PAIR_RED));
      for (i = vpc_width; i < bar_width; i++) {
        mvwaddch(win, row + 14, col + 4 + i, ACS_BLOCK);
      }
      wattroff(win, COLOR_PAIR(COLOR_PAIR_RED));

      // Draw legend
      wattron(win, COLOR_PAIR(COLOR_PAIR_GREEN));
      mvwprintw(win, row + 15, col + 4, "■ Valid Pages: %d (%.1f%%)", line->vpc,
                100.0 * line->vpc / total_pages);
      wattroff(win, COLOR_PAIR(COLOR_PAIR_GREEN));

      wattron(win, COLOR_PAIR(COLOR_PAIR_RED));
      mvwprintw(win, row + 16, col + 4, "■ Invalid Pages: %d (%.1f%%)",
                line->ipc, 100.0 * line->ipc / total_pages);
      wattroff(win, COLOR_PAIR(COLOR_PAIR_RED));
    }

    // Find corresponding zone for this line
    for (i = 0; i < data->num_zones; i++) {
      if (data->zone_info[i].rnd &&
          data->zone_info[i].d.zslba / data->zone_info[i].d.zcap ==
              line->ridx) {
        // Display zone details
        int detail_row = row + 18;
        draw_separator(win, detail_row, col + 2, width - 8);
        detail_row++;

        detail_row = draw_zone_type_specific_info(win, detail_row, col + 2,
                                                  &data->zone_info[i], 1);
        break;
      }
    }
  } else {
    // Draw SZone details
    hyssd_zone_info_t *zone = (hyssd_zone_info_t *)selected_item;

    // Calculate zone ID from ZSLBA and capacity
    int zone_id = -1;
    if (zone->d.zcap > 0) {
      zone_id = zone->d.zslba / zone->d.zcap;
    }

    snprintf(title, sizeof(title), "Zone %d Details (SZone)", zone_id);
    draw_box_with_title(win, row, col, height, width, title, COLOR_PAIR_CYAN);

    // Zone attributes
    uint8_t state = zone->d.zs >> 4;
    const char *state_str = get_zone_state_str(state);

    mvwprintw(win, row + 2, col + 2, "Zone ID:               %d", zone_id);
    mvwprintw(win, row + 3, col + 2, "Type:                  Sequential Zone");
    mvwprintw(win, row + 4, col + 2, "Zone Type (ZT):        %d", zone->d.zt);
    mvwprintw(win, row + 5, col + 2, "Zone State (ZS):       %s (%d)",
              state_str, state);
    mvwprintw(win, row + 6, col + 2, "Zone Attribute (ZA):   0x%x", zone->d.za);

    // LBA information
    mvwprintw(win, row + 8, col + 2, "Start LBA:             0x%lx",
              zone->d.zslba);
    mvwprintw(win, row + 9, col + 2, "Zone Capacity:         %lu sectors",
              zone->d.zcap);
    mvwprintw(win, row + 10, col + 2, "Write Pointer:         0x%lx",
              zone->w_ptr);

    // Zone utilization
    uint64_t capacity = zone->d.zcap;
    uint64_t used = 0;
    if (zone->w_ptr > zone->d.zslba) {
      used = zone->w_ptr - zone->d.zslba;
    }
    double utilization = (capacity > 0) ? (100.0 * used / capacity) : 0;

    mvwprintw(win, row + 12, col + 2, "Used Capacity:         %lu sectors",
              used);
    mvwprintw(win, row + 13, col + 2, "Utilization:           %.1f%%",
              utilization);
    mvwprintw(win, row + 14, col + 2, "Dirty Flag:            %s",
              zone->dirty ? "Yes (Written)" : "No (Clean)");

    // Draw utilization bar
    int bar_width = width - 8;
    int used_width = 0;

    if (capacity > 0) {
      used_width = bar_width * used / capacity;
    }

    mvwprintw(win, row + 16, col + 2, "Zone Utilization:");

    // Draw used part
    wattron(win, COLOR_PAIR(COLOR_PAIR_CYAN));
    for (i = 0; i < used_width; i++) {
      mvwaddch(win, row + 17, col + 4 + i, ACS_BLOCK);
    }
    wattroff(win, COLOR_PAIR(COLOR_PAIR_CYAN));

    // Draw free part
    wattron(win, COLOR_PAIR(COLOR_PAIR_NORMAL));
    for (i = used_width; i < bar_width; i++) {
      mvwaddch(win, row + 17, col + 4 + i, ACS_BLOCK);
    }
    wattroff(win, COLOR_PAIR(COLOR_PAIR_NORMAL));
  }

  // Draw help text
  mvwprintw(win, row + height - 2, col + 2,
            "Press Enter to return to line list");

  return row + height;
}

/**
 * Initialize line view
 *
 * @return 0 on success, non-zero on failure
 */
static int line_view_init(void) {
  if (line_view_initialized) {
    return 0;
  }

  // Initialize with default sort and filter modes
  line_view_active_panel = 0;  // Start with RZone panel active
  line_view_rzone_sort = SORT_BY_ID;
  line_view_szone_sort = SORT_SZ_BY_ID;
  line_view_rzone_filter = FILTER_ALL;
  line_view_szone_filter = FILTER_SZ_ALL;

  line_view_initialized = 1;
  return 0;
}

/**
 * Activate line view
 *
 * @return 0 on success, non-zero on failure
 */
static int line_view_activate(void) {
  // Reset view state when activating
  line_view_top_line = 0;
  line_view_szone_top_line = 0;
  line_view_selected_line = 0;
  line_view_szone_selected_line = 0;
  line_view_show_detail = 0;
  return 0;
}

/**
 * Deactivate line view
 *
 * @return 0 on success, non-zero on failure
 */
static int line_view_deactivate(void) {
  // Nothing specific to do on deactivation
  return 0;
}

/**
 * Render line view
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param width Window width
 * @param height Window height
 * @return 0 on success, non-zero on failure
 */
static int line_view_render(WINDOW *win, device_manager_t *manager, int width,
                            int height) {
  device_context_t *device = &manager->devices[manager->current_device];
  int next_row;

  // Check if this is the correct mode
  if (manager->mode != MODE_HYSSD) {
    mvwprintw(win, 3, 1, "Line view is only available in HYSSD mode");
    return 0;
  }

  // Calculate layout
  width = width - 2;  // Adjust for padding
  next_row = 3;       // Start below the header

  // For multi-device mode, add an extra row for device selector
  if (manager->device_count > 1) {
    next_row = 4;
  }

  // Draw either list view or detail view
  if (line_view_show_detail) {
    draw_line_detail(win, next_row, 1, width, height - next_row, manager);
  } else {
    draw_line_list(win, next_row, 1, width, height - next_row, manager);
  }

  return 0;
}

/**
 * Handle input for line view
 *
 * @param ch Input character
 * @param manager Device manager
 * @return 1 if the input was handled, 0 if not
 */
static int line_view_handle_input(int ch, device_manager_t *manager) {
  // Handle view-specific input
  if (line_view_show_detail) {
    // In detail view, only handle Enter to go back
    if (ch == '\n' || ch == KEY_ENTER) {
      line_view_show_detail = 0;
      return 1;
    }
    return 0;
  }

  // Handle input for list view
  switch (ch) {
    case '\t':  // Tab key - switch between panels
      line_view_active_panel = 1 - line_view_active_panel;
      return 1;

    case KEY_UP:
      // Move selection up in active panel
      if (line_view_active_panel == 0) {
        if (line_view_selected_line > 0) {
          line_view_selected_line--;
        }
      } else {
        if (line_view_szone_selected_line > 0) {
          line_view_szone_selected_line--;
        }
      }
      return 1;

    case KEY_DOWN:
      // Move selection down in active panel
      if (line_view_active_panel == 0) {
        line_view_selected_line++;
        // Upper limit checked in render
      } else {
        line_view_szone_selected_line++;
        // Upper limit checked in render
      }
      return 1;

    case KEY_PPAGE:  // Page Up - half page jump
    {
      int jump = line_view_visible_rows / 2;
      if (jump < 1) jump = 1;
      if (line_view_active_panel == 0) {
        line_view_selected_line -= jump;
        if (line_view_selected_line < 0) {
          line_view_selected_line = 0;
        }
      } else {
        line_view_szone_selected_line -= jump;
        if (line_view_szone_selected_line < 0) {
          line_view_szone_selected_line = 0;
        }
      }
      return 1;
    }

    case KEY_NPAGE:  // Page Down - half page jump
    {
      int jump = line_view_visible_rows / 2;
      if (jump < 1) jump = 1;
      if (line_view_active_panel == 0) {
        line_view_selected_line += jump;
        // Upper limit checked in render
      } else {
        line_view_szone_selected_line += jump;
        // Upper limit checked in render
      }
      return 1;
    }

    case KEY_HOME:
      // Go to first item in active panel
      if (line_view_active_panel == 0) {
        line_view_selected_line = 0;
      } else {
        line_view_szone_selected_line = 0;
      }
      return 1;

    case KEY_END:
      // Go to last item in active panel
      if (line_view_active_panel == 0) {
        line_view_selected_line = INT_MAX;  // Will be adjusted in render
      } else {
        line_view_szone_selected_line = INT_MAX;  // Will be adjusted in render
      }
      return 1;

    case '\n':  // Enter key
    case KEY_ENTER:
      // Show details for selected item
      line_view_show_detail = 1;
      return 1;

    case 's':  // Cycle sort mode for active panel
    case 'S':
      if (line_view_active_panel == 0) {
        line_view_rzone_sort = (line_view_rzone_sort + 1) % SORT_MODE_COUNT;
      } else {
        line_view_szone_sort = (line_view_szone_sort + 1) % SORT_SZ_MODE_COUNT;
      }
      return 1;

    case 'f':  // Cycle filter mode for active panel
    case 'F':
      if (line_view_active_panel == 0) {
        line_view_rzone_filter =
            (line_view_rzone_filter + 1) % FILTER_MODE_COUNT;
        line_view_selected_line = 0;  // Reset selection when filter changes
      } else {
        line_view_szone_filter =
            (line_view_szone_filter + 1) % FILTER_SZ_MODE_COUNT;
        line_view_szone_selected_line =
            0;  // Reset selection when filter changes
      }
      return 1;
  }

  return 0;  // Input not handled
}

/**
 * Clean up resources used by line view
 */
static void line_view_cleanup(void) {
  // Clean up any resources
  line_view_initialized = 0;
}