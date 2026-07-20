/**
 * device_manager.c
 * Multi-device Management Implementation
 */

#include "../../include/core/device_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../include/common/history.h"
#include "../../include/core/nvme_cmd.h"
#include "../../include/modes/bbssd_mode.h"
#include "../../include/modes/hyssd_mode.h"
#include "../../include/modes/multi_mode.h"
#include "../../include/modes/znssd_mode.h"
#include "../../include/views/debug_view.h"

/**
 * Initialize the device manager
 *
 * @param options Command line options
 * @return Pointer to initialized device manager or NULL on failure
 */
device_manager_t *device_manager_init(hytrack_options_t *options) {
  device_manager_t *manager;
  int i;

  // Allocate device manager
  manager = (device_manager_t *)malloc(sizeof(device_manager_t));
  if (!manager) {
    return NULL;
  }

  // Initialize manager fields
  memset(manager, 0, sizeof(device_manager_t));
  manager->device_count = options->device_count;
  manager->current_device = options->current_device;
  manager->mode = options->mode;

  // Store reference to options for views to access
  manager->options = options;

  // Initialize view state
  manager->view_state.auto_scale = 1;   // Auto-scale graphs by default
  manager->view_state.show_grid = 1;    // Show grid by default
  manager->view_state.sort_mode = 0;    // Default sorting mode
  manager->view_state.filter_mode = 0;  // Default filter mode

  // Initialize devices
  for (i = 0; i < manager->device_count; i++) {
    device_context_t *device = &manager->devices[i];

    // Set device path
    strncpy(device->device_path, options->devices[i], MAX_DEVICE_PATH - 1);
    device->device_path[MAX_DEVICE_PATH - 1] = '\0';

    // Detect device type and get device info
    device->device_info =
        (nvme_device_info_t *)malloc(sizeof(nvme_device_info_t));
    if (!device->device_info) {
      fprintf(stderr, "Failed to allocate device info for device %d\n", i);
      device_manager_cleanup(manager);
      return NULL;
    }

    if (nvme_detect_device_info(device->device_path, device->device_info) !=
        0) {
      fprintf(stderr, "Failed to detect device info for device %d\n", i);
      device_manager_cleanup(manager);
      return NULL;
    }

    // Set device type from device info
    device->device_type = device->device_info->type;

    // Initialize history data
    device->histories = (history_set_t *)malloc(sizeof(history_set_t));
    if (!device->histories) {
      fprintf(stderr, "Failed to allocate history data for device %d\n", i);
      device_manager_cleanup(manager);
      return NULL;
    }
    history_set_init(device->histories);

    // Initialize device-specific data
    switch (manager->mode) {
      case MODE_HYSSD:
        if (hyssd_mode_init(device) != 0) {
          fprintf(stderr, "Failed to initialize HYSSD mode for device %d\n", i);
          device_manager_cleanup(manager);
          return NULL;
        }
        debug_log_add("Initialized HYSSD mode for device %s",
                      device->device_path);
        break;

      case MODE_BBSSD:
        if (bbssd_mode_init(device) != 0) {
          fprintf(stderr, "Failed to initialize BBSSD mode for device %d\n", i);
          device_manager_cleanup(manager);
          return NULL;
        }
        debug_log_add("Initialized BBSSD mode for device %s",
                      device->device_path);
        break;

      case MODE_MULTI:
        // Multi-mode initialization
        if (i == 0) {
          // First device is BBSSD
          if (bbssd_mode_init(device) != 0) {
            fprintf(stderr, "Failed to initialize BBSSD mode for device %d\n",
                    i);
            device_manager_cleanup(manager);
            return NULL;
          }
          debug_log_add("Initialized BBSSD mode for device %s",
                        device->device_path);
        } else {
          // Second device is ZNSSD, initialized via multi_mode_init
          // This is handled by multi_mode_init below
        }
        break;
    }
  }

  // Special handling for multi-device mode
  if (manager->mode == MODE_MULTI) {
    if (multi_mode_init(manager) != 0) {
      fprintf(stderr, "Failed to initialize multi-device mode\n");
      device_manager_cleanup(manager);
      return NULL;
    }
    debug_log_add("Initialized multi-device mode");
  }

  return manager;
}

