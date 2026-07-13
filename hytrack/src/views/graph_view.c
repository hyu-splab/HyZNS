/**
 * graph_view.c
 * Graph View Implementation
 */

#include "../../include/views/graph_view.h"

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/common/ui_common.h"
#include "../../include/core/device_manager.h"

// Forward declarations of private functions
static int graph_view_init(void);
static int graph_view_activate(void);
static int graph_view_deactivate(void);
static int graph_view_render(WINDOW *win, device_manager_t *manager, int width,
                             int height);
static int graph_view_handle_input(int ch, device_manager_t *manager);
static void graph_view_cleanup(void);

// Local variables
static int graph_view_initialized = 0;
static int graph_view_selected_graph = 0;  // Currently selected graph
static int graph_view_layout = 0;  // 0: all graphs, 1: single large graph

// View interface implementation
view_interface_t graph_view = {.name = "Graphs",
                               .description = "Graphical view of device trends",
                               .key = "3",
                               .init = graph_view_init,
                               .activate = graph_view_activate,
                               .deactivate = graph_view_deactivate,
                               .render = graph_view_render,
                               .handle_input = graph_view_handle_input,
                               .cleanup = graph_view_cleanup};

/**
 * Get the selected history
 *
 * @param device Device context
 * @return Pointer to the selected history
 */
static History *get_selected_history(device_context_t *device) {
  history_set_t *histories = device->histories;

  switch (graph_view_selected_graph) {
    case 0:
      return &histories->free_line_history;
    case 1:
      return &histories->victim_line_history;
    case 2:
      return &histories->full_line_history;
    case 3:
      return &histories->rzone_history;
    case 4:
      return &histories->active_rzone_history;
    case 5:
      return &histories->active_szone_history;
    default:
      return &histories->free_line_history;
  }
}

/**
 * Draw all graphs in tiled layout
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Available width
 * @param height Available height
 * @param device Device context
 * @param show_grid Whether to show grid
 * @return Next row after the panel
 */
static int draw_all_graphs(WINDOW *win, int row, int col, int width, int height,
                           device_context_t *device, int show_grid) {
  int graph_height = (height - 4) / 3;
  int graph_width = (width - 6) / 2;
  int i, j;
  int next_row = row;
  history_set_t *histories = device->histories;

  History *history_array[] = {
      &histories->free_line_history,    &histories->victim_line_history,
      &histories->full_line_history,    &histories->rzone_history,
      &histories->active_rzone_history, &histories->active_szone_history};

  int colors[] = {COLOR_PAIR_GREEN, COLOR_PAIR_YELLOW, COLOR_PAIR_RED,
                  COLOR_PAIR_BLUE,  COLOR_PAIR_CYAN,   COLOR_PAIR_MAGENTA};

  // Draw box with title
  draw_box_with_title(win, row, col, height, width, "History Trends",
                      COLOR_PAIR_NORMAL);

  // If there's not enough data, show message
  if (histories->free_line_history.count <= 1) {
    mvwprintw(win, row + 2, col + 2,
              "Collecting data... Please wait for graphs to appear.");
    return row + height;
  }

  // Draw graphs in 2x3 grid
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 2; j++) {
      int idx = i * 2 + j;
      if (idx < 6) {
        int is_selected = (idx == graph_view_selected_graph);
        int graph_row = row + 2 + i * graph_height;
        int graph_col = col + 2 + j * (graph_width + 2);

        // Draw highlight box for selected graph
        if (is_selected) {
          wattron(win, A_BOLD | COLOR_PAIR(colors[idx]));
          mvwprintw(win, graph_row - 1, graph_col, "%s",
                    history_array[idx]->name);
          wattroff(win, A_BOLD | COLOR_PAIR(colors[idx]));

          // Draw box around selected graph
          wattron(win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
          mvwhline(win, graph_row - 2, graph_col - 1, ACS_HLINE,
                   graph_width + 2);
          mvwhline(win, graph_row + graph_height, graph_col - 1, ACS_HLINE,
                   graph_width + 2);
          mvwvline(win, graph_row - 1, graph_col - 1, ACS_VLINE, graph_height);
          mvwvline(win, graph_row - 1, graph_col + graph_width, ACS_VLINE,
                   graph_height);
          mvwaddch(win, graph_row - 2, graph_col - 1, ACS_ULCORNER);
          mvwaddch(win, graph_row - 2, graph_col + graph_width, ACS_URCORNER);
          mvwaddch(win, graph_row + graph_height, graph_col - 1, ACS_LLCORNER);
          mvwaddch(win, graph_row + graph_height, graph_col + graph_width,
                   ACS_LRCORNER);
          wattroff(win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
        } else {
          mvwprintw(win, graph_row - 1, graph_col, "%s",
                    history_array[idx]->name);
        }

        // Draw the graph
        draw_sparkline(win, graph_row, graph_col, history_array[idx],
                       graph_height - 2, graph_width, NULL, colors[idx],
                       show_grid);
      }
    }
  }

  // Add help text
  mvwprintw(win, row + height - 2, col + 2,
            "Arrow keys to select graph | Enter to expand | 'g' to toggle grid "
            "| 'a' to toggle auto-scale");

  return row + height;
}

/**
 * Draw single large graph
 *
 * @param win Window to draw on
 * @param row Starting row
 * @param col Starting column
 * @param width Available width
 * @param height Available height
 * @param device Device context
 * @param show_grid Whether to show grid
 * @return Next row after the panel
 */
