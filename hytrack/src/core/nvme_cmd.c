/**
 * nvme_cmd.c
 * NVMe Command Implementation for HYTRACK
 */

#include "../../include/core/nvme_cmd.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../include/common/config.h"
#include "../../include/views/debug_view.h"

// Define NVMe command opcodes if not already defined
#ifndef NVME_ADMIN_CMD_IDENTIFY
#define NVME_ADMIN_CMD_IDENTIFY 0x06
#endif

#ifndef NVME_ADMIN_CMD_HYSSD
#define NVME_ADMIN_CMD_HYSSD 0xD0
#endif

#ifndef NVME_ADMIN_CMD_BBSSD
#define NVME_ADMIN_CMD_BBSSD 0xD1
#endif

#ifndef NVME_ADMIN_CMD_ZNSSD
#define NVME_ADMIN_CMD_ZNSSD 0xD2
#endif

#ifndef DEVICE_TYPE_OPCODE
#define DEVICE_TYPE_OPCODE 0xC0
#endif

// Define device info command if not already defined
#ifndef DEVICE_INFO_OPCODE
#define DEVICE_INFO_OPCODE 0xC1
#endif

/**
 * Common function to execute NVMe admin command
 *
 * @param device_path Path to the NVMe device
 * @param opcode Admin command opcode
 * @param buffer Buffer for data transfer
 * @param size Buffer size
 * @param cdw10 Command dword 10
 * @param cdw11 Command dword 11
 * @param cdw12 Command dword 12
 * @param cdw13 Command dword 13
 * @param cdw14 Command dword 14
 * @param cdw15 Command dword 15
 * @return 0 on success, non-zero on failure
 */
int nvme_admin_cmd(const char *device_path, uint8_t opcode, void *buffer,
                   size_t size, uint32_t cdw10, uint32_t cdw11, uint32_t cdw12,
                   uint32_t cdw13, uint32_t cdw14, uint32_t cdw15) {
  int fd;
  struct nvme_passthru_cmd cmd = {0};
  int ret = -1;

  // Open NVMe device
  fd = open(device_path, O_RDONLY);
  if (fd < 0) {
    debug_log_add("Failed to open device %s: %s", device_path, strerror(errno));
    return -1;
  }

  // Setup NVMe Admin Passthru command
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = opcode;
  cmd.nsid = 1;  // Namespace ID
  cmd.addr = (unsigned long)(uintptr_t)buffer;
  cmd.data_len = size;
  cmd.cdw10 = cdw10;
  cmd.cdw11 = cdw11;
  cmd.cdw12 = cdw12;
  cmd.cdw13 = cdw13;
  cmd.cdw14 = cdw14;
  cmd.cdw15 = cdw15;

  // Execute command
  ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
  close(fd);

  if (ret) {
    debug_log_add("NVMe command execution failed (opcode 0x%02x): %s", opcode,
                  strerror(errno));
    return -1;
  }

  return 0;
}

/**
 * Execute HYSSD monitoring command to get data based on device info
 *
 * @param data Pointer to hyssd_monitor_data_t structure
 * @param device_path Path to the NVMe device
 * @param device_info Pointer to device info structure (if NULL, will be
 * detected)
 * @return 0 on success, non-zero on failure
 */