/**
 * Clean up the device manager
 *
 * @param manager Device manager to clean up
 */
void device_manager_cleanup(device_manager_t *manager) {
  int i;

  if (!manager) {
    return;
  }

  // Cleanup each device
  for (i = 0; i < manager->device_count; i++) {
    device_context_t *device = &manager->devices[i];

    // Cleanup device-specific data
    switch (manager->mode) {
      case MODE_HYSSD:
        hyssd_mode_cleanup(device);
        break;

      case MODE_BBSSD:
        bbssd_mode_cleanup(device);
        break;

      case MODE_MULTI:
        // Cleanup handled by multi_mode_cleanup below
        break;
    }

    // Free device info
    if (device->device_info) {
      free(device->device_info);
      device->device_info = NULL;
    }

    // Free history data
    if (device->histories) {
      free(device->histories);
      device->histories = NULL;
    }

    // Free device data
    if (device->device_data) {
      free(device->device_data);
      device->device_data = NULL;
    }
  }

  // Special cleanup for multi-device mode
  if (manager->mode == MODE_MULTI) {
    multi_mode_cleanup(manager);
  }

  // Free the manager
  free(manager);
}

/**
 * Update data for all devices
 *
 * @param manager Device manager
 * @return 0 on success, non-zero if any device failed
 */
int device_manager_update_data(device_manager_t *manager) {
  int i;
  int status = 0;

  for (i = 0; i < manager->device_count; i++) {
    device_context_t *device = &manager->devices[i];
    int result = 0;

    // Get device data
    switch (manager->mode) {
      case MODE_HYSSD:
        result = hyssd_mode_get_data(device);
        if (result == 0) {
          hyssd_mode_update_history(device);
        }
        break;

      case MODE_BBSSD:
        result = bbssd_mode_get_data(device);
        if (result == 0) {
          bbssd_mode_update_history(device);
        }
        break;

      case MODE_MULTI:
        if (i == 0) {
          // BBSSD device
          result = bbssd_mode_get_data(device);
          if (result == 0) {
            bbssd_mode_update_history(device);
          }
        } else {
          // ZNSSD device
          result = znssd_mode_get_data(device);
          if (result == 0) {
            znssd_mode_update_history(device);
          }
        }
        break;
    }

    // Update device status
    device->status = result;
    device->update_count++;

    // Update global status
    if (result != 0) {
      status = result;
    }
  }

  // Update view state
  manager->view_state.update_count++;

  return status;
}

/**
 * Switch to a different device
 *
 * @param manager Device manager
 * @param device_index Index of the device to switch to
 * @return 0 on success, non-zero on failure
 */
int device_manager_switch_device(device_manager_t *manager, int device_index) {
  if (!manager || device_index < 0 || device_index >= manager->device_count) {
    return -1;
  }

  manager->current_device = device_index;
  debug_log_add("Switched to device %d: %s", device_index,
                manager->devices[device_index].device_path);

  return 0;
}

/**
 * Get device-specific info
 *
 * @param manager Device manager
 * @param device_index Device index (-1 for current device)
 * @return Pointer to device info or NULL on error
 */
nvme_device_info_t *device_manager_get_device_info(device_manager_t *manager,
                                                   int device_index) {
  if (!manager) {
    return NULL;
  }

  if (device_index < 0) {
    device_index = manager->current_device;
  }

  if (device_index >= manager->device_count) {
    return NULL;
  }

  return manager->devices[device_index].device_info;
}

/**
 * Get device-specific data pointer
 *
 * @param manager Device manager
 * @param device_index Device index (-1 for current device)
 * @return Pointer to device data or NULL on error
 */
void *device_manager_get_data(device_manager_t *manager, int device_index) {
  if (!manager) {
    return NULL;
  }

  if (device_index < 0) {
    device_index = manager->current_device;
  }

  if (device_index >= manager->device_count) {
    return NULL;
  }

  return manager->devices[device_index].device_data;
}

/**
 * Get current device context
 *
 * @param manager Device manager
 * @return Pointer to current device context or NULL
 */
device_context_t *device_manager_get_current(device_manager_t *manager) {
  if (!manager || manager->current_device >= manager->device_count) {
    return NULL;
  }

  return &manager->devices[manager->current_device];
}