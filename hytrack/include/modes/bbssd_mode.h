/**
 * bbssd_mode.h
 * BBSSD Mode Definitions and Functions
 */

#ifndef BBSSD_MODE_H
#define BBSSD_MODE_H

#include <ncurses.h>

#include "../core/data_types.h"

// BBSSD write pointer info structure - Must match FEMU's structure
typedef struct {
  int curline_id; /* Current write line ID */
  int ch;         /* Channel */
  int lun;        /* LUN */
  int pg;         /* Page */
  int blk;        /* Block */
  int pl;         /* Plane */
} bbssd_write_pointer_info_t;

// BBSSD line info structure - Must match FEMU's structure
typedef struct {
  int id;       /* Line ID (same as block ID) */
  int ipc;      /* Invalid page count in this line */
  int vpc;      /* Valid page count in this line */
  uint64_t pos; /* Position in victim line priority queue */
} bbssd_line_info_t;

// BBSSD line management info structure - Must match FEMU's structure
typedef struct {
  int tt_lines;              /* Total number of lines */
  int free_line_cnt;         /* Number of free lines */
  int victim_line_cnt;       /* Number of victim lines */
  int full_line_cnt;         /* Number of full lines */
  bbssd_line_info_t lines[]; /* Flexible array member */
} bbssd_line_mgmt_info_t;

// BBSSD monitor data structure
typedef struct bbssd_data_s {
  bbssd_write_pointer_info_t wp_info;
  uint64_t gc_count;      /* Number of GC operations performed */
  uint64_t gc_pgs;        /* # of pages moved in GC */
  bbssd_line_mgmt_info_t *lm_info;  // Pointer to allow flexible array member
} bbssd_data_t;

/**
 * Initialize BBSSD mode for a device
 *
 * @param device Device context to initialize
 * @return 0 on success, non-zero on failure
 */
int bbssd_mode_init(device_context_t *device);

/**
 * Clean up BBSSD mode resources
 *
 * @param device Device context to clean up
 */
void bbssd_mode_cleanup(device_context_t *device);

/**
 * Get BBSSD data from device
 *
 * @param device Device context
 * @return 0 on success, non-zero on failure
 */
int bbssd_mode_get_data(device_context_t *device);

/**
 * Update BBSSD history data
 *
 * @param device Device context
 */
void bbssd_mode_update_history(device_context_t *device);

/**
 * Render BBSSD-specific UI elements
 *
 * @param win Window to render to
 * @param device Device context
 * @param row Starting row
 * @param col Starting column
 * @param width Available width
 * @param height Available height
 * @return Next row after rendered elements
 */
int bbssd_mode_render(WINDOW *win, device_context_t *device, int row, int col,
                      int width, int height);

#endif /* BBSSD_MODE_H */