int nvme_get_hyssd_data(hyssd_monitor_data_t *data, const char *device_path,
                        const nvme_device_info_t *device_info) {
  int ret;
  void *buffer = NULL;
  size_t buffer_size;
  nvme_device_info_t local_device_info;
  int tt_lines;
  uint32_t num_zones;

  // If device_info is NULL, detect device info first
  if (device_info == NULL) {
    ret = nvme_detect_device_info(device_path, &local_device_info);
    if (ret != 0) {
      debug_log_add("Failed to detect device info");
      return -1;
    }
    device_info = &local_device_info;
  }

  // Validate device type
  if (device_info->type != DEVICE_HYSSD) {
    debug_log_add("Invalid device type: expected HYSSD");
    return -1;
  }

  // Get tt_lines and num_zones from device info
  tt_lines = device_info->hyssd.tt_lines;
  num_zones = device_info->hyssd.num_zones;

  // Calculate exact buffer size needed
  size_t header_size =
      sizeof(hyssd_write_pointer_info_t) + sizeof(uint64_t) * 2 + sizeof(int) * 4 + sizeof(uint32_t) + sizeof(uint64_t);
  size_t line_info_size = tt_lines * sizeof(hyssd_line_info_t);
  size_t zone_info_size = num_zones * sizeof(hyssd_zone_info_t);
  buffer_size = header_size + line_info_size + zone_info_size;

  // Allocate buffer with exact size needed
  buffer = malloc(buffer_size);
  if (!buffer) {
    debug_log_add("Failed to allocate buffer");
    return -1;
  }

  // Clear the buffer
  memset(buffer, 0, buffer_size);

  // Execute command to get all data at once
  ret = nvme_admin_cmd(device_path, NVME_ADMIN_CMD_HYSSD, buffer, buffer_size,
                       (buffer_size / 4) - 1, 0, 0, 0, 0, 0);

  if (ret != 0) {
    debug_log_add("Failed to get HYSSD data");
    free(buffer);
    return ret;
  }

  // Parse and fill the data structure
  uint8_t *ptr = (uint8_t *)buffer;

  // 1. Copy write pointer info
  memcpy(&data->wp_info, ptr, sizeof(hyssd_write_pointer_info_t));
  ptr += sizeof(hyssd_write_pointer_info_t);

  data->gc_count = *(uint64_t *)ptr;
  ptr += sizeof(uint64_t);
  data->gc_pgs = *(uint64_t *)ptr;
  ptr += sizeof(uint64_t);

  // 2. Process line management header fields
  if (data->lm_info == NULL) {
    data->lm_info = malloc(sizeof(hyssd_line_mgmt_info_t) +
                           tt_lines * sizeof(hyssd_line_info_t));
    if (!data->lm_info) {
      debug_log_add("Failed to allocate memory for line management info");
      free(buffer);
      return -1;
    }
  } else if (data->lm_info->tt_lines < tt_lines) {
    void *new_lm =
        realloc(data->lm_info, sizeof(hyssd_line_mgmt_info_t) +
                                   tt_lines * sizeof(hyssd_line_info_t));
    if (!new_lm) {
      debug_log_add("Failed to reallocate line management info");
      free(buffer);
      return -1;
    }
    data->lm_info = new_lm;
  }

  // Copy header fields
  data->lm_info->tt_lines = *(int *)ptr;
  ptr += sizeof(int);
  data->lm_info->free_line_cnt = *(int *)ptr;
  ptr += sizeof(int);
  data->lm_info->victim_line_cnt = *(int *)ptr;
  ptr += sizeof(int);
  data->lm_info->full_line_cnt = *(int *)ptr;
  ptr += sizeof(int);

  // 3. Get number of zones
  data->num_zones = *(uint32_t *)ptr;
  ptr += sizeof(uint32_t);

  data->zr_count = *(uint64_t *)ptr;
  ptr += sizeof(uint64_t);

  // 4. Copy line info array
  memcpy(data->lm_info->lines, ptr, tt_lines * sizeof(hyssd_line_info_t));
  ptr += tt_lines * sizeof(hyssd_line_info_t);

  // 5. Process zone info
  if (data->zone_info == NULL || data->allocated_zones < num_zones) {
    void *new_zones =
        realloc(data->zone_info, num_zones * sizeof(hyssd_zone_info_t));
    if (!new_zones) {
      debug_log_add("Failed to allocate memory for zone info");
      free(buffer);
      return -1;
    }
    data->zone_info = new_zones;
    data->allocated_zones = num_zones;
  }

  // Copy zone info array
  memcpy(data->zone_info, ptr, num_zones * sizeof(hyssd_zone_info_t));

  free(buffer);
  return 0;
}

/**
 * Execute BBSSD monitoring command to get data based on device info
 *
 * @param data Pointer to bbssd_data_t structure
 * @param device_path Path to the NVMe device
 * @param device_info Pointer to device info structure (if NULL, will be
 * detected)
 * @return 0 on success, non-zero on failure
 */
