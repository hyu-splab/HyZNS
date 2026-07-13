/**
 * log_view.c
 * Unified Log View Implementation for all HYTRACK modes
 */

#include "../../include/views/log_view.h"

#include <assert.h>
#include <errno.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../../include/common/global_state.h"
#include "../../include/common/log_utils.h"
#include "../../include/common/ui_common.h"
#include "../../include/core/device_manager.h"
#include "../../include/modes/bbssd_mode.h"
#include "../../include/modes/hyssd_mode.h"
#include "../../include/modes/multi_mode.h"
#include "../../include/modes/znssd_mode.h"
#include "../../include/views/debug_view.h"

// Forward declarations of private functions
static int log_view_init(void);
static int log_view_activate(
    device_manager_t *manager);  // Modified to accept manager
static int log_view_deactivate(void);
static int log_view_render(WINDOW *win, device_manager_t *manager, int width,
                           int height);
static int log_view_handle_input(int ch, device_manager_t *manager);
static void log_view_cleanup(void);

// Mode-specific functions
static int log_start_recording(device_manager_t *manager);
static int log_stop_recording(device_manager_t *manager);
static int log_record_data(device_manager_t *manager);

// HYSSD specific functions
int hyssd_log_start_recording(device_manager_t *manager);
static int hyssd_log_record_data(device_manager_t *manager);
static int hyssd_log_render_config(WINDOW *win, device_manager_t *manager,
                                   int row, int width);
static int hyssd_log_handle_input(int ch, device_manager_t *manager);

// BBSSD specific functions
int bbssd_log_start_recording(device_manager_t *manager);
static int bbssd_log_record_data(device_manager_t *manager);
static int bbssd_log_render_config(WINDOW *win, device_manager_t *manager,
                                   int row, int width);
static int bbssd_log_handle_input(int ch, device_manager_t *manager);

// Multi-device specific functions
int multi_log_start_recording(device_manager_t *manager);
static int multi_log_record_data(device_manager_t *manager);
static int multi_log_render_config(WINDOW *win, device_manager_t *manager,
                                   int row, int width);
static int multi_log_handle_input(int ch, device_manager_t *manager);

// Global logging state
static log_view_state_t log_state = {0};

// Added global variable to track if auto-start has been processed
static int auto_start_processed = 0;

// View interface implementation
view_interface_t log_view = {
    .name = "Logging",
    .description = "Log device data to file for later analysis",
    .key = "7",
    .init = log_view_init,
    .activate = log_view_activate,
    .deactivate = log_view_deactivate,
    .render = log_view_render,
    .handle_input = log_view_handle_input,
    .cleanup = log_view_cleanup};

/**
 * Initialize log view
 *
 * @return 0 on success, non-zero on failure
 */
static int log_view_init(void) {
  if (log_state.common.initialized) {
    return 0;
  }

  log_utils_init_state(&log_state.common);
  log_state.szone_log_mode = SZONE_LOG_AGGREGATE;
  log_state.include_all_devices =
      1;  // Default: include all devices in multi-device mode

  // Set global state pointer for signal handler access
  g_log_state = &log_state;

  auto_start_processed = 0;  // Reset auto-start flag

  debug_log_add("Log view initialized");
  return 0;
}

/**
 * Activate log view
 *
 * @param manager Device manager containing options
 * @return 0 on success, non-zero on failure
 */
static int log_view_activate(device_manager_t *manager) {
  debug_log_add("Log view activated");

  // Check if manager is valid
  if (!manager || !manager->options) {
    debug_log_add("Log view activated with invalid manager");
    return 0;
  }

  // Get options from device manager
  hytrack_options_t *options = manager->options;

  // Check if we need to set the log mode from command line
  if (options->log_mode == 1) {
    // Set to individual mode
    log_state.szone_log_mode = SZONE_LOG_INDIVIDUAL;
    debug_log_add("Log mode set to individual from command line");
  } else {
    // Set/keep as aggregate mode (default)
    log_state.szone_log_mode = SZONE_LOG_AGGREGATE;
  }

  // Get log directory from options
  if (options->log_directory[0] != '\0') {
    strncpy(log_state.common.log_directory, options->log_directory,
            sizeof(log_state.common.log_directory) - 1);
    log_state.common.log_directory[sizeof(log_state.common.log_directory) - 1] =
        '\0';
    debug_log_add("Log directory set to %s from command line",
                  log_state.common.log_directory);
  }

  // Check if we need to auto-start logging
  if (options->auto_start_log && !auto_start_processed) {
    debug_log_add("Auto-starting logging from command line");
    log_start_recording(manager);
    auto_start_processed = 1;  // Mark as processed to avoid multiple starts
  }

  return 0;
}

/**
 * Deactivate log view
 *
 * @return 0 on success, non-zero on failure
 */
static int log_view_deactivate(void) {
  debug_log_add("Log view deactivated");
  return 0;
}

/**
 * Mode-specific start recording dispatcher
 */