static int draw_large_graph(WINDOW *win, int row, int col, int width,
                            int height, device_context_t *device,
                            int show_grid) {
  History *history = get_selected_history(device);
  int color = COLOR_PAIR_NORMAL;

  // Determine color based on selected graph
  switch (graph_view_selected_graph) {
    case 0:
      color = COLOR_PAIR_GREEN;
      break;
    case 1:
      color = COLOR_PAIR_YELLOW;
      break;
    case 2:
      color = COLOR_PAIR_RED;
      break;
    case 3:
      color = COLOR_PAIR_BLUE;
      break;
    case 4:
      color = COLOR_PAIR_CYAN;
      break;
    case 5:
      color = COLOR_PAIR_MAGENTA;
      break;
  }

  // Draw box with title
  char title[128];
  snprintf(title, sizeof(title), "%s History", history->name);
  draw_box_with_title(win, row, col, height, width, title, color);

  // If there's not enough data, show message
  if (history->count <= 1) {
    mvwprintw(win, row + 2, col + 2,
              "Collecting data... Please wait for graph to appear.");
    return row + height;
  }

  // Draw the large graph
  draw_sparkline(win, row + 2, col + 2, history, height - 6, width - 4, NULL,
                 color, show_grid);

  // Add statistics about the data
  int min = -1, max = 0, sum = 0, latest = 0, avg = 0;
  int i, idx;

  // Calculate statistics
  for (i = 0; i < history->count; i++) {
    idx = (i < HISTORY_SIZE) ? i : (history->index + i) % HISTORY_SIZE;
    int value = history->data[idx];

    sum += value;
    if (min == -1 || value < min) min = value;
    if (value > max) max = value;
  }

  if (history->count > 0) {
    avg = sum / history->count;
    idx = (history->count < HISTORY_SIZE)
              ? (history->count - 1)
              : ((history->index - 1 + HISTORY_SIZE) % HISTORY_SIZE);
    latest = history->data[idx];
  }

  // Draw statistics
  mvwprintw(win, row + height - 3, col + 2,
            "Min: %d  Max: %d  Avg: %d  Latest: %d", min, max, avg, latest);

  // Add help text
  mvwprintw(win, row + height - 2, col + 2,
            "Enter to return to all graphs | 'g' to toggle grid | 'a' to "
            "toggle auto-scale");

  return row + height;
}

/**
 * Initialize graph view
 *
 * @return 0 on success, non-zero on failure
 */
static int graph_view_init(void) {
  if (graph_view_initialized) {
    return 0;
  }

  // Do any initialization if needed

  graph_view_initialized = 1;
  return 0;
}

/**
 * Activate graph view
 *
 * @return 0 on success, non-zero on failure
 */
static int graph_view_activate(void) {
  // Nothing specific to do on activation
  return 0;
}

/**
 * Deactivate graph view
 *
 * @return 0 on success, non-zero on failure
 */
static int graph_view_deactivate(void) {
  // Nothing specific to do on deactivation
  return 0;
}

/**
 * Render graph view
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param width Window width
 * @param height Window height
 * @return 0 on success, non-zero on failure
 */
static int graph_view_render(WINDOW *win, device_manager_t *manager, int width,
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

  // Draw either all graphs or a single expanded graph
  if (graph_view_layout == 0) {
    // All graphs in tiled layout
    draw_all_graphs(win, next_row, 1, width, height - next_row, device,
                    manager->view_state.show_grid);
  } else {
    // Single large graph
    draw_large_graph(win, next_row, 1, width, height - next_row, device,
                     manager->view_state.show_grid);
  }

  return 0;
}

/**
 * Handle input for graph view
 *
 * @param ch Input character
 * @param manager Device manager
 * @return 1 if the input was handled, 0 if not
 */
static int graph_view_handle_input(int ch, device_manager_t *manager) {
  // Handle view-specific input
  switch (ch) {
    case 'g':  // Toggle grid
    case 'G':
      manager->view_state.show_grid = !manager->view_state.show_grid;
      return 1;

    case 'a':  // Toggle auto-scale
    case 'A':
      manager->view_state.auto_scale = !manager->view_state.auto_scale;
      return 1;

    case KEY_UP:
      if (graph_view_layout == 0) {
        // Move selection up
        if (graph_view_selected_graph >= 2) {
          graph_view_selected_graph -= 2;
        }
      }
      return 1;

    case KEY_DOWN:
      if (graph_view_layout == 0) {
        // Move selection down
        if (graph_view_selected_graph < 4) {
          graph_view_selected_graph += 2;
        }
      }
      return 1;

    case KEY_LEFT:
      if (graph_view_layout == 0) {
        // Move selection left
        if (graph_view_selected_graph % 2 == 1) {
          graph_view_selected_graph--;
        }
      }
      return 1;

    case KEY_RIGHT:
      if (graph_view_layout == 0) {
        // Move selection right
        if (graph_view_selected_graph % 2 == 0 &&
            graph_view_selected_graph < 5) {
          graph_view_selected_graph++;
        }
      }
      return 1;

    case '\n':  // Enter key
    case KEY_ENTER:
      // Toggle between layouts
      graph_view_layout = !graph_view_layout;
      return 1;
  }

  return 0;  // Input not handled
}

/**
 * Clean up resources used by graph view
 */
static void graph_view_cleanup(void) {
  // Clean up any resources
  graph_view_initialized = 0;
}