int nvme_get_bbssd_data(bbssd_data_t *data, const char *device_path,
                        const nvme_device_info_t *device_info) {
  int ret;
  void *buffer = NULL;
  size_t buffer_size;
  nvme_device_info_t local_device_info;
  int tt_lines;

  // If device_info is NULL, detect device info first
  if (device_info == NULL) {
    ret = nvme_detect_device_info(device_path, &local_device_info);
    if (ret != 0) {
      debug_log_add("Failed to detect device info");
      return -1;
    }
    device_info = &local_device_info;
  }

  // Validate device type
  if (device_info->type != DEVICE_BBSSD) {
    debug_log_add("Invalid device type: expected BBSSD");
    return -1;
  }

  // Get total lines from device info
  tt_lines = device_info->bbssd.tt_lines;

  // Calculate exact buffer size needed
  size_t header_size = sizeof(bbssd_write_pointer_info_t) + sizeof(uint64_t) * 2 + sizeof(int) * 4;
  size_t line_info_size = tt_lines * sizeof(bbssd_line_info_t);
  buffer_size = header_size + line_info_size;

  // Allocate buffer with exact size needed
  buffer = malloc(buffer_size);
  if (!buffer) {
    debug_log_add("Failed to allocate buffer");
    return -1;
  }

  // Clear the buffer
  memset(buffer, 0, buffer_size);

  // Execute command to get all data at once
  ret = nvme_admin_cmd(device_path, NVME_ADMIN_CMD_BBSSD, buffer, buffer_size,
                       (buffer_size / 4) - 1, 0, 0, 0, 0, 0);

  if (ret != 0) {
    debug_log_add("Failed to get BBSSD data");
    free(buffer);
    return ret;
  }

  // Parse and fill the data structure
  uint8_t *ptr = (uint8_t *)buffer;

  // 1. Copy write pointer info
  memcpy(&data->wp_info, ptr, sizeof(bbssd_write_pointer_info_t));
  ptr += sizeof(bbssd_write_pointer_info_t);

  data->gc_count = *(uint64_t *)ptr;
  ptr += sizeof(uint64_t);
  data->gc_pgs = *(uint64_t *)ptr;
  ptr += sizeof(uint64_t);
  
  // 2. Process line management header fields
  if (data->lm_info == NULL) {
    data->lm_info = malloc(sizeof(bbssd_line_mgmt_info_t) +
                           tt_lines * sizeof(bbssd_line_info_t));
    if (!data->lm_info) {
      debug_log_add("Failed to allocate memory for line management info");
      free(buffer);
      return -1;
    }
  } else if (data->lm_info->tt_lines < tt_lines) {
    void *new_lm =
        realloc(data->lm_info, sizeof(bbssd_line_mgmt_info_t) +
                                   tt_lines * sizeof(bbssd_line_info_t));
    if (!new_lm) {
      debug_log_add("Failed to reallocate line management info");
      free(buffer);
      return -1;
    }
    data->lm_info = new_lm;
  }

  // Copy header fields
  data->lm_info->tt_lines = *(int *)ptr;
  ptr += sizeof(int);
  data->lm_info->free_line_cnt = *(int *)ptr;
  ptr += sizeof(int);
  data->lm_info->victim_line_cnt = *(int *)ptr;
  ptr += sizeof(int);
  data->lm_info->full_line_cnt = *(int *)ptr;
  ptr += sizeof(int);

  // 3. Copy line info array
  memcpy(data->lm_info->lines, ptr, tt_lines * sizeof(bbssd_line_info_t));

  free(buffer);
  return 0;
}

/**
 * Execute ZNSSD monitoring command to get data
 *
 * @param data Pointer to znssd_data_t structure
 * @param device_path Path to the NVMe device
 * @param device_info Optional device info structure (can be NULL)
 * @return 0 on success, non-zero on failure
 */
int nvme_get_znssd_data(znssd_data_t *data, const char *device_path,
                        const nvme_device_info_t *device_info) {
  int ret;
  void *buffer = NULL;
  size_t buffer_size;
  nvme_device_info_t local_device_info;
  uint64_t num_zones;

  // If device_info is NULL, detect device info first
  if (device_info == NULL) {
    ret = nvme_detect_device_info(device_path, &local_device_info);
    if (ret != 0) {
      debug_log_add("Failed to detect device info");
      return -1;
    }
    device_info = &local_device_info;
  }

  // Validate device type
  if (device_info->type != DEVICE_ZNSSD) {
    debug_log_add("Invalid device type: expected ZNSSD");
    return -1;
  }

  // Get number of zones from device info
  num_zones = device_info->znssd.nr_zones;

  // Calculate exact buffer size needed
  size_t header_size = sizeof(uint64_t) * 2;  // For num_zones, zr_count
  size_t zone_info_size = num_zones * sizeof(znssd_zone_info_t);
  buffer_size = header_size + zone_info_size;

  // Allocate buffer with exact size needed
  buffer = malloc(buffer_size);
  if (!buffer) {
    debug_log_add("Failed to allocate buffer");
    return -1;
  }

  // Clear the buffer
  memset(buffer, 0, buffer_size);

  // Execute command to get all data at once
  ret = nvme_admin_cmd(device_path, NVME_ADMIN_CMD_ZNSSD, buffer, buffer_size,
                       (buffer_size / 4) - 1, 0, 0, 0, 0, 0);

  if (ret != 0) {
    debug_log_add("Failed to get ZNSSD data");
    free(buffer);
    return -1;
  }

  // Parse and fill the data structure
  uint8_t *ptr = (uint8_t *)buffer;

  // Get number of zones
  data->num_zones = *(uint64_t *)ptr;
  ptr += sizeof(uint64_t);
  data->zr_count = *(uint64_t *)ptr;
  ptr += sizeof(uint64_t);

  // Free old data if exists
  if (data->znssd_data == NULL) {
    // Allocate memory for zone info
    data->znssd_data = malloc(data->num_zones * sizeof(znssd_zone_info_t));

    if (!data->znssd_data) {
      debug_log_add("Failed to allocate memory for ZNSSD zone info");
      free(buffer);
      return -1;
    }
  }

  // Copy zone info array
  memcpy(data->znssd_data, ptr, data->num_zones * sizeof(znssd_zone_info_t));

  free(buffer);
  return 0;
}