static int log_start_recording(device_manager_t *manager) {
  switch (manager->mode) {
    case MODE_HYSSD:
      return hyssd_log_start_recording(manager);
    case MODE_BBSSD:
      return bbssd_log_start_recording(manager);
    case MODE_MULTI:
      return multi_log_start_recording(manager);
    default:
      debug_log_add("Logging not supported in current mode");
      return -1;
  }
}

/**
 * Mode-specific stop recording dispatcher
 */
static int log_stop_recording(device_manager_t *manager) {
  return log_utils_stop_recording(&log_state.common);
}

/**
 * Mode-specific record data dispatcher
 */
static int log_record_data(device_manager_t *manager) {
  if (!log_state.common.recording_active || !log_state.common.log_file) {
    return -1;
  }

  // Increment counter and check if we should record this update
  log_state.common.update_counter++;
  if (log_state.common.update_counter < log_state.common.update_interval) {
    return 0;
  }
  log_state.common.update_counter = 0;

  switch (manager->mode) {
    case MODE_HYSSD:
      return hyssd_log_record_data(manager);
    case MODE_BBSSD:
      return bbssd_log_record_data(manager);
    case MODE_MULTI:
      return multi_log_record_data(manager);
    default:
      return -1;
  }
}

/**
 * Render log view
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param width Window width
 * @param height Window height
 * @return 0 on success, non-zero on failure
 */
static int log_view_render(WINDOW *win, device_manager_t *manager, int width,
                           int height) {
  int row;
  struct timespec current_time;
  double elapsed_sec = 0;
  time_t now;
  struct tm *local_time;
  char time_str[64];
  char mode_name[32];

  // Get mode name
  switch (manager->mode) {
    case MODE_HYSSD:
      strcpy(mode_name, "HYSSD");
      break;
    case MODE_BBSSD:
      strcpy(mode_name, "BBSSD");
      break;
    case MODE_MULTI:
      strcpy(mode_name, "MULTI");
      break;
    default:
      strcpy(mode_name, "UNKNOWN");
      break;
  }

  // Calculate layout
  width = width - 2;  // Adjust for padding

  // Start row position after header
  row = 3;

  // For multi-device mode, add an extra row for device selector
  if (manager->device_count > 1) {
    row = 4;
  }

  // Draw status box with fixed height
  int status_box_height = 9;
  draw_box_with_title(win, row, 1, status_box_height, width, "Log Status",
                      COLOR_PAIR_NORMAL);

  // Content inside status box
  int content_row = row + 2;

  // Current mode
  mvwprintw(win, content_row++, 3, "Mode: %s", mode_name);

  // Current device(s)
  if (manager->mode == MODE_MULTI && log_state.include_all_devices) {
    mvwprintw(win, content_row++, 3, "Device: ALL DEVICES");
  } else {
    device_context_t *device = &manager->devices[manager->current_device];
    mvwprintw(win, content_row++, 3, "Device: %s", device->device_path);
  }

  // Recording status
  if (log_state.common.recording_active) {
    // Calculate elapsed time with high precision
    log_utils_get_high_precision_time(&current_time);
    elapsed_sec = log_utils_calculate_elapsed_time(log_state.common.start_time,
                                                   current_time);

    // For display purposes, use standard time
    time(&now);
    local_time = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", local_time);

    wattron(win, COLOR_PAIR(COLOR_PAIR_GREEN));
    mvwprintw(win, content_row++, 3, "Status: RECORDING");
    wattroff(win, COLOR_PAIR(COLOR_PAIR_GREEN));

    mvwprintw(win, content_row++, 3, "Log file: %s",
              log_state.common.log_file_path);
    mvwprintw(win, content_row++, 3, "Recording time: %.3f seconds",
              elapsed_sec);
    mvwprintw(win, content_row++, 3, "Records: %u (Interval: every %d updates)",
              log_state.common.record_count, log_state.common.update_interval);
    // mvwprintw(win, content_row++, 3, "Current time: %s", time_str);
  } else {
    wattron(win, COLOR_PAIR(COLOR_PAIR_RED));
    mvwprintw(win, content_row++, 3, "Status: STOPPED");
    wattroff(win, COLOR_PAIR(COLOR_PAIR_RED));

    if (log_state.common.record_count > 0) {
      mvwprintw(win, content_row++, 3, "Last log file: %s",
                log_state.common.log_file_path);
      mvwprintw(win, content_row++, 3, "Records: %u",
                log_state.common.record_count);
    } else {
      mvwprintw(win, content_row++, 3, "No log files created yet");
    }
  }

  // Move to the position after status box
  row += status_box_height + 1;

  // Draw configuration box with dynamic height based on mode
  int config_box_height = 15;  // Default height

  // Render mode-specific configuration
  switch (manager->mode) {
    case MODE_HYSSD:
      config_box_height = 17;  // HYSSD needs more height for SZone mode
      break;
    case MODE_MULTI:
      config_box_height = 19;  // Multi mode needs height for device selection
      break;
    case MODE_BBSSD:
    default:
      config_box_height = 15;
      break;
  }

  draw_box_with_title(win, row, 1, config_box_height, width,
                      "Log Configuration", COLOR_PAIR_NORMAL);

  // Common configuration settings
  int content_row_start = row + 2;
  content_row = content_row_start;

  mvwprintw(win, content_row++, 3, "Log directory: %s",
            log_state.common.log_directory);
  mvwprintw(win, content_row++, 3, "Logging interval: every %d updates",
            log_state.common.update_interval);

  // Timing precision information
  wattron(win, COLOR_PAIR(COLOR_PAIR_YELLOW));
  mvwprintw(win, content_row++, 3,
            "Timing precision: Millisecond precision (0.001s)");
  wattroff(win, COLOR_PAIR(COLOR_PAIR_YELLOW));

  content_row++;  // Add a blank line

  // Render mode-specific configuration
  content_row = content_row_start + 4;  // Start after common config
  switch (manager->mode) {
    case MODE_HYSSD:
      hyssd_log_render_config(win, manager, content_row, width);
      break;
    case MODE_BBSSD:
      bbssd_log_render_config(win, manager, content_row, width);
      break;
    case MODE_MULTI:
      multi_log_render_config(win, manager, content_row, width);
      break;
  }

  // If logging is active, record data on each render
  if (log_state.common.recording_active) {
    log_record_data(manager);
  }

  return 0;
}

