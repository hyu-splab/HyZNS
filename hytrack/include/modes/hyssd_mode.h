/**
 * hyssd_monitor.h
 * HYSSD Monitor Data Structures
 */

#ifndef HYSSD_MONITOR_H
#define HYSSD_MONITOR_H

#include <ncurses.h>
#include <stddef.h> /* For NULL */
#include <stdint.h>
#include <stdlib.h> /* For free */
#include <string.h> /* For memset */

#include "../core/data_types.h"

/* Zone descriptor info for host-side application */
typedef struct {
  uint8_t zt;     /* Zone Type */
  uint8_t zs;     /* Zone State */
  uint8_t za;     /* Zone Attributes */
  uint64_t zcap;  /* Zone Capacity */
  uint64_t zslba; /* Zone Start LBA */
  uint64_t wp;    /* Write Pointer */
} hyssd_zone_descr_info_t;

/* Zone info structure for host-side application */
typedef struct {
  hyssd_zone_descr_info_t d;
  uint64_t w_ptr; /* Write pointer */
  uint8_t dirty;  /* Written or not */
  uint8_t rnd;    /* Random zone or seq zone */
} hyssd_zone_info_t;

/* Write pointer info for host-side application */
typedef struct {
  int curline_id; /* Current write line ID */
  int ch;         /* Channel */
  int lun;        /* LUN */
  int pg;         /* Page */
  int blk;        /* Block */
  int pl;         /* Plane */
} hyssd_write_pointer_info_t;

/* Line info structure for host-side application */
typedef struct {
  int id;                 /* Line ID (same as block ID) */
  int ipc;                /* Invalid page count in this line */
  int vpc;                /* Valid page count in this line */
  unsigned long long pos; /* Position in victim line priority queue */
  int is_rnd;             /* Is random zone (1) or sequential zone (0) */
  int ridx;               /* If random zone line, which index */
} hyssd_line_info_t;

/* Line management info structure for host-side application */
typedef struct {
  int tt_lines;        /* Total number of lines */
  int free_line_cnt;   /* Number of free lines */
  int victim_line_cnt; /* Number of victim lines */
  int full_line_cnt;   /* Number of full lines */
  /* Line info array follows as flexible array member */
  hyssd_line_info_t lines[]; /* Flexible array member */
} hyssd_line_mgmt_info_t;

/*
 * Main data structure for HYSSD monitoring
 * This structure is used by both client application and HYSSD mode
 */
typedef struct {
  /* Write pointer information */
  hyssd_write_pointer_info_t wp_info;

  uint64_t gc_count;      /* Number of GC operations performed */
  uint64_t gc_pgs;        /* # of pages moved in GC */
  
  /* Line management information */
  hyssd_line_mgmt_info_t *lm_info;

  /* Zone information */
  uint32_t num_zones;           /* Number of actual zones */
  uint64_t zr_count;             /* Zone Reset count */
  uint32_t allocated_zones;     /* Number of zones we've allocated space for */
  hyssd_zone_info_t *zone_info; /* Array of zone information */
} hyssd_monitor_data_t;

/* Use the same structure for the HYSSD mode data */
typedef hyssd_monitor_data_t hyssd_data_t;

/* Initialize HYSSD monitor data structure */
static inline void hyssd_monitor_data_init(hyssd_monitor_data_t *data) {
  if (data) {
    memset(data, 0, sizeof(hyssd_monitor_data_t));
    data->lm_info = NULL;
    data->zone_info = NULL;
    data->num_zones = 0;
    data->zr_count = 0;
    data->allocated_zones = 0;
  }
}

/* Free resources allocated by HYSSD monitor data structure */
static inline void hyssd_monitor_data_free(hyssd_monitor_data_t *data) {
  if (data) {
    if (data->lm_info) {
      free(data->lm_info);
      data->lm_info = NULL;
    }

    if (data->zone_info) {
      free(data->zone_info);
      data->zone_info = NULL;
    }

    data->num_zones = 0;
    data->zr_count = 0;
    data->allocated_zones = 0;
  }
}

/* Get HYSSD data from the device */
int nvme_get_hyssd_data(hyssd_monitor_data_t *data, const char *device_path,
                        const nvme_device_info_t *device_info);
/**
 * Initialize HYSSD mode for a device
 *
 * @param device Device context to initialize
 * @return 0 on success, non-zero on failure
 */
int hyssd_mode_init(device_context_t *device);

/**
 * Clean up HYSSD mode resources
 *
 * @param device Device context to clean up
 */
void hyssd_mode_cleanup(device_context_t *device);

/**
 * Get HYSSD data from device
 *
 * @param device Device context
 * @return 0 on success, non-zero on failure
 */
int hyssd_mode_get_data(device_context_t *device);

/**
 * Update HYSSD history data
 *
 * @param device Device context
 */
void hyssd_mode_update_history(device_context_t *device);

/**
 * Render HYSSD-specific UI elements
 *
 * @param win Window to render to
 * @param device Device context
 * @param row Starting row
 * @param col Starting column
 * @param width Available width
 * @param height Available height
 * @return Next row after rendered elements
 */
int hyssd_mode_render(WINDOW *win, device_context_t *device, int row, int col,
                      int width, int height);

#endif /* HYSSD_MONITOR_H */