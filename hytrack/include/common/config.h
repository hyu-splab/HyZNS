/**
 * config.h
 * Common Configuration and Constants for HYTRACK
 */

#ifndef CONFIG_H
#define CONFIG_H

// Configuration values
#define DEFAULT_DEVICE "/dev/nvme0n1"
#define BUFFER_SIZE 8192
#define DEFAULT_INTERVAL 1.0
#define HISTORY_SIZE 60      // Increased history for better trending
#define MAX_DEVICES 4        // Maximum number of supported devices
#define MAX_DEVICE_PATH 256  // Maximum device path length

// NVMe Admin command opcodes
#define NVME_ADMIN_CMD_HYSSD 0xC0     // Custom opcode for HYSSD
#define NVME_ADMIN_CMD_BBSSD 0xC1     // Custom opcode for BBSSD
#define NVME_ADMIN_CMD_ZNSSD 0xC2     // Custom opcode for ZNSSD
#define NVME_ADMIN_CMD_IDENTIFY 0x06  // Identify command

// Device type detection
#define DEVICE_TYPE_OPCODE 0xD0  // Custom opcode for device type query

// Timeout values
#define NVME_CMD_TIMEOUT 5000  // Timeout in milliseconds

#endif /* CONFIG_H */