/**
 * Handle input for log view
 *
 * @param ch Input character
 * @param manager Device manager
 * @return 1 if the input was handled, 0 if not
 */
static int log_view_handle_input(int ch, device_manager_t *manager) {
  // Common controls for all modes
  switch (ch) {
    case 's':  // Start recording
    case 'S':
      if (!log_state.common.recording_active) {
        log_start_recording(manager);
      }
      return 1;

    case 'p':  // Stop recording (p for pause)
    case 'P':
      if (log_state.common.recording_active) {
        log_stop_recording(manager);
      }
      return 1;

    case '+':  // Increase logging interval
      log_state.common.update_interval++;
      debug_log_add("Logging interval increased to %d",
                    log_state.common.update_interval);
      return 1;

    case '-':  // Decrease logging interval
      if (log_state.common.update_interval > 1) {
        log_state.common.update_interval--;
        debug_log_add("Logging interval decreased to %d",
                      log_state.common.update_interval);
      }
      return 1;
  }

  // Mode-specific controls
  switch (manager->mode) {
    case MODE_HYSSD:
      return hyssd_log_handle_input(ch, manager);
    case MODE_BBSSD:
      return bbssd_log_handle_input(ch, manager);
    case MODE_MULTI:
      return multi_log_handle_input(ch, manager);
    default:
      return 0;
  }
}

/**
 * Clean up resources used by log view
 */
static void log_view_cleanup(void) {
  // Stop recording if active
  if (log_state.common.recording_active) {
    log_utils_stop_recording(&log_state.common);
  }

  // Reset global state pointer
  g_log_state = NULL;

  debug_log_add("Log view cleanup");
  log_state.common.initialized = 0;
}

/*
 * HYSSD MODE IMPLEMENTATION
 */

/**
 * Start recording log data for HYSSD mode
 */
int hyssd_log_start_recording(device_manager_t *manager) {
  device_context_t *device = &manager->devices[manager->current_device];
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;

  // Check if we have valid data
  if (!data || !data->lm_info) {
    debug_log_add("Warning: No valid HYSSD data to log");  // TODO
    // return -1;
  }

  // Build CSV header
  char header[4096];

  // Common information for both SZone modes
  strcpy(header, "timestamp,elapsed_sec,");
  strcat(header,
         "total_lines,rzone_count,free_lines,victim_lines,full_lines,total_ipc,"
         "total_vpc,gc_count,gc_pgs,rzone_utilization_pct,");
  strcat(header,
         "szone_count,szone_total_capacity,open_zones,full_zones,zr_count,"
         "szone_utilization_pct");

  // Mode-specific headers
  if (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL) {
    // For individual mode, add a separate column for each sequential zone
    for (int i = 0; i < data->num_zones; i++) {
      if (!data->zone_info[i].rnd) {
        // Calculate zone ID for clearer labeling
        hyssd_zone_info_t *zone = &data->zone_info[i];
        int zone_id = (zone->d.zcap > 0) ? (zone->d.zslba / zone->d.zcap) : i;
        char zone_column[32];
        snprintf(zone_column, sizeof(zone_column), ",z#%d", zone_id);
        strcat(header, zone_column);
      }
    }
  } else {
    // For aggregate mode, just add one column for total usage
    strcat(header, ",szone_total_used");
  }

  // Create log file with HYSSD-specific mode indicator
  const char *mode_str = (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL)
                             ? "hyssd_individual"
                             : "hyssd_aggregate";

  return log_utils_start_recording(&log_state.common, device, header, mode_str);
}

