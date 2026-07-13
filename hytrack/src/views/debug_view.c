/**
 * debug_view.c
 * Debug View Implementation
 */

#include "../../include/views/debug_view.h"

#include <ncurses.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../include/common/ui_common.h"
#include "../../include/core/device_manager.h"

// Forward declarations of private functions
static int debug_view_activate(void);
static int debug_view_deactivate(void);
static int debug_view_render(WINDOW *win, device_manager_t *manager, int width,
                             int height);
static int debug_view_handle_input(int ch, device_manager_t *manager);
static void debug_view_cleanup(void);

// Local variables
static int debug_view_initialized = 0;
static int debug_view_raw_data = 0;  // Toggle between summary and raw data view
static char debug_log_buffer[DEBUG_LOG_SIZE]
                            [256];  // Circular buffer for log entries
static int debug_log_entries = 0;   // Number of log entries (이름 변경:
                                    // debug_log_count -> debug_log_entries)
static int debug_log_index = 0;     // Current index in circular buffer
static int debug_log_top = 0;       // Top displayed log entry

// View interface implementation
view_interface_t debug_view = {.name = "Debug",
                               .description = "Debug and raw data view",
                               .key = "5",
                               .init = debug_view_init,
                               .activate = debug_view_activate,
                               .deactivate = debug_view_deactivate,
                               .render = debug_view_render,
                               .handle_input = debug_view_handle_input,
                               .cleanup = debug_view_cleanup};

/**
 * Add a log entry
 *
 * @param format Format string
 * @param ... Additional arguments
 */
void debug_log_add(const char *format, ...) {
  va_list args;
  char buffer[256];
  time_t now;
  struct tm *local_time;

  // Get current time
  time(&now);
  local_time = localtime(&now);

  // Format time
  strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S] ", local_time);

  // Format message
  va_start(args, format);
  vsnprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), format,
            args);
  va_end(args);

  // Add to log buffer
  strncpy(debug_log_buffer[debug_log_index], buffer,
          sizeof(debug_log_buffer[0]) - 1);
  debug_log_buffer[debug_log_index][sizeof(debug_log_buffer[0]) - 1] = '\0';

  // Update index
  debug_log_index = (debug_log_index + 1) % DEBUG_LOG_SIZE;
  if (debug_log_entries < DEBUG_LOG_SIZE) {
    debug_log_entries++;
  }
}

/**
 * Clear the debug log
 */
void debug_log_clear(void) {
  debug_log_entries = 0;
  debug_log_index = 0;
  debug_log_add("Log cleared");
}

/**
 * Get number of entries in debug log
 *
 * @return Number of log entries
 */
int debug_log_get_count(
    void) {  // 이름 변경: debug_log_count -> debug_log_get_count
  return debug_log_entries;
}

/**
 * Get log entry by index
 *
 * @param index Log entry index
 * @return Log entry string or NULL if invalid
 */
const char *debug_log_get(int index) {
  if (index < 0 || index >= debug_log_entries) {
    return NULL;
  }

  if (debug_log_entries < DEBUG_LOG_SIZE) {
    return debug_log_buffer[index];
  } else {
    return debug_log_buffer[(debug_log_index + index) % DEBUG_LOG_SIZE];
  }
}

/**
 * Draw summary panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Panel width
 * @param manager Device manager
 * @return Next row after the panel
 */
static int draw_summary_panel(WINDOW *win, int row, int col, int width,
                              device_manager_t *manager) {
  device_context_t *device = &manager->devices[manager->current_device];

  // Draw box with title
  draw_box_with_title(win, row, col, 14, width, "Debug Summary",
                      COLOR_PAIR_NORMAL);

  // Draw device information
  row += 2;
  col += 2;

  mvwprintw(win, row, col, "Device Path: %s", device->device_path);
  row++;

  const char *device_types[] = {"BBSSD", "ZNSSD", "HYSSD", "Unknown"};
  int type_index = device->device_type;
  if (type_index < 0 || type_index > 2) {
    type_index = 3;  // Unknown
  }

  mvwprintw(win, row, col, "Device Type: %s", device_types[type_index]);
  row++;

  const char *mode_types[] = {"HYSSD", "BBSSD", "BBSSD+ZNSSD", "Unknown"};
  int mode_index = manager->mode;
  if (mode_index < 0 || mode_index > 2) {
    mode_index = 3;  // Unknown
  }

  mvwprintw(win, row, col, "Operating Mode: %s", mode_types[mode_index]);
  row++;

  // Draw device status
  row++;
  mvwprintw(win, row, col, "Device Status: %s",
            device->status == 0 ? "OK" : "Error");
  row++;

  mvwprintw(win, row, col, "Update Count: %d", device->update_count);
  row++;

  // Draw history information
  row++;
  mvwprintw(win, row, col, "History Size: %d/%d entries",
            device->histories->free_line_history.count, HISTORY_SIZE);
  row++;

  mvwprintw(win, row, col, "Auto-scale: %s",
            manager->view_state.auto_scale ? "ON" : "OFF");
  mvwprintw(win, row, col + 25, "Show Grid: %s",
            manager->view_state.show_grid ? "ON" : "OFF");
  row++;

  // Add footer
  row += 2;
  mvwprintw(win, row, col, "Press 'd' to toggle raw data view");

  return row + 2;  // Include margin
}

/**
 * Draw raw data panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Panel width
 * @param height Panel height
 * @param manager Device manager
 * @return Next row after the panel
 */
