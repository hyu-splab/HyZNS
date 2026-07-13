/**
 * global_state.c
 * Global State Implementations for HYTRACK
 */

#include "../../include/common/global_state.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../include/common/log_utils.h"
#include "../../include/views/debug_view.h"

// Global variable definitions
log_view_state_t *g_log_state = NULL;
char g_custom_log_filename[256] = {0};

void read_custom_filename(void) {
  FILE *file = fopen(FILENAME_REQUEST_FILE, "r");
  if (file) {
    // Clear previous value
    g_custom_log_filename[0] = '\0';

    // Read filename from file
    if (fgets(g_custom_log_filename, sizeof(g_custom_log_filename), file)) {
      // Remove trailing newline if present
      char *newline = strchr(g_custom_log_filename, '\n');
      if (newline) *newline = '\0';

      debug_log_add("Read custom filename: %s", g_custom_log_filename);
    }

    fclose(file);

    // Remove the request file
    unlink(FILENAME_REQUEST_FILE);
  }
}

/**
 * Stop logging if it's currently active
 * Called when SIGUSR1 signal is received
 */
void stop_logging_if_active(void) {
  // Check if logging state is initialized and recording is active
  if (g_log_state != NULL && g_log_state->common.recording_active) {
    // Stop the recording
    log_utils_stop_recording(&g_log_state->common);

    // Log the event
    debug_log_add("Logging stopped by external signal (SIGUSR1)");
  }
}

/**
 * Start logging if it's not already active
 * Called when SIGUSR2 signal is received
 */
void start_logging_if_inactive(void) {
  // Check if logging state is initialized but not active
  if (g_log_state != NULL && g_device_manager != NULL &&
      !g_log_state->common.recording_active) {
    // Try to read custom filename
    read_custom_filename();

    // Check if we have a custom filename
    if (g_custom_log_filename[0] != '\0') {
      debug_log_add("Using custom filename: %s", g_custom_log_filename);

      // Copy the custom filename to the log state
      strncpy(g_log_state->common.custom_filename, g_custom_log_filename,
              sizeof(g_log_state->common.custom_filename) - 1);
      g_log_state->common
          .custom_filename[sizeof(g_log_state->common.custom_filename) - 1] =
          '\0';

      // Clear the global variable for next use
      g_custom_log_filename[0] = '\0';
    }

    debug_log_add("Starting logging via external signal (SIGUSR2)");

    // Start the recording based on current device manager mode
    switch (g_device_manager->mode) {
      case MODE_HYSSD:
        hyssd_log_start_recording(g_device_manager);
        break;
      case MODE_BBSSD:
        bbssd_log_start_recording(g_device_manager);
        break;
      case MODE_MULTI:
        multi_log_start_recording(g_device_manager);
        break;
      default:
        debug_log_add("Logging not supported in current mode");
        break;
    }
  }
}