/**
 * dashboard_view.c
 * Dashboard View Implementation
 */

#include "../../include/views/dashboard_view.h"

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
static int dashboard_init(void);
static int dashboard_activate(void);
static int dashboard_deactivate(void);
static int dashboard_render(WINDOW *win, device_manager_t *manager, int width,
                            int height);
static int dashboard_handle_input(int ch, device_manager_t *manager);
static void dashboard_cleanup(void);

// Local variables
static int dashboard_initialized = 0;

// View interface implementation
view_interface_t dashboard_view = {
    .name = "Dashboard",
    .description = "Combined overview of device status",
    .key = "1",
    .init = dashboard_init,
    .activate = dashboard_activate,
    .deactivate = dashboard_deactivate,
    .render = dashboard_render,
    .handle_input = dashboard_handle_input,
    .cleanup = dashboard_cleanup};

/**
 * Draw trends panel
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Panel width
 * @param height Panel height
 * @param device Device context
 * @param show_grid Whether to show grid
 * @return Next row after the panel
 */
static int draw_trends_panel(WINDOW *win, int row, int col, int width,
                             int height, device_context_t *device,
                             int show_grid) {
  int next_row;
  int graph_height = 5;
  int graph_width;
  history_set_t *histories = device->histories;

  // Draw box with title
  draw_box_with_title(win, row, col, height, width, "History Trends",
                      COLOR_PAIR_NORMAL);

  // If there's no data, show message
  if (histories->free_line_history.count <= 1) {
    mvwprintw(win, row + 2, col + 2,
              "Collecting data... Please wait for graphs to appear.");
    return row + height;
  }

  // Calculate graph width
  graph_width = (width - 6) < HISTORY_SIZE ? width - 6 : HISTORY_SIZE;

  // Free lines trend
  next_row = draw_sparkline(win, row + 2, col + 2,
                            &histories->free_line_history, graph_height,
                            graph_width, NULL, COLOR_PAIR_GREEN, show_grid);

  // Victim lines trend
  next_row = draw_sparkline(win, next_row, col + 2,
                            &histories->victim_line_history, graph_height,
                            graph_width, NULL, COLOR_PAIR_YELLOW, show_grid);

  // Full lines trend
  if (next_row + graph_height + 2 < row + height - 1) {
    next_row = draw_sparkline(win, next_row, col + 2,
                              &histories->full_line_history, graph_height,
                              graph_width, NULL, COLOR_PAIR_RED, show_grid);
  }

  // Footer with navigation hint
  mvwprintw(win, row + height - 2, col + 2,
            "Press 'g' to view detailed graphs");

  return row + height;
}

/**
 * Initialize dashboard view
 *
 * @return 0 on success, non-zero on failure
 */
static int dashboard_init(void) {
  if (dashboard_initialized) {
    return 0;
  }

  // Do any initialization if needed

  dashboard_initialized = 1;
  return 0;
}

/**
 * Activate dashboard view
 *
 * @return 0 on success, non-zero on failure
 */
static int dashboard_activate(void) {
  // Nothing specific to do on activation
  return 0;
}

/**
 * Deactivate dashboard view
 *
 * @return 0 on success, non-zero on failure
 */
static int dashboard_deactivate(void) {
  // Nothing specific to do on deactivation
  return 0;
}

/**
 * Render dashboard view
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param width Window width
 * @param height Window height
 * @return 0 on success, non-zero on failure
 */
static int dashboard_render(WINDOW *win, device_manager_t *manager, int width,
                            int height) {
  device_context_t *device = &manager->devices[manager->current_device];
  int next_row, right_col;

  // Calculate layout
  width = width - 2;  // Adjust for padding
  next_row = 3;       // Start below the header

  // For multi-device mode, add an extra row for device selector
  if (manager->device_count > 1) {
    next_row = 4;
  }

  right_col = width - 32;  // Width of right column

  // Render based on device type and mode
  switch (manager->mode) {
    case MODE_HYSSD: {
      // Draw HYSSD-specific elements
      next_row = hyssd_mode_render(win, device, next_row, 1, width, height);
      break;
    }

    case MODE_BBSSD: {
      // Draw BBSSD-specific elements
      next_row = bbssd_mode_render(win, device, next_row, 1, width, height);
      break;
    }

    case MODE_MULTI: {
      // Draw multi-device overview
      next_row =
          multi_mode_render_overview(win, manager, next_row, 1, width, height);
      break;
    }

    default:
      // Unknown mode, draw generic message
      mvwprintw(win, next_row, 1, "Unknown mode or device type");
      next_row += 2;
      break;
  }

  // Draw trend graphs below the main content
  int trends_height = height - next_row - 1;
  if (trends_height >= 15) {
    draw_trends_panel(win, next_row, 1, width, trends_height, device,
                      manager->view_state.show_grid);
  }

  return 0;
}

/**
 * Handle input for dashboard view
 *
 * @param ch Input character
 * @param manager Device manager
 * @return 1 if the input was handled, 0 if not
 */
static int dashboard_handle_input(int ch, device_manager_t *manager) {
  // Handle view-specific input
  switch (ch) {
    case 'g':  // Switch to graph view
    case 'G':
      return 0;  // Let view_manager handle it

    case 'l':  // Switch to line view (if available in this mode)
    case 'L':
      return 0;  // Let view_manager handle it

    case ' ':  // Toggle grid
      manager->view_state.show_grid = !manager->view_state.show_grid;
      return 1;

    case 'a':  // Toggle auto-scale
    case 'A':
      manager->view_state.auto_scale = !manager->view_state.auto_scale;
      return 1;
  }

  return 0;  // Input not handled
}

/**
 * Clean up resources used by dashboard view
 */
static void dashboard_cleanup(void) {
  // Clean up any resources
  dashboard_initialized = 0;
}