static int draw_raw_data_panel(WINDOW *win, int row, int col, int width,
                               int height, device_manager_t *manager) {
  device_context_t *device = &manager->devices[manager->current_device];

  // Draw box with title
  draw_box_with_title(win, row, col, height, width, "Raw Data View",
                      COLOR_PAIR_NORMAL);

  row += 2;
  col += 2;

  // Display raw data based on device type
  mvwprintw(win, row, col, "Raw data view for %s", device->device_path);
  row++;

  // Here we would add device-type specific raw data viewing
  // This is a placeholder that would need to be expanded based on the specific
  // data structures of each device type

  // Draw navigation help
  mvwprintw(win, row + height - 5, col,
            "Press UP/DOWN/PGUP/PGDN to navigate, 'd' to toggle view");

  return row + height;
}

/**
 * Draw debug log
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Panel width
 * @param height Panel height
 * @return Next row after the panel
 */
static int draw_debug_log(WINDOW *win, int row, int col, int width,
                          int height) {
  int i, idx;
  int lines_per_page = height - 6;

  // Draw box with title
  draw_box_with_title(win, row, col, height, width, "Debug Log",
                      COLOR_PAIR_NORMAL);

  row += 2;
  col += 2;

  // If log is empty, show message
  if (debug_log_entries == 0) {
    mvwprintw(win, row, col, "No log entries yet.");
    return row + height - 2;
  }

  // Display log entries
  for (i = 0; i < lines_per_page && i < debug_log_entries; i++) {
    if (debug_log_entries < DEBUG_LOG_SIZE) {
      idx = i;
    } else {
      idx = (debug_log_index - debug_log_entries + i + DEBUG_LOG_SIZE) %
            DEBUG_LOG_SIZE;
    }

    // Wrap text if needed
    if ((int)strlen(debug_log_buffer[idx]) > width - 4) {  // 타입 캐스팅 추가
      char buffer[256];
      strncpy(buffer, debug_log_buffer[idx], width - 7);
      buffer[width - 7] = '\0';
      strcat(buffer, "...");
      mvwprintw(win, row + i, col, "%s", buffer);
    } else {
      mvwprintw(win, row + i, col, "%s", debug_log_buffer[idx]);
    }
  }

  // Draw log size
  mvwprintw(win, row + height - 4, col, "Log entries: %d/%d", debug_log_entries,
            DEBUG_LOG_SIZE);

  return row + height;
}

/**
 * Initialize debug view
 *
 * @return 0 on success, non-zero on failure
 */
int debug_view_init(void) {
  if (debug_view_initialized) {
    return 0;
  }

  // Initialize log buffer
  memset(debug_log_buffer, 0, sizeof(debug_log_buffer));
  debug_log_entries = 0;
  debug_log_index = 0;

  // Add initial log entries
  debug_log_add("Debug view initialized");
  debug_log_add("HYTRACK monitor started");

  debug_view_initialized = 1;
  return 0;
}

/**
 * Activate debug view
 *
 * @return 0 on success, non-zero on failure
 */
static int debug_view_activate(void) {
  debug_log_add("Debug view activated");
  return 0;
}

/**
 * Deactivate debug view
 *
 * @return 0 on success, non-zero on failure
 */
static int debug_view_deactivate(void) {
  debug_log_add("Debug view deactivated");
  return 0;
}

/**
 * Render debug view
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param width Window width
 * @param height Window height
 * @return 0 on success, non-zero on failure
 */
static int debug_view_render(WINDOW *win, device_manager_t *manager, int width,
                             int height) {
  int next_row;

  // Add log entry for each render (but not too frequently)
  // if (manager->view_state.update_count % 10 == 0) {
  //   debug_log_add("Update count: %d", manager->view_state.update_count);
  // }

  // Calculate layout
  width = width - 2;  // Adjust for padding
  next_row = 3;       // Start below the header

  // For multi-device mode, add an extra row for device selector
  if (manager->device_count > 1) {
    next_row = 4;
  }

  // Draw summary panel
  next_row = draw_summary_panel(win, next_row, 1, width, manager);

  // Draw either raw data or log depending on mode
  int remaining_height = height - next_row - 1;
  if (debug_view_raw_data) {
    draw_raw_data_panel(win, next_row, 1, width, remaining_height, manager);
  } else {
    draw_debug_log(win, next_row, 1, width, remaining_height);
  }

  return 0;
}

/**
 * Handle input for debug view
 *
 * @param ch Input character
 * @param manager Device manager
 * @return 1 if the input was handled, 0 if not
 */
static int debug_view_handle_input(int ch, device_manager_t *manager) {
  // Handle view-specific input
  switch (ch) {
    case 'd':  // Toggle raw data view
    case 'D':
      debug_view_raw_data = !debug_view_raw_data;
      return 1;

    case KEY_UP:
      if (debug_view_raw_data && debug_log_top > 0) {
        debug_log_top--;
      }
      return 1;

    case KEY_DOWN:
      if (debug_view_raw_data) {
        debug_log_top++;
        // Upper limit is checked during render
      }
      return 1;

    case KEY_PPAGE:  // Page Up
      if (debug_view_raw_data) {
        debug_log_top -= 10;
        if (debug_log_top < 0) {
          debug_log_top = 0;
        }
      }
      return 1;

    case KEY_NPAGE:  // Page Down
      if (debug_view_raw_data) {
        debug_log_top += 10;
        // Upper limit is checked during render
      }
      return 1;

    case 'c':  // Clear log
    case 'C':
      if (!debug_view_raw_data) {
        debug_log_clear();
      }
      return 1;

    case 'l':  // Add test log entry
    case 'L':
      debug_log_add("Test log entry at update count %d",
                    manager->view_state.update_count);
      return 1;
  }

  return 0;  // Input not handled
}

/**
 * Clean up resources used by debug view
 */
static void debug_view_cleanup(void) {
  // Add final log entry
  debug_log_add("Debug view cleanup");

  // Clean up any resources
  debug_view_initialized = 0;
}