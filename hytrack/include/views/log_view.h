/**
 * log_view.h
 * Unified Log View Interface for all HYTRACK modes
 */

#ifndef LOG_VIEW_H
#define LOG_VIEW_H

#include "../common/log_utils.h"
#include "view_interface.h"

// Logging modes for sequential zones (HYSSD specific)
typedef enum {
  SZONE_LOG_INDIVIDUAL,  // Log each zone's utilization separately
  SZONE_LOG_AGGREGATE    // Log total utilization across all zones
} szone_log_mode_t;

// Per-mode logging state
typedef struct {
  // Common logging state
  log_state_t common;

  // HYSSD specific
  szone_log_mode_t szone_log_mode;

  // Multi-device specific
  int include_all_devices;  // Flag to log all devices in multi-device mode
} log_view_state_t;

// Exported view interface
extern view_interface_t log_view;

// Exported logging functions for external control
int hyssd_log_start_recording(device_manager_t *manager);
int bbssd_log_start_recording(device_manager_t *manager);
int multi_log_start_recording(device_manager_t *manager);

#endif /* LOG_VIEW_H */