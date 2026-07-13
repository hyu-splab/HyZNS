/**
 * ui_common.c
 * Common UI Components Implementation
 */

#include "../../include/common/ui_common.h"

#include <libgen.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/core/data_types.h"
#include "../../include/core/device_manager.h"

/**
 * Initialize color pairs
 */
void init_colors(void) {
  start_color();
  use_default_colors();

  init_pair(COLOR_PAIR_HEADER, COLOR_BLACK, COLOR_CYAN);
  init_pair(COLOR_PAIR_NORMAL, COLOR_WHITE, -1);
  init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_BLACK, COLOR_WHITE);
  init_pair(COLOR_PAIR_GREEN, COLOR_GREEN, -1);
  init_pair(COLOR_PAIR_YELLOW, COLOR_YELLOW, -1);
  init_pair(COLOR_PAIR_RED, COLOR_RED, -1);
  init_pair(COLOR_PAIR_BLUE, COLOR_BLUE, -1);
  init_pair(COLOR_PAIR_CYAN, COLOR_CYAN, -1);
  init_pair(COLOR_PAIR_MAGENTA, COLOR_MAGENTA, -1);
  init_pair(COLOR_PAIR_STATUS_OK, COLOR_BLACK, COLOR_GREEN);
  init_pair(COLOR_PAIR_STATUS_WARN, COLOR_BLACK, COLOR_YELLOW);
  init_pair(COLOR_PAIR_STATUS_CRITICAL, COLOR_WHITE, COLOR_RED);
}

/**
 * Draw bar graph
 *
 * @param win Window object
 * @param row Row position
 * @param col Column position
 * @param value Current value
 * @param max_value Maximum value
 * @param width Graph width
 * @param color_pair Color pair
 */
void draw_bar(WINDOW *win, int row, int col, int value, int max_value,
              int width, int color_pair) {
  int i, bar_width;

  // If max_value is 0 or less, set it to 1
  if (max_value <= 0) max_value = 1;

  // Calculate bar length
  bar_width = (int)((float)value * width / max_value);
  bar_width = (bar_width > width) ? width : bar_width;

  // Draw bar
  wattron(win, COLOR_PAIR(color_pair));
  mvwprintw(win, row, col, "[");
  for (i = 0; i < width; i++) {
    if (i < bar_width) {
      waddch(win, ACS_BLOCK);
    } else {
      waddch(win, ' ');
    }
  }
  wprintw(win, "] %d/%d", value, max_value);
  wattroff(win, COLOR_PAIR(color_pair));
}

/**
 * Draw sparkline graph
 *
 * @param win Window object
 * @param row Row position
 * @param col Column position
 * @param history History data
 * @param height Graph height
 * @param width Graph width
 * @param title Graph title
 * @param color_pair Color pair
 * @param show_grid Whether to show grid lines
 * @return Next element's starting row
 */
int draw_sparkline(WINDOW *win, int row, int col, History *history, int height,
                   int width, const char *title, int color_pair,
                   int show_grid) {
  int i, j, max_value = 0;
  int x, y, idx;
  float threshold;

  // If there's no data, exit
  if (history->count == 0) {
    return row + 1;
  }

  // Use history's max_value or find maximum value if auto-scaling
  if (history->max_value > 0) {
    max_value = history->max_value;
  } else {
    for (i = 0; i < history->count; i++) {
      if (history->data[i] > max_value) {
        max_value = history->data[i];
      }
    }
  }

  // If max_value is 0 or less, set it to 1
  if (max_value <= 0) max_value = 1;

  // Draw title
  wattron(win, A_BOLD);
  mvwprintw(win, row, col, "%s (Max: %d)", title ? title : history->name,
            max_value);
  wattroff(win, A_BOLD);

  // Draw grid if enabled
  if (show_grid) {
    wattron(win, COLOR_PAIR(COLOR_PAIR_NORMAL) | A_DIM);

    // Draw horizontal grid lines (25%, 50%, 75%)
    for (i = 1; i < 4; i++) {
      int grid_y = row + height - (i * height / 4);
      mvwhline(win, grid_y, col, ACS_HLINE, width);
      mvwprintw(win, grid_y, col + width + 1, "%d%%", i * 25);
    }

    // Draw vertical grid lines (every 10 units)
    for (i = 10; i < width; i += 10) {
      mvwvline(win, row + 1, col + i, ACS_VLINE, height);
    }

    wattroff(win, COLOR_PAIR(COLOR_PAIR_NORMAL) | A_DIM);
  }

  // Draw graph
  wattron(win, COLOR_PAIR(color_pair));
  for (y = 0; y < height; y++) {
    threshold = (float)max_value * (height - y) / height;

    for (x = 0; x < width && x < history->count; x++) {
      // Calculate correct index in circular buffer
      if (history->count < HISTORY_SIZE) {
        idx = x;
      } else {
        idx = (history->index + x) % HISTORY_SIZE;
      }

      if ((float)history->data[idx] >= threshold) {
        mvwaddch(win, row + height - y, col + x, ACS_BLOCK);
      } else if (show_grid && (y % (height / 4) == 0 || x % 10 == 0)) {
        // Don't overwrite grid points
        continue;
      } else {
        mvwaddch(win, row + height - y, col + x, ' ');
      }
    }
  }

  // Draw baseline
  for (x = 0; x < width && x < history->count; x++) {
    mvwaddch(win, row + height + 1, col + x, ACS_HLINE);
  }

  // Draw most recent value
  int recent_value = 0;
  if (history->count > 0) {
    if (history->count < HISTORY_SIZE) {
      recent_value = history->data[history->count - 1];
    } else {
      idx = (history->index - 1 + HISTORY_SIZE) % HISTORY_SIZE;
      recent_value = history->data[idx];
    }
  }

  mvwprintw(win, row, col + width - 12, "Now: %d", recent_value);

  wattroff(win, COLOR_PAIR(color_pair));

  return row + height + 2;  // Include margin
}

