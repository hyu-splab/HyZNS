/**
 * log_utils.h
 * Common Logging Utilities for HYTRACK
 */

#ifndef LOG_UTILS_H
#define LOG_UTILS_H

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "../core/data_types.h"

// Common logging state structure
typedef struct {
  int initialized;             // Flag indicating if logging is initialized
  int recording_active;        // Flag indicating if recording is active
  char log_file_path[256];     // Path to the current log file
  FILE *log_file;              // File handle for the current log file
  struct timespec start_time;  // Start time of recording
  unsigned int record_count;   // Number of records written
  int update_interval;         // Interval for recording (in updates)
  int update_counter;          // Counter for update interval
  char log_directory[256];     // Directory for log files
  char custom_filename[256];   // Custom filename provided via signal
} log_state_t;

/**
 * Initialize logging state with default values
 *
 * @param state Pointer to logging state structure
 * @return 0 on success, non-zero on failure
 */
int log_utils_init_state(log_state_t *state);

/**
 * Create the log directory if it doesn't exist
 *
 * @param log_directory Directory path for log files
 * @return 0 on success, non-zero on failure
 */
int log_utils_create_directory(const char *log_directory);

/**
 * Get current time with high precision
 *
 * @param ts Pointer to timespec structure to store the time
 */
void log_utils_get_high_precision_time(struct timespec *ts);

/**
 * Calculate elapsed time in seconds with millisecond precision
 *
 * @param start Start time
 * @param end End time
 * @return Elapsed time in seconds with millisecond precision
 */
double log_utils_calculate_elapsed_time(struct timespec start,
                                        struct timespec end);

/**
 * Generate a human-readable timestamp for the current time
 *
 * @param current_time Current time as timespec
 * @param buffer Buffer to store the timestamp
 * @param buffer_size Size of the buffer
 * @param include_ms Flag indicating whether to include milliseconds
 * @return Pointer to the buffer
 */
char *log_utils_format_timestamp(struct timespec current_time, char *buffer,
                                 size_t buffer_size, int include_ms);

/**
 * Generate a filename timestamp
 *
 * @param buffer Buffer to store the timestamp
 * @param buffer_size Size of the buffer
 * @return Pointer to the buffer
 */
char *log_utils_get_filename_timestamp(char *buffer, size_t buffer_size);

/**
 * Extract device name from device path
 *
 * @param device_path Full device path
 * @param buffer Buffer to store the device name
 * @param buffer_size Size of the buffer
 * @return Pointer to the buffer
 */
char *log_utils_extract_device_name(const char *device_path, char *buffer,
                                    size_t buffer_size);

/**
 * Start recording log data to file (generic implementation)
 *
 * @param state Pointer to logging state structure
 * @param device Device context
 * @param header CSV header string
 * @param mode_name Mode name for filename
 * @return 0 on success, non-zero on failure
 */
int log_utils_start_recording(log_state_t *state, device_context_t *device,
                              const char *header, const char *mode_name);

/**
 * Stop recording log data
 *
 * @param state Pointer to logging state structure
 * @return 0 on success, non-zero on failure
 */
int log_utils_stop_recording(log_state_t *state);

#endif /* LOG_UTILS_H */