/**
 * Record HYSSD data to log file
 */
static int hyssd_log_record_data(device_manager_t *manager) {
  device_context_t *device = &manager->devices[manager->current_device];
  hyssd_data_t *data = (hyssd_data_t *)device->device_data;
  struct timespec current_time;

  if (!data || !data->lm_info) {
    return -1;
  }

  // Get current high precision time
  log_utils_get_high_precision_time(&current_time);
  double elapsed_sec = log_utils_calculate_elapsed_time(
      log_state.common.start_time, current_time);

  // Format timestamp for CSV
  char timestamp[64];
  log_utils_format_timestamp(current_time, timestamp, sizeof(timestamp), 1);

  // Count zones and calculate totals
  int total_zones = data->num_zones;
  int open_zones = 0;
  int full_zones = 0;
  int rand_zones = 0;
  int sequential_zones = 0;
  uint64_t total_szone_capacity = 0;
  uint64_t total_szone_used = 0;
  double rzone_utilization_pct = 0.0;
  double szone_utilization_pct = 0.0;

  // Process all zones to calculate statistics
  for (int i = 0; i < total_zones; i++) {
    hyssd_zone_info_t *zone = &data->zone_info[i];

    if (zone->rnd) {
      rand_zones++;
    } else {
      sequential_zones++;

      // Add to total capacity for SZones
      total_szone_capacity += zone->d.zcap;

      // Calculate used space
      uint64_t zone_used = 0;
      if (zone->w_ptr > zone->d.zslba) {
        zone_used = zone->w_ptr - zone->d.zslba;
      }
      total_szone_used += zone_used;
    }

    // Zone is open if write pointer is not at start or end
    if (zone->w_ptr > zone->d.zslba &&
        zone->w_ptr < (zone->d.zslba + zone->d.zcap)) {
      open_zones++;
    }

    // Zone is full if write pointer reached the end
    if (zone->w_ptr >= (zone->d.zslba + zone->d.zcap)) {
      full_zones++;
    }
  }

  // Calculate total IPC and VPC across all lines and get RZone utilization
  int total_ipc = 0;
  int total_vpc = 0;

  for (int i = 0; i < data->lm_info->tt_lines; i++) {
    if (data->lm_info->lines[i].is_rnd) {
      total_ipc += data->lm_info->lines[i].ipc;
      total_vpc += data->lm_info->lines[i].vpc;
    }
  }

  // Calculate utilization percentages
  if (total_vpc + total_ipc > 0 && device->device_info->hyssd.tt_rpgs > 0) {
    rzone_utilization_pct = (double)(total_vpc + total_ipc) /
                            device->device_info->hyssd.tt_rpgs * 100.0;
  }

  if (total_szone_capacity > 0) {
    szone_utilization_pct =
        (double)total_szone_used / total_szone_capacity * 100.0;
  }

  assert(log_state.common.log_file != NULL);

  // Write basic data to CSV file
  fprintf(log_state.common.log_file, "%s,%.3f,", timestamp, elapsed_sec);

  // Rzone information
  fprintf(log_state.common.log_file, "%d,%d,%d,%d,%d,%d,%d,%lu,%lu,%.2f,",
          data->lm_info->tt_lines, rand_zones, data->lm_info->free_line_cnt,
          data->lm_info->victim_line_cnt, data->lm_info->full_line_cnt,
          total_ipc, total_vpc, data->gc_count, data->gc_pgs, rzone_utilization_pct);

  // Szone information
  fprintf(log_state.common.log_file, "%d,%lu,%d,%d,%lu,%.2f", sequential_zones,
          total_szone_capacity, open_zones, full_zones, data->zr_count, szone_utilization_pct);

  // SZone utilization based on mode
  if (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL) {
    // Write individual zone data (only for sequential zones)
    for (int i = 0; i < total_zones; i++) {
      if (!data->zone_info[i].rnd) {
        // Calculate zone utilization
        hyssd_zone_info_t *zone = &data->zone_info[i];
        uint64_t zone_used = 0;
        if (zone->w_ptr > zone->d.zslba) {
          zone_used = zone->w_ptr - zone->d.zslba;
        }

        fprintf(log_state.common.log_file, ",%lu", zone_used);
      }
    }
  } else {
    // Aggregate SZone statistics
    fprintf(log_state.common.log_file, ",%lu", total_szone_used);
  }

  fprintf(log_state.common.log_file, "\n");

  // Flush file to ensure data is written
  fflush(log_state.common.log_file);

  log_state.common.record_count++;
  return 0;
}

/**
 * Render HYSSD specific configuration
 */
