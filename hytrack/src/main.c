/**
 * main.c
 * HYTRACK Main Application
 */

#include <getopt.h>
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../include/common/config.h"
#include "../include/common/ui_common.h"
#include "../include/core/device_manager.h"
#include "../include/hytrack.h"
#include "../include/views/debug_view.h"
#include "../include/views/view_manager.h"

// Global variables for signal handling
static int g_running = 1;
static WINDOW *g_win = NULL;
device_manager_t *g_device_manager = NULL;

/**
 * Parse command line arguments
 *
 * @param argc Argument count
 * @param argv Argument array
 * @param options Options structure to store results
 */
void parse_args(int argc, char *argv[], hytrack_options_t *options) {
  int opt;
  static struct option long_options[] = {
      {"device", required_argument, 0, 'd'},
      {"device2", required_argument, 0, '2'},
      {"mode", required_argument, 0, 'm'},
      {"interval", required_argument, 0, 'i'},
      {"view", required_argument, 0, 'v'},
      {"no-cursor", no_argument, 0, 'c'},
      {"debug", no_argument, 0, 'D'},
      {"help", no_argument, 0, 'h'},
      {"start-log", no_argument, 0, 'L'},
      {"log-mode", required_argument, 0, 'M'},
      {"log-dir", required_argument, 0, 'O'},
      {0, 0, 0, 0}};

  // Set default values
  options->device_count = 1;
  options->current_device = 0;
  options->mode = MODE_HYSSD;
  options->interval = DEFAULT_INTERVAL;
  options->starting_view = 0;
  options->hide_cursor = 1;
  options->debug_mode = 0;
  options->auto_start_log = 0;  // Default: don't auto-start logging
  options->log_mode = 0;  // Default: aggregate mode (0=aggregate, 1=individual)
  strcpy(options->log_directory, "logs");  // Default to logs subdirectory

  // Set default device
  strncpy(options->devices[0], DEFAULT_DEVICE, MAX_DEVICE_PATH - 1);
  options->devices[0][MAX_DEVICE_PATH - 1] = '\0';

  // Parse arguments
  while ((opt = getopt_long(argc, argv, "d:2:m:i:v:cDhLM:O:", long_options,
                            NULL)) != -1) {
    switch (opt) {
      case 'd':  // Device path
        strncpy(options->devices[0], optarg, MAX_DEVICE_PATH - 1);
        options->devices[0][MAX_DEVICE_PATH - 1] = '\0';
        break;

      case '2':  // Second device for multi-mode
        options->device_count = 2;
        strncpy(options->devices[1], optarg, MAX_DEVICE_PATH - 1);
        options->devices[1][MAX_DEVICE_PATH - 1] = '\0';
        break;

      case 'm':  // Operating mode
        if (strcmp(optarg, "hyssd") == 0) {
          options->mode = MODE_HYSSD;
        } else if (strcmp(optarg, "bbssd") == 0) {
          options->mode = MODE_BBSSD;
        } else if (strcmp(optarg, "multi") == 0) {
          options->mode = MODE_MULTI;
          if (options->device_count < 2) {
            options->device_count = 2;
            strncpy(options->devices[1], "/dev/nvme1n1", MAX_DEVICE_PATH - 1);
            options->devices[1][MAX_DEVICE_PATH - 1] = '\0';
          }
        } else {
          fprintf(stderr, "Unknown mode: %s. Using default.\n", optarg);
        }
        break;

      case 'i':  // Update interval
        options->interval = atof(optarg);
        if (options->interval <= 0.0) {
          options->interval = DEFAULT_INTERVAL;
        }
        break;

      case 'v':  // View index
        options->starting_view = atoi(optarg);
        break;

      case 'c':  // Cursor visibility
        options->hide_cursor = 1;
        break;

      case 'D':  // Debug mode
        options->debug_mode = 1;
        break;

      case 'L':  // Start logging immediately
        options->auto_start_log = 1;
        options->starting_view = VIEW_LOG;  // Set view to logging view
        break;

      case 'M':  // Log mode
        if (strcmp(optarg, "individual") == 0) {
          options->log_mode = 1;  // Individual mode
        } else if (strcmp(optarg, "aggregate") == 0) {
          options->log_mode = 0;  // Aggregate mode
        } else {
          fprintf(stderr, "Unknown log mode: %s. Using default (aggregate).\n",
                  optarg);
        }
        break;
      case 'O':  // Log directory
        strncpy(options->log_directory, optarg, MAX_DEVICE_PATH - 1);
        options->log_directory[MAX_DEVICE_PATH - 1] = '\0';
        break;
      case 'h':  // Help
      default:
        printf("HYTRACK - Advanced SSD Monitoring Tool\n\n");
        printf("Usage: %s [options]\n\n", argv[0]);
        printf("Options:\n");
        printf("  -d, --device=DEV     Device to monitor (default: %s)\n",
               DEFAULT_DEVICE);
        printf("  -2, --device2=DEV    Second device for multi-device mode\n");
        printf(
            "  -m, --mode=MODE      Operating mode: hyssd, bbssd, multi "
            "(default: hyssd)\n");
        printf(
            "  -i, --interval=SEC   Update interval in seconds (default: "
            "%.1f)\n",
            DEFAULT_INTERVAL);
        printf("  -v, --view=NUM       Starting view number (default: 0)\n");
        printf("  -c, --no-cursor      Hide cursor (default: on)\n");
        printf("  -D, --debug          Enable debug mode\n");
        printf(
            "  -L, --start-log      Auto-start logging (switches to log "
            "view)\n");
        printf(
            "  -M, --log-mode=MODE  Log mode: aggregate, individual (default: "
            "aggregate)\n");
        printf("  -h, --help           Show this help message\n");
        printf(
            "  -O, --log-dir=DIR     Directory to store log files (default: "
            "logs)\n");
        exit(opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE);
    }
  }
}

