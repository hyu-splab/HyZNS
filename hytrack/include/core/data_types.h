/**
 * data_types.h
 * HYTRACK Data Type Definitions
 */

#ifndef DATA_TYPES_H
#define DATA_TYPES_H

#include <stdint.h>

#include "../common/config.h"

// Include history types first to avoid circular dependencies
#include "../common/history.h"

// Operating mode definitions
typedef enum {
  MODE_HYSSD = 0,  // HYSSD mode
  MODE_BBSSD = 1,  // BBSSD standalone mode
  MODE_MULTI = 2   // BBSSD+ZNSSD multi-device mode
} hytrack_mode_t;

// Device type definitions
typedef uint64_t device_type_t;

enum {
  DEVICE_BBSSD = 0,
  DEVICE_ZNSSD = 1,
  DEVICE_HYSSD = 2,
  DEVICE_UNKNOWN = -1,
};

// Unified device information structure
typedef struct {
  // Common device information
  device_type_t type;  // Device type
  // Device type specific information
  union {
    // HYSSD specific information
    struct {
      int secsz;        /* sector size in bytes */
      int secs_per_pg;  /* # of sectors per page */
      int pgs_per_blk;  /* # of NAND pages per block */
      int blks_per_pl;  /* # of blocks per plane */
      int pls_per_lun;  /* # of planes per LUN (Die) */
      int luns_per_ch;  /* # of LUNs per channel */
      int nchs;         /* # of channels in the SSD */
      int secs_per_blk; /* # of sectors per block */
      int secs_per_pl;  /* # of sectors per plane */
      int secs_per_lun; /* # of sectors per LUN */
      int secs_per_ch;  /* # of sectors per channel */
      int tt_secs;      /* # of sectors in the SSD */
      int pgs_per_pl;   /* # of pages per plane */
      int pgs_per_lun;  /* # of pages per LUN (Die) */
      int pgs_per_ch;   /* # of pages per channel */
      int tt_pgs;       /* total # of pages in the SSD */
      int blks_per_lun; /* # of blocks per LUN */
      int blks_per_ch;  /* # of blocks per channel */
      int tt_blks;      /* total # of blocks in the SSD */
      int secs_per_line;
      int pgs_per_line;
      int blks_per_line;
      int tt_lines;
      int pls_per_ch; /* # of planes per channel */
      int tt_pls;     /* total # of planes in the SSD */
      int tt_luns;    /* total # of LUNs in the SSD */
      int lbas_per_zone;
      int pgs_per_zone; /* # of pages per zone */
      int tt_rpgs;      /* total # of random zone pages */
      int num_zones;    /* total # of zones */
    } hyssd;

    // BBSSD specific information
    struct {
      int secsz;        /* sector size in bytes */
      int secs_per_pg;  /* # of sectors per page */
      int pgs_per_blk;  /* # of NAND pages per block */
      int blks_per_pl;  /* # of blocks per plane */
      int pls_per_lun;  /* # of planes per LUN (Die) */
      int luns_per_ch;  /* # of LUNs per channel */
      int nchs;         /* # of channels in the SSD */
      int secs_per_blk; /* # of sectors per block */
      int secs_per_pl;  /* # of sectors per plane */
      int secs_per_lun; /* # of sectors per LUN */
      int secs_per_ch;  /* # of sectors per channel */
      int tt_secs;      /* # of sectors in the SSD */
      int pgs_per_pl;   /* # of pages per plane */
      int pgs_per_lun;  /* # of pages per LUN (Die) */
      int pgs_per_ch;   /* # of pages per channel */
      int tt_pgs;       /* total # of pages in the SSD */
      int blks_per_lun; /* # of blocks per LUN */
      int blks_per_ch;  /* # of blocks per channel */
      int tt_blks;      /* total # of blocks in the SSD */
      int secs_per_line;
      int pgs_per_line;
      int blks_per_line;
      int tt_lines;
      int pls_per_ch; /* # of planes per channel */
      int tt_pls;     /* total # of planes in the SSD */
      int tt_luns;    /* total # of LUNs in the SSD */
    } bbssd;

    // ZNSSD specific information
    struct {
      uint64_t num_ch;
      uint64_t num_lun;
      uint64_t nr_zones;
    } znssd;
  };
} nvme_device_info_t;

// Command line options
typedef struct {
  char devices[MAX_DEVICES][MAX_DEVICE_PATH];  // Device path array
  int device_count;                            // Number of configured devices
  int current_device;   // Currently selected device index
  hytrack_mode_t mode;  // Operating mode
  float interval;       // Update interval
  int starting_view;    // Initial view to display
  int hide_cursor;      // Flag to hide cursor (1) or show it (0)
  int debug_mode;       // Flag to enable debug mode
  int auto_start_log;   // Flag to start logging automatically
  int log_mode;         // Logging mode (0: individual, 1: aggregate)
  char log_directory[MAX_DEVICE_PATH];  // Log directory path
} hytrack_options_t;

// Base data structure for global view state
typedef struct {
  // View and display settings
  int update_count;  // Number of updates performed
  int auto_scale;    // Flag for auto-scaling graphs
  int show_grid;     // Flag for showing grid on graphs
  int sort_mode;     // Current sort mode
  int filter_mode;   // Current filter mode

  // Optional mode-specific state data can be added here
} view_state_t;

// Forward declarations for device context and manager
struct device_context_s;
typedef struct device_context_s device_context_t;

struct device_manager_s;
typedef struct device_manager_s device_manager_t;

// Include mode-specific data structures
#include "../modes/bbssd_mode.h"
#include "../modes/hyssd_mode.h"
#include "../modes/multi_mode.h"
#include "../modes/znssd_mode.h"

#endif /* DATA_TYPES_H */