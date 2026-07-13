/**
 * ui_common.h
 * Common UI Components for HYTRACK
 */

#ifndef UI_COMMON_H
#define UI_COMMON_H

#include <ncurses.h>

#include "../core/data_types.h"

// Color pair definitions
typedef enum {
  COLOR_PAIR_HEADER = 1,
  COLOR_PAIR_NORMAL,
  COLOR_PAIR_HIGHLIGHT,
  COLOR_PAIR_GREEN,
  COLOR_PAIR_YELLOW,
  COLOR_PAIR_RED,
  COLOR_PAIR_BLUE,
  COLOR_PAIR_CYAN,
  COLOR_PAIR_MAGENTA,
  COLOR_PAIR_STATUS_OK,
  COLOR_PAIR_STATUS_WARN,
  COLOR_PAIR_STATUS_CRITICAL
} ColorPairs;

// Function prototypes

/**
 * Initialize color pairs
 */
void init_colors(void);

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
              int width, int color_pair);

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
                   int width, const char *title, int color_pair, int show_grid);

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
                         const char *title, int color_pair);

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
               int *row_colors, int highlight_row);

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
                           int inverted, const char *label);

/**
 * Draw a horizontal separator line
 *
 * @param win Window object
 * @param y Y coordinate
 * @param x X coordinate
 * @param width Line width
 */
void draw_separator(WINDOW *win, int y, int x, int width);

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
                       int color_pair);

/**
 * Format number with appropriate units (K, M, G, etc.)
 *
 * @param value Number to format
 * @param buffer Output buffer
 * @param size Buffer size
 */
void format_number(long long value, char *buffer, size_t size);

/**
 * Draw device selector for multi-device mode
 *
 * @param win Window object
 * @param ctx Device manager context
 * @param width Window width
 */
void draw_device_selector(WINDOW *win, device_manager_t *manager, int width);

#endif /* UI_COMMON_H */