/**
 * Signal handler for clean termination
 *
 * @param sig Signal number
 */
void signal_handler(int sig) {
  g_running = 0;

  // Clean up ncurses
  if (g_win) {
    endwin();
  }

  // Clean up device manager
  if (g_device_manager) {
    device_manager_cleanup(g_device_manager);
  }

  exit(sig);
}

/**
 * Signal handler for logging control
 * Handles SIGUSR1 to stop logging and SIGUSR2 to start logging
 *
 * @param sig Signal number
 */
void log_signal_handler(int sig) {
  if (sig == SIGUSR1) {
    stop_logging_if_active();
  } else if (sig == SIGUSR2) {
    start_logging_if_inactive();
  }
}

/**
 * Main function
 *
 * @param argc Argument count
 * @param argv Argument array
 * @return Exit code
 */
int main(int argc, char *argv[]) {
  hytrack_options_t options;
  WINDOW *win;
  int ch;
  int width, height;
  struct timespec last_update = {0, 0};
  struct timespec now;
  int needs_redraw = 1;
  int data_valid = 0;

  // Parse command line arguments
  parse_args(argc, argv, &options);

  // Setup signal handlers
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGUSR1, log_signal_handler);  // Handler for stopping logging
  signal(SIGUSR2, log_signal_handler);  // Handler for starting logging

  // Initialize debug view if debug mode is enabled
  if (options.debug_mode) {
    debug_view_init();
  }

  // Initialize device manager
  g_device_manager = device_manager_init(&options);
  if (!g_device_manager) {
    fprintf(stderr, "Failed to initialize device manager\n");
    return EXIT_FAILURE;
  }

  // Initialize ncurses
  win = initscr();
  g_win = win;
  if (!win) {
    fprintf(stderr, "Failed to initialize ncurses\n");
    device_manager_cleanup(g_device_manager);
    return EXIT_FAILURE;
  }

  // Configure ncurses
  cbreak();           // Disable line buffering
  noecho();           // Don't echo input
  keypad(win, TRUE);  // Enable function keys, arrow keys, etc.
  if (options.hide_cursor) {
    curs_set(0);  // Hide cursor
  }
  timeout(10);  // Non-blocking getch with 10ms delay

  // Initialize colors
  init_colors();

  // Initialize view manager with view 0 first
  if (view_manager_init(0) != 0) {
    endwin();
    fprintf(stderr, "Failed to initialize view manager\n");
    device_manager_cleanup(g_device_manager);
    return EXIT_FAILURE;
  }

  // Register views based on mode
  view_manager_register_views(options.mode);

  // Switch to the selected starting view (this will trigger activation with the
  // device manager)
  view_manager_switch(options.starting_view, g_device_manager);

  // Main loop - separates data polling from input handling for responsive UI
  while (g_running) {
    // Check if it's time to update device data
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - last_update.tv_sec) +
                     (now.tv_nsec - last_update.tv_nsec) / 1e9;

    if (elapsed >= options.interval || !data_valid) {
      if (device_manager_update_data(g_device_manager) == 0) {
        data_valid = 1;
      } else {
        data_valid = 0;
      }
      clock_gettime(CLOCK_MONOTONIC, &last_update);
      needs_redraw = 1;
    }

    // Drain all pending input at once for responsive scrolling
    while ((ch = getch()) != ERR) {
      if (ch == 'q' || ch == 'Q') {
        g_running = 0;
        break;
      } else if (ch == 'r' || ch == 'R') {
        data_valid = 0;  // Force data refresh
        needs_redraw = 1;
      } else if (ch == KEY_RESIZE) {
        clear();
        refresh();
        needs_redraw = 1;
      } else {
        if (view_manager_handle_input(ch, g_device_manager)) {
          needs_redraw = 1;
        }
      }
    }

    // Redraw only when needed
    if (needs_redraw) {
      getmaxyx(win, height, width);
      clear();
      if (data_valid) {
        view_manager_draw_header(win, g_device_manager, width);
        view_manager_render(win, g_device_manager, width, height);
      } else {
        mvprintw(
            1, 1,
            "Error updating device data. Press 'q' to quit or 'r' to retry.");
      }
      refresh();
      needs_redraw = 0;
    }

    // Short sleep to prevent CPU spinning (10ms)
    usleep(10000);
  }

  // Cleanup
  view_manager_cleanup();
  endwin();
  device_manager_cleanup(g_device_manager);

  return EXIT_SUCCESS;
}