static int hyssd_log_render_config(WINDOW *win, device_manager_t *manager,
                                   int row, int width) {
  int content_row = row;

  // SZone mode
  wattron(win, COLOR_PAIR(COLOR_PAIR_CYAN));
  mvwprintw(win, content_row++, 3, "SZone logging mode: %s",
            (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL) ? "Individual"
                                                               : "Aggregate");
  wattroff(win, COLOR_PAIR(COLOR_PAIR_CYAN));

  content_row++;  // Add a blank line

  // Info about logged data
  mvwprintw(win, content_row++, 3, "Data logged:");
  mvwprintw(win, content_row++, 5,
            "- Line stats: total, free, victim, full counts, total IPC, total "
            "VPC, utilization");
  mvwprintw(win, content_row++, 5,
            "- Zone stats: total, open, full counts, total capacity, total "
            "used, utilization");

  content_row++;  // Add some spacing

  // Controls section
  wattron(win, A_BOLD);
  mvwprintw(win, content_row++, 3, "Controls:");
  wattroff(win, A_BOLD);

  // Calculate position for two-column layout
  int left_column = 5;
  int right_column = width / 2 + 1;

  mvwprintw(win, content_row, left_column, "s - Start recording");
  mvwprintw(win, content_row++, right_column, "p - Stop recording");

  mvwprintw(win, content_row, left_column, "+ - Increase logging interval");
  mvwprintw(win, content_row++, right_column, "- - Decrease logging interval");

  mvwprintw(win, content_row++, left_column,
            "m - Toggle SZone logging mode (Individual/Aggregate)");

  return 0;
}

/**
 * Handle HYSSD specific input
 */
static int hyssd_log_handle_input(int ch, device_manager_t *manager) {
  switch (ch) {
    case 'm':  // Toggle SZone logging mode
    case 'M':
      if (log_state.common.recording_active) {
        debug_log_add("Cannot change SZone mode while recording");
      } else {
        log_state.szone_log_mode =
            (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL)
                ? SZONE_LOG_AGGREGATE
                : SZONE_LOG_INDIVIDUAL;
        debug_log_add("SZone logging mode changed to %s",
                      (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL)
                          ? "Individual"
                          : "Aggregate");
      }
      return 1;
  }

  return 0;  // Input not handled
}

/*
 * BBSSD MODE IMPLEMENTATION
 */

/**
 * Start recording log data for BBSSD mode
 */
int bbssd_log_start_recording(device_manager_t *manager) {
  device_context_t *device = &manager->devices[manager->current_device];
  bbssd_data_t *data = (bbssd_data_t *)device->device_data;

  // Check if we have valid data
  if (!data || !data->lm_info) {
    debug_log_add("Warning: No valid BBSSD data to log");
    // return -1;
  }

  // Build CSV header for BBSSD logs
  char header[4096];
  strcpy(header, "timestamp,elapsed_sec,");
  strcat(header,
         "total_lines,free_lines,victim_lines,full_lines,total_ipc,total_vpc,"
         "wp_line,gc_count,gc_pgs,utilization_pct");

  return log_utils_start_recording(&log_state.common, device, header, "bbssd");
}

/**
 * Record BBSSD data to log file
 */
static int bbssd_log_record_data(device_manager_t *manager) {
  device_context_t *device = &manager->devices[manager->current_device];
  bbssd_data_t *data = (bbssd_data_t *)device->device_data;
  struct timespec current_time;

  if (!data || !data->lm_info) {
    return -1;
  }

  // Get current high precision time
  log_utils_get_high_precision_time(&current_time);
  double elapsed_sec = log_utils_calculate_elapsed_time(
      log_state.common.start_time, current_time);

  // Format timestamp for CSV
  char timestamp[64];
  log_utils_format_timestamp(current_time, timestamp, sizeof(timestamp), 1);

  // Calculate BBSSD statistics
  int total_lines = data->lm_info->tt_lines;
  int free_lines = data->lm_info->free_line_cnt;
  int victim_lines = data->lm_info->victim_line_cnt;
  int full_lines = data->lm_info->full_line_cnt;
  int total_ipc = 0;
  int total_vpc = 0;
  int wp_line = data->wp_info.curline_id;
  double bbssd_utilization_pct = 0.0;

  // Process all lines to calculate total IPC and VPC
  for (int i = 0; i < total_lines; i++) {
    bbssd_line_info_t *line = &data->lm_info->lines[i];
    total_ipc += line->ipc;
    total_vpc += line->vpc;
  }

  // Calculate utilization percentage (similar to HYSSD calculation)
  if (device->device_info && device->device_info->bbssd.tt_pgs > 0) {
    bbssd_utilization_pct = (double)(total_vpc + total_ipc) /
                            device->device_info->bbssd.tt_pgs * 100.0;
  }

  assert(log_state.common.log_file != NULL);

  // Write BBSSD metrics to CSV
  fprintf(log_state.common.log_file, "%s,%.3f,%d,%d,%d,%d,%d,%d,%d,%lu,%lu,%.2f\n",
          timestamp, elapsed_sec, total_lines, free_lines, victim_lines,
          full_lines, total_ipc, total_vpc, wp_line, data->gc_count, data->gc_pgs, bbssd_utilization_pct);

  // Flush file to ensure data is written
  fflush(log_state.common.log_file);

  log_state.common.record_count++;
  return 0;
}

