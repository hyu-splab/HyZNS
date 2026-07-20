/**
 * global_state.h
 * Global State and Signal Handlers for HYTRACK
 */

#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include "../core/data_types.h"
#include "../core/device_manager.h"
#include "../views/log_view.h"

#define FILENAME_REQUEST_FILE "/tmp/hytrack_filename_request"

// Global variable declarations
extern device_manager_t *g_device_manager;
extern log_view_state_t *g_log_state;
extern char g_custom_log_filename[256];

// Logging control functions
void stop_logging_if_active(void);
void start_logging_if_inactive(void);
void read_custom_filename(void);

#endif /* GLOBAL_STATE_H */