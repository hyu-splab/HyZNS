/**
 * nvme_cmd.h
 * NVMe Command Definitions and Declarations
 */

#ifndef NVME_CMD_H
#define NVME_CMD_H

#include <stdint.h>
#include <stdlib.h>

#include "data_types.h"

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
                   uint32_t cdw13, uint32_t cdw14, uint32_t cdw15);

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
                        const nvme_device_info_t *device_info);

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
                        const nvme_device_info_t *device_info);

/**
 * Execute ZNSSD monitoring command to get data
 *
 * @param data Pointer to znssd_data_t structure
 * @param device_path Path to the NVMe device
 * @param device_info Optional device info structure (can be NULL)
 * @return 0 on success, non-zero on failure
 */
int nvme_get_znssd_data(znssd_data_t *data, const char *device_path,
                        const nvme_device_info_t *device_info);

/**
 * Detect device type and populate device info structure
 *
 * @param device_path Path to the NVMe device
 * @param device_info Pointer to device info structure to populate
 * @return 0 on success, non-zero on failure
 */
int nvme_detect_device_info(const char *device_path,
                            nvme_device_info_t *device_info);

/**
 * Detect device type using NVMe identify command
 *
 * @param device_path Path to the NVMe device
 * @return Device type or DEVICE_UNKNOWN on error
 */
device_type_t nvme_detect_device_type(const char *device_path);

#endif /* NVME_CMD_H */