/**
 * Render BBSSD specific configuration
 */
static int bbssd_log_render_config(WINDOW *win, device_manager_t *manager,
                                   int row, int width) {
  int content_row = row;

  // Info about logged data
  mvwprintw(win, content_row++, 3, "Data logged:");
  mvwprintw(win, content_row++, 5,
            "- Line stats: total, free, victim, full counts");
  mvwprintw(win, content_row++, 5,
            "- Page stats: total valid pages, total invalid pages, utilization "
            "percentage");
  mvwprintw(win, content_row++, 5, "- Write pointer: current line position");

  content_row++;  // Add some spacing

  // Controls section
  wattron(win, A_BOLD);
  mvwprintw(win, content_row++, 3, "Controls:");
  wattroff(win, A_BOLD);

  // Calculate position for two-column layout
  int left_column = 5;
  int right_column = width / 2 + 1;

  mvwprintw(win, content_row, left_column, "s - Start recording");
  mvwprintw(win, content_row++, right_column, "p - Stop recording");

  mvwprintw(win, content_row, left_column, "+ - Increase logging interval");
  mvwprintw(win, content_row++, right_column, "- - Decrease logging interval");

  return 0;
}

/**
 * Handle BBSSD specific input
 */
static int bbssd_log_handle_input(int ch, device_manager_t *manager) {
  // Currently no BBSSD-specific inputs
  return 0;  // Input not handled
}

/*
 * MULTI-DEVICE MODE IMPLEMENTATION
 */

/**
 * Start recording log data for multi-device mode
 */
int multi_log_start_recording(device_manager_t *manager) {
  // If we're recording all devices, create a combined log file
  if (log_state.include_all_devices) {
    // Create a temporary device for log file naming
    device_context_t temp_device;
    strcpy(temp_device.device_path, "multi_all_devices");

    // Build CSV header for multi-device logs
    char header[4096];
    strcpy(header, "timestamp,elapsed_sec,");

    // BBSSD data columns (same for both modes)
    strcat(header,
           "total_lines,bbssd_count,free_lines,victim_lines,full_lines,total_"
           "ipc,total_vpc,gc_count,gc_pgs,bbssd_utilization_pct,");

    // ZNSSD data columns
    if (log_state.szone_log_mode == SZONE_LOG_AGGREGATE) {
      // Aggregate mode
      strcat(header,
             "zns_zone_count,zns_zone_total_capacity,open_zones,full_zones,zr_count,"
             "zns_utilization_pct,zns_zone_total_used");
    } else {
      // Individual mode
      strcat(header,
             "zns_zone_count,zns_zone_total_capacity,open_zones,full_zones,zr_count,"
             "zns_utilization_pct");

      // Find ZNSSD devices and add individual zone columns
      for (int i = 0; i < manager->device_count; i++) {
        device_context_t *device = &manager->devices[i];

        if (device->device_type == DEVICE_ZNSSD) {
          znssd_data_t *znssd = (znssd_data_t *)device->device_data;

          if (znssd && znssd->znssd_data) {
            // Add a column for each zone
            for (uint64_t z = 0; z < znssd->num_zones; z++) {
              char zone_column[32];
              snprintf(zone_column, sizeof(zone_column), ",z#%ld", z);
              strcat(header, zone_column);
            }
          }
        }
      }
    }

    // 모드에 따라 다른 파일명 사용
    const char *mode_str = (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL)
                               ? "multi_individual"
                               : "multi_aggregate";

    return log_utils_start_recording(&log_state.common, &temp_device, header,
                                     mode_str);
  } else {
    // If we're only recording the current device, use device-specific recording
    device_context_t *device = &manager->devices[manager->current_device];

    switch (device->device_type) {
      case DEVICE_BBSSD:
        return bbssd_log_start_recording(manager);
      case DEVICE_ZNSSD:
        debug_log_add("ZNSSD-specific logging not implemented");
        return -1;
      default:
        debug_log_add("Unknown device type for logging");
        return -1;
    }
  }
}

/**
 * Record multi-device data to log file
 */
