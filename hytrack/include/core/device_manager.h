/**
 * device_manager.h
 * Multi-device Management for HYTRACK
 */

#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include <ncurses.h>

#include "../common/history.h"
#include "data_types.h"

// Forward declarations
typedef struct device_context_s device_context_t;
typedef struct device_manager_s device_manager_t;

// Device context structure
struct device_context_s {
  char device_path[MAX_DEVICE_PATH];  // Device path
  device_type_t device_type;          // Device type (BBSSD, ZNSSD, HYSSD)
  nvme_device_info_t *device_info;    // Device information structure
  void *device_data;                  // Device-specific data
  history_set_t *histories;           // History data for this device
  int data_size;                      // Size of device data
  int update_count;                   // Number of times this device was updated
  int status;                         // 0 for success, non-zero for error
};

// Multi-device manager structure
struct device_manager_s {
  device_context_t devices[MAX_DEVICES];  // Array of device contexts
  int device_count;                       // Number of configured devices
  int current_device;                     // Currently selected device index
  hytrack_mode_t mode;                    // Current operating mode
  view_state_t view_state;                // Global view state
  hytrack_options_t *options;             // Command line options reference
};

/**
 * Initialize the device manager
 *
 * @param options Command line options
 * @return Pointer to initialized device manager or NULL on failure
 */
device_manager_t *device_manager_init(hytrack_options_t *options);

/**
 * Clean up the device manager
 *
 * @param manager Device manager to clean up
 */
void device_manager_cleanup(device_manager_t *manager);

/**
 * Update data for all devices
 *
 * @param manager Device manager
 * @return 0 on success, non-zero if any device failed
 */
int device_manager_update_data(device_manager_t *manager);

/**
 * Switch to a different device
 *
 * @param manager Device manager
 * @param device_index Index of the device to switch to
 * @return 0 on success, non-zero on failure
 */
int device_manager_switch_device(device_manager_t *manager, int device_index);

/**
 * Get device-specific info
 *
 * @param manager Device manager
 * @param device_index Device index (-1 for current device)
 * @return Pointer to device info or NULL on error
 */
nvme_device_info_t *device_manager_get_device_info(device_manager_t *manager,
                                                   int device_index);

/**
 * Get device-specific data pointer
 *
 * @param manager Device manager
 * @param device_index Device index (-1 for current device)
 * @return Pointer to device data or NULL on error
 */
void *device_manager_get_data(device_manager_t *manager, int device_index);

/**
 * Get current device context
 *
 * @param manager Device manager
 * @return Pointer to current device context or NULL
 */
device_context_t *device_manager_get_current(device_manager_t *manager);

#endif /* DEVICE_MANAGER_H */