/**
 * Draw box with title
 *
 * @param win Window object
 * @param y Top-left corner y coordinate
 * @param x Top-left corner x coordinate
 * @param height Box height
 * @param width Box width
 * @param title Box title
 * @param color_pair Color pair for title
 */
void draw_box_with_title(WINDOW *win, int y, int x, int height, int width,
                         const char *title, int color_pair) {
  // Draw the box
  box(win, 0, 0);
  mvwvline(win, y + 1, x, ACS_VLINE, height - 2);
  mvwvline(win, y + 1, x + width - 1, ACS_VLINE, height - 2);
  mvwhline(win, y, x, ACS_HLINE, width);
  mvwhline(win, y + height - 1, x, ACS_HLINE, width);

  // Draw the corners
  mvwaddch(win, y, x, ACS_ULCORNER);
  mvwaddch(win, y, x + width - 1, ACS_URCORNER);
  mvwaddch(win, y + height - 1, x, ACS_LLCORNER);
  mvwaddch(win, y + height - 1, x + width - 1, ACS_LRCORNER);

  // Draw the title
  if (title && strlen(title) > 0) {
    wattron(win, COLOR_PAIR(color_pair) | A_BOLD);
    mvwprintw(win, y, x + 2, " %s ", title);
    wattroff(win, COLOR_PAIR(color_pair) | A_BOLD);
  }
}

/**
 * Draw table with headers and data
 *
 * @param win Window object
 * @param y Top-left corner y coordinate
 * @param x Top-left corner x coordinate
 * @param headers Array of header strings
 * @param header_count Number of headers
 * @param widths Array of column widths
 * @param rows Array of string arrays for each row
 * @param row_count Number of rows
 * @param row_colors Array of color pairs for each row (can be NULL)
 * @param highlight_row Row index to highlight (-1 for none)
 * @return Next row after the table
 */