static int multi_log_record_data(device_manager_t *manager) {
  struct timespec current_time;

  // Get current high precision time
  log_utils_get_high_precision_time(&current_time);
  double elapsed_sec = log_utils_calculate_elapsed_time(
      log_state.common.start_time, current_time);

  // Format timestamp for CSV
  char timestamp[64];
  log_utils_format_timestamp(current_time, timestamp, sizeof(timestamp), 1);

  // If include_all_devices is enabled, log data for all devices
  if (log_state.include_all_devices) {
    // Common BBSSD statistics
    int total_lines = 0;
    int bbssd_count = 0;
    int free_lines = 0;
    int victim_lines = 0;
    int full_lines = 0;
    int total_ipc = 0;
    int total_vpc = 0;
    uint64_t gc_count = 0;
    uint64_t gc_pgs = 0;
    double bbssd_utilization_pct = 0.0;

    // Common ZNSSD statistics
    uint64_t zns_zone_count = 0;
    uint64_t zns_zr_count = 0;
    uint64_t zns_zone_total_capacity = 0;
    int open_zones = 0;
    int full_zones = 0;
    double zns_utilization_pct = 0.0;
    uint64_t zns_zone_total_used = 0;

    // Collect BBSSD data
    for (int i = 0; i < manager->device_count; i++) {
      device_context_t *device = &manager->devices[i];
      bbssd_data_t *bbssd = (bbssd_data_t *)device->device_data;
      if (device->device_type == DEVICE_BBSSD) {
        bbssd_count++;

        if (bbssd && bbssd->lm_info) {
          // Add to totals
          total_lines += bbssd->lm_info->tt_lines;
          free_lines += bbssd->lm_info->free_line_cnt;
          victim_lines += bbssd->lm_info->victim_line_cnt;
          full_lines += bbssd->lm_info->full_line_cnt;
          gc_count += bbssd->gc_count;
          gc_pgs += bbssd->gc_pgs;
          // Calculate IPC and VPC
          for (int j = 0; j < bbssd->lm_info->tt_lines; j++) {
            total_ipc += bbssd->lm_info->lines[j].ipc;
            total_vpc += bbssd->lm_info->lines[j].vpc;
          }
        }
      }
    }

    // Calculate BBSSD utilization
    if (bbssd_count > 0 && total_lines > 0) {
      bbssd_utilization_pct =
          (double)(total_lines - free_lines) / total_lines * 100.0;
    }

    // Collect ZNSSD data and track individual zone usage
    znssd_data_t *znssd_data = NULL;
    uint64_t *zone_used_values = NULL;

    for (int i = 0; i < manager->device_count; i++) {
      device_context_t *device = &manager->devices[i];

      if (device->device_type == DEVICE_ZNSSD) {
        znssd_data_t *znssd = (znssd_data_t *)device->device_data;

        if (znssd && znssd->znssd_data) {
          // If in individual mode and this is the first ZNSSD device, store for
          // later
          if (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL &&
              znssd_data == NULL) {
            znssd_data = znssd;
            zone_used_values =
                (uint64_t *)malloc(znssd->num_zones * sizeof(uint64_t));
            if (zone_used_values == NULL) {
              debug_log_add("Failed to allocate memory for zone data");
              return -1;
            }
            memset(zone_used_values, 0, znssd->num_zones * sizeof(uint64_t));
          }

          // Add to zone counts
          zns_zone_count += znssd->num_zones;
          zns_zr_count += znssd->zr_count;

          // Process all zones
          for (uint64_t j = 0; j < znssd->num_zones; j++) {
            znssd_zone_info_t *zone = &znssd->znssd_data[j];

            // Add to capacity
            zns_zone_total_capacity += zone->d.zcap;

            // Check zone state
            if (zone->w_ptr > zone->d.zslba &&
                zone->w_ptr < (zone->d.zslba + zone->d.zcap)) {
              open_zones++;
            }

            if (zone->w_ptr >= (zone->d.zslba + zone->d.zcap)) {
              full_zones++;
            }

            // Calculate used space
            uint64_t zone_used = 0;
            if (zone->w_ptr > zone->d.zslba) {
              zone_used = zone->w_ptr - zone->d.zslba;
            }
            zns_zone_total_used += zone_used;

            // Store individual zone usage if this is the saved device
            if (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL &&
                znssd == znssd_data && j < znssd->num_zones) {
              zone_used_values[j] = zone_used;
            }
          }
        }
      }
    }

    // Calculate ZNSSD utilization
    if (zns_zone_total_capacity > 0) {
      zns_utilization_pct =
          (double)zns_zone_total_used / zns_zone_total_capacity * 100.0;
    }

    assert(log_state.common.log_file != NULL);

    // Write to CSV file
    fprintf(log_state.common.log_file, "%s,%.3f,%d,%d,%d,%d,%d,%d,%d,%lu,%lu,%.2f,",
            timestamp, elapsed_sec, total_lines, bbssd_count, free_lines,
            victim_lines, full_lines, total_ipc, total_vpc, gc_count, gc_pgs,
            bbssd_utilization_pct);

    // Write ZNSSD data
    fprintf(log_state.common.log_file, "%lu,%lu,%d,%d,%lu,%.2f", zns_zone_count,
            zns_zone_total_capacity, open_zones, full_zones,
            zns_zr_count, zns_utilization_pct);

    if (log_state.szone_log_mode == SZONE_LOG_AGGREGATE) {
      // For aggregate mode, add total used
      fprintf(log_state.common.log_file, ",%lu", zns_zone_total_used);
    } else {
      // For individual mode, add each zone's usage
      if (znssd_data && zone_used_values) {
        for (uint64_t z = 0; z < znssd_data->num_zones; z++) {
          fprintf(log_state.common.log_file, ",%lu", zone_used_values[z]);
        }
      }
    }

    fprintf(log_state.common.log_file, "\n");

    // Clean up
    if (zone_used_values) {
      free(zone_used_values);
    }
  } else {
    // If we're only logging the current device, use device-specific logging
    device_context_t *device = &manager->devices[manager->current_device];

    switch (device->device_type) {
      case DEVICE_BBSSD:
        return bbssd_log_record_data(manager);
      case DEVICE_ZNSSD:
        debug_log_add("ZNSSD-specific logging not implemented");
        return -1;
      default:
        return -1;
    }
  }

  // Flush file to ensure data is written
  fflush(log_state.common.log_file);

  log_state.common.record_count++;
  return 0;
}