/**
 * Detect device type and populate device info structure
 *
 * @param device_path Path to the NVMe device
 * @param device_info Pointer to device info structure to populate
 * @return 0 on success, non-zero on failure
 */
int nvme_detect_device_info(const char *device_path,
                            nvme_device_info_t *device_info) {
  uint8_t buffer[4096] = {0};
  int ret;

  if (!device_info) {
    debug_log_add("Invalid device_info pointer");
    return -1;
  }

  // Initialize device_info structure
  memset(device_info, 0, sizeof(nvme_device_info_t));

  // First try to use device-specific type detection command
  ret = nvme_admin_cmd(device_path, DEVICE_TYPE_OPCODE, buffer, sizeof(buffer),
                       1023, 0, 0, 0, 0, 0);
  if (ret == 0) {
    // First byte indicates device type
    device_info->type = buffer[0];

    switch (device_info->type) {
      case DEVICE_BBSSD:
        debug_log_add("Detected BBSSD device: %s", device_path);
        break;

      case DEVICE_ZNSSD:
        debug_log_add("Detected ZNSSD device: %s", device_path);
        break;

      case DEVICE_HYSSD:
        debug_log_add("Detected HYSSD device: %s", device_path);
        break;

      default:
        debug_log_add("Unknown device type: %d", buffer[0]);
        device_info->type = DEVICE_UNKNOWN;
        break;
    }
  } else {
    // Try the identify namespace command
    memset(buffer, 0, sizeof(buffer));
    ret = nvme_admin_cmd(device_path, NVME_ADMIN_CMD_IDENTIFY, buffer,
                         sizeof(buffer), 0, 1, 0, 0, 0, 0);
    if (ret != 0) {
      debug_log_add("Failed to identify device: %s", device_path);
      device_info->type = DEVICE_UNKNOWN;
      return -1;
    }

    // Check ZNS bit in identify data
    if (buffer[28] & 0x02) {
      debug_log_add("Detected ZNSSD device from identify data: %s",
                    device_path);
      device_info->type = DEVICE_ZNSSD;
    } else {
      // Default to BBSSD if no specific type detected
      debug_log_add("Assuming BBSSD device: %s", device_path);
      device_info->type = DEVICE_BBSSD;
    }
  }

  // Now get detailed device information based on detected type
  // memset(buffer, 0, sizeof(buffer));
  // ret = nvme_admin_cmd(device_path, DEVICE_INFO_OPCODE, buffer,
  // sizeof(buffer),
  //                      device_info->type, 0, 0, 0, 0, 0);

  if (ret == 0) {
    // Parse the buffer and populate device_info based on device type
    switch (device_info->type) {
      case DEVICE_HYSSD: {
        // Copy HYSSD specific information
        memcpy(&device_info->hyssd, buffer + sizeof(device_type_t),
               sizeof(device_info->hyssd));
        debug_log_add("HYSSD device info populated for %s", device_path);
        break;
      }

      case DEVICE_BBSSD: {
        // Copy BBSSD specific information
        memcpy(&device_info->bbssd, buffer + sizeof(device_type_t),
               sizeof(device_info->bbssd));
        debug_log_add("BBSSD device info populated for %s", device_path);
        break;
      }

      case DEVICE_ZNSSD: {
        // Copy ZNSSD specific information
        memcpy(&device_info->znssd, buffer + sizeof(device_type_t),
               sizeof(device_info->znssd));
        debug_log_add("ZNSSD device info populated for %s", device_path);
        break;
      }

      default:
        debug_log_add("Cannot populate info for unknown device type");
        return -1;
    }

    return 0;
  } else {
    debug_log_add("Failed to get device info for %s", device_path);
    return -1;
  }
}

/**
 * Detect device type using NVMe identify command
 *
 * @param device_path Path to the NVMe device
 * @return Device type or DEVICE_UNKNOWN on error
 */
device_type_t nvme_detect_device_type(const char *device_path) {
  nvme_device_info_t device_info;

  // Use the new function to get device info
  if (nvme_detect_device_info(device_path, &device_info) == 0) {
    return device_info.type;
  }

  return DEVICE_UNKNOWN;
}