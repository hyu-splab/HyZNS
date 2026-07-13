/**
 * log_utils.c
 * Common Logging Utilities for HYTRACK
 */

#include "../include/common/log_utils.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../include/views/debug_view.h"

/**
 * Initialize logging state with default values
 */
int log_utils_init_state(log_state_t *state) {
  if (!state) {
    return -1;
  }

  memset(state, 0, sizeof(log_state_t));
  state->initialized = 1;
  state->recording_active = 0;
  state->record_count = 0;
  state->update_interval = 1;
  state->update_counter = 0;
  strcpy(state->log_directory, "logs");
  state->log_file_path[0] = '\0';
  state->custom_filename[0] = '\0';
  return 0;
}

/**
 * Create the log directory if it doesn't exist
 */
int log_utils_create_directory(const char *log_directory) {
  struct stat st = {0};

  if (stat(log_directory, &st) == -1) {
    if (mkdir(log_directory, 0755) == -1) {
      debug_log_add("Error creating log directory: %s", strerror(errno));
      return -1;
    } else {
      debug_log_add("Created log directory: %s", log_directory);
    }
  }
  return 0;
}

/**
 * Get current time with high precision
 */
void log_utils_get_high_precision_time(struct timespec *ts) {
  clock_gettime(CLOCK_REALTIME, ts);
}

/**
 * Calculate elapsed time in seconds with millisecond precision
 */
double log_utils_calculate_elapsed_time(struct timespec start,
                                        struct timespec end) {
  return (end.tv_sec - start.tv_sec) +
         ((double)(end.tv_nsec - start.tv_nsec) / 1000000000.0);
}

/**
 * Generate a human-readable timestamp for the current time
 */
char *log_utils_format_timestamp(struct timespec current_time, char *buffer,
                                 size_t buffer_size, int include_ms) {
  time_t current_time_sec = current_time.tv_sec;
  struct tm *tm_info = localtime(&current_time_sec);

  if (include_ms) {
    snprintf(
        buffer, buffer_size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
        tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
        tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
        current_time.tv_nsec / 1000000);  // Convert nanoseconds to milliseconds
  } else {
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", tm_info);
  }

  return buffer;
}

/**
 * Generate a filename timestamp
 */
char *log_utils_get_filename_timestamp(char *buffer, size_t buffer_size) {
  time_t now;
  struct tm *local_time;

  time(&now);
  local_time = localtime(&now);
  strftime(buffer, buffer_size, "%Y%m%d_%H%M%S", local_time);

  return buffer;
}

/**
 * Extract device name from device path
 */
char *log_utils_extract_device_name(const char *device_path, char *buffer,
                                    size_t buffer_size) {
  const char *last_slash = strrchr(device_path, '/');

  if (last_slash) {
    snprintf(buffer, buffer_size, "%s", last_slash + 1);
  } else {
    snprintf(buffer, buffer_size, "%s", device_path);
  }

  return buffer;
}

/**
 * Start recording log data to file (generic implementation)
 */
int log_utils_start_recording(log_state_t *state, device_context_t *device,
                              const char *header, const char *mode_name) {
  char timestamp[64];
  char device_name[64];
  char filename[256];
  char full_path[512];

  // Check if already recording
  if (state->recording_active) {
    return 0;  // Already recording
  }

  // Create log directory if it doesn't exist
  if (log_utils_create_directory(state->log_directory) != 0) {
    return -1;
  }

  // Generate filename - either custom or auto-generated
  if (state->custom_filename[0] != '\0') {
    // Use custom filename
    strncpy(filename, state->custom_filename, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';

    // Clear custom filename for next use
    state->custom_filename[0] = '\0';

    debug_log_add("Using custom filename: %s", filename);
  } else {
    // Auto-generate filename with timestamp and mode
    log_utils_get_filename_timestamp(timestamp, sizeof(timestamp));

    if (device) {
      log_utils_extract_device_name(device->device_path, device_name,
                                    sizeof(device_name));
      snprintf(filename, sizeof(filename), "%s_%s_%s.csv", timestamp,
               device_name, mode_name);
    } else {
      snprintf(filename, sizeof(filename), "%s_%s.csv", timestamp, mode_name);
    }

    debug_log_add("Using auto-generated filename: %s", filename);
  }

  // Build full path
  snprintf(full_path, sizeof(full_path), "%s/%s", state->log_directory,
           filename);

  // Create the log file
  state->log_file = fopen(full_path, "w");
  if (!state->log_file) {
    debug_log_add("Error creating log file: %s (%s)", full_path,
                  strerror(errno));
    return -1;
  }

  debug_log_add("Created log file: %s", full_path);

  // Store the file path
  strncpy(state->log_file_path, full_path, sizeof(state->log_file_path) - 1);
  state->log_file_path[sizeof(state->log_file_path) - 1] = '\0';

  // Write the CSV header
  fprintf(state->log_file, "%s\n", header);

  // Initialize recording state
  log_utils_get_high_precision_time(&state->start_time);
  state->recording_active = 1;
  state->record_count = 0;
  state->update_counter = 0;

  return 0;
}

/**
 * Stop recording log data
 */
int log_utils_stop_recording(log_state_t *state) {
  if (!state->recording_active) {
    debug_log_add("No active log recording to stop");
    return 0;
  }

  if (state->log_file) {
    fclose(state->log_file);
    state->log_file = NULL;
  }

  state->recording_active = 0;
  debug_log_add("Stopped logging after %u records to %s", state->record_count,
                state->log_file_path);
  return 0;
}