int draw_table(WINDOW *win, int y, int x, const char **headers,
               int header_count, int *widths, char ***rows, int row_count,
               int *row_colors, int highlight_row) {
  int i, j, current_x;

  // Draw headers
  wattron(win, A_BOLD);
  current_x = x;
  for (i = 0; i < header_count; i++) {
    mvwprintw(win, y, current_x, "%-*s", widths[i], headers[i]);
    current_x += widths[i];
  }
  wattroff(win, A_BOLD);

  // Draw separator
  mvwhline(win, y + 1, x, ACS_HLINE, current_x - x);

  // Draw data rows
  for (i = 0; i < row_count; i++) {
    current_x = x;

    // Apply row color if specified
    if (row_colors) {
      wattron(win, COLOR_PAIR(row_colors[i]));
    }

    // Apply highlight if this is the selected row
    if (i == highlight_row) {
      wattron(win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    }

    // Draw each cell
    for (j = 0; j < header_count; j++) {
      mvwprintw(win, y + 2 + i, current_x, "%-*s", widths[j], rows[i][j]);
      current_x += widths[j];
    }

    // Reset attributes
    if (i == highlight_row) {
      wattroff(win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    }

    if (row_colors) {
      wattroff(win, COLOR_PAIR(row_colors[i]));
    }
  }

  return y + 2 + row_count;
}

/**
 * Draw status indicator
 *
 * @param win Window object
 * @param y Y coordinate
 * @param x X coordinate
 * @param value Current value
 * @param warn_threshold Warning threshold
 * @param critical_threshold Critical threshold
 * @param inverted If true, lower values are critical
 * @param label Label text
 */
void draw_status_indicator(WINDOW *win, int y, int x, int value,
                           int warn_threshold, int critical_threshold,
                           int inverted, const char *label) {
  int color_pair;

  // Determine status color
  if (inverted) {
    if (value <= critical_threshold) {
      color_pair = COLOR_PAIR_STATUS_CRITICAL;
    } else if (value <= warn_threshold) {
      color_pair = COLOR_PAIR_STATUS_WARN;
    } else {
      color_pair = COLOR_PAIR_STATUS_OK;
    }
  } else {
    if (value >= critical_threshold) {
      color_pair = COLOR_PAIR_STATUS_CRITICAL;
    } else if (value >= warn_threshold) {
      color_pair = COLOR_PAIR_STATUS_WARN;
    } else {
      color_pair = COLOR_PAIR_STATUS_OK;
    }
  }

  // Draw status indicator
  wattron(win, color_pair);
  mvwprintw(win, y, x, " %s: %d ", label, value);
  wattroff(win, color_pair);
}

/**
 * Draw a horizontal separator line
 *
 * @param win Window object
 * @param y Y coordinate
 * @param x X coordinate
 * @param width Line width
 */
void draw_separator(WINDOW *win, int y, int x, int width) {
  mvwhline(win, y, x, ACS_HLINE, width);
}

/**
 * Draw progress bar with percentage
 *
 * @param win Window object
 * @param y Y coordinate
 * @param x X coordinate
 * @param width Bar width
 * @param progress Progress value (0-100)
 * @param color_pair Color pair
 */
void draw_progress_bar(WINDOW *win, int y, int x, int width, int progress,
                       int color_pair) {
  int i, bar_width;

  // Normalize progress
  if (progress < 0) progress = 0;
  if (progress > 100) progress = 100;

  // Calculate bar width
  bar_width = width * progress / 100;

  // Draw progress bar
  wattron(win, COLOR_PAIR(color_pair));
  mvwprintw(win, y, x, "[");
  for (i = 0; i < width; i++) {
    if (i < bar_width) {
      waddch(win, ACS_BLOCK);
    } else {
      waddch(win, ' ');
    }
  }
  wprintw(win, "] %d%%", progress);
  wattroff(win, COLOR_PAIR(color_pair));
}

/**
 * Format number with appropriate units (K, M, G, etc.)
 *
 * @param value Number to format
 * @param buffer Output buffer
 * @param size Buffer size
 */
void format_number(long long value, char *buffer, size_t size) {
  const char *units[] = {"", "K", "M", "G", "T", "P"};
  int unit_index = 0;
  double scaled_value = (double)value;

  // Scale value to appropriate unit
  while (scaled_value >= 1000.0 && unit_index < 5) {
    scaled_value /= 1000.0;
    unit_index++;
  }

  // Format with appropriate precision
  if (unit_index == 0) {
    snprintf(buffer, size, "%lld", value);
  } else if (scaled_value < 10.0) {
    snprintf(buffer, size, "%.2f%s", scaled_value, units[unit_index]);
  } else if (scaled_value < 100.0) {
    snprintf(buffer, size, "%.1f%s", scaled_value, units[unit_index]);
  } else {
    snprintf(buffer, size, "%.0f%s", scaled_value, units[unit_index]);
  }
}

/**
 * Draw device selector for multi-device mode
 *
 * @param win Window object
 * @param manager Device manager
 * @param width Window width
 */
void draw_device_selector(WINDOW *win, device_manager_t *manager, int width) {
  int i;

  // Draw at top of screen (after the header)
  wattron(win, COLOR_PAIR(COLOR_PAIR_NORMAL));
  mvwprintw(win, 2, 0, "Devices:");

  for (i = 0; i < manager->device_count; i++) {
    device_context_t *device = &manager->devices[i];
    char *dev_basename = basename(device->device_path);

    // Determine device type text
    char type_str[10] = "Unknown";
    switch (device->device_type) {
      case DEVICE_BBSSD:
        strcpy(type_str, "BBSSD");
        break;
      case DEVICE_ZNSSD:
        strcpy(type_str, "ZNSSD");
        break;
      case DEVICE_HYSSD:
        strcpy(type_str, "HYSSD");
        break;
      default:
        break;
    }

    // Highlight current device
    if (i == manager->current_device) {
      wattron(win, A_REVERSE);
    }

    // Print device info
    mvwprintw(win, 2, 10 + i * 25, "[%d:%s-%s]", i, dev_basename, type_str);

    if (i == manager->current_device) {
      wattroff(win, A_REVERSE);
    }
  }

  // Print navigation hint
  mvwprintw(win, 2, width - 25, "TAB: Switch Device");
  wattroff(win, COLOR_PAIR(COLOR_PAIR_NORMAL));
}