/**
 * Render multi-device specific configuration
 */
static int multi_log_render_config(WINDOW *win, device_manager_t *manager,
                                   int row, int width) {
  int content_row = row;

  // Multi-device mode
  wattron(win, COLOR_PAIR(COLOR_PAIR_CYAN));
  mvwprintw(
      win, content_row++, 3, "Multi-device logging: %s",
      log_state.include_all_devices ? "All devices" : "Current device only");
  wattroff(win, COLOR_PAIR(COLOR_PAIR_CYAN));

  // SZone mode (only visible when logging all devices)
  if (log_state.include_all_devices) {
    wattron(win, COLOR_PAIR(COLOR_PAIR_CYAN));
    mvwprintw(win, content_row++, 3, "Logging mode: %s",
              (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL) ? "Individual"
                                                                 : "Aggregate");
    wattroff(win, COLOR_PAIR(COLOR_PAIR_CYAN));
  }

  content_row++;  // Add a blank line

  // Info about logged data
  mvwprintw(win, content_row++, 3, "Data logged:");

  if (log_state.include_all_devices) {
    if (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL) {
      mvwprintw(win, content_row++, 5,
                "- BBSSD: lines (total, free, victim, full), utilization");
      mvwprintw(win, content_row++, 5,
                "- ZNSSD: zones (total, open, full), utilization");
    } else {
      mvwprintw(win, content_row++, 5,
                "- BBSSD: aggregate line counts and average utilization");
      mvwprintw(win, content_row++, 5,
                "- ZNSSD: aggregate zone counts and average utilization");
    }
  } else {
    // Info depends on current device type
    device_context_t *device = &manager->devices[manager->current_device];

    switch (device->device_type) {
      case DEVICE_BBSSD:
        mvwprintw(win, content_row++, 5,
                  "- BBSSD-specific metrics (see BBSSD logging)");
        break;
      case DEVICE_ZNSSD:
        mvwprintw(win, content_row++, 5,
                  "- ZNSSD-specific metrics (zones and utilization)");
        break;
      default:
        mvwprintw(win, content_row++, 5,
                  "- Device type not supported in multi-mode");
        break;
    }
  }

  content_row++;  // Add some spacing

  // Controls section
  wattron(win, A_BOLD);
  mvwprintw(win, content_row++, 3, "Controls:");
  wattroff(win, A_BOLD);

  // Calculate position for two-column layout
  int left_column = 5;
  int right_column = width / 2 + 1;

  mvwprintw(win, content_row, left_column, "s - Start recording");
  mvwprintw(win, content_row++, right_column, "p - Stop recording");

  mvwprintw(win, content_row, left_column, "+ - Increase logging interval");
  mvwprintw(win, content_row++, right_column, "- - Decrease logging interval");

  mvwprintw(win, content_row++, left_column,
            "a - Toggle All/Current device logging");

  // Only show mode toggle if logging all devices
  if (log_state.include_all_devices) {
    mvwprintw(win, content_row++, left_column,
              "m - Toggle logging mode (Individual/Aggregate)");
  }

  return 0;
}

/**
 * Handle multi-device specific input
 */
static int multi_log_handle_input(int ch, device_manager_t *manager) {
  switch (ch) {
    case 'a':  // Toggle All/Current device logging
    case 'A':
      if (log_state.common.recording_active) {
        debug_log_add("Cannot change device logging mode while recording");
      } else {
        log_state.include_all_devices = !log_state.include_all_devices;
        debug_log_add("Multi-device logging changed to %s",
                      log_state.include_all_devices ? "All devices"
                                                    : "Current device only");
      }
      return 1;

    case 'm':  // Toggle Individual/Aggregate mode
    case 'M':
      if (!log_state.include_all_devices) {
        // Only applicable in all-devices mode
        return 0;
      }

      if (log_state.common.recording_active) {
        debug_log_add("Cannot change logging mode while recording");
      } else {
        log_state.szone_log_mode =
            (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL)
                ? SZONE_LOG_AGGREGATE
                : SZONE_LOG_INDIVIDUAL;
        debug_log_add("Multi-device logging mode changed to %s",
                      (log_state.szone_log_mode == SZONE_LOG_INDIVIDUAL)
                          ? "Individual"
                          : "Aggregate");
      }
      return 1;
  }

  return 0;  // Input not handled
}