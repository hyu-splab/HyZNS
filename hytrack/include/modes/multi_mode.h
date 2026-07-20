/**
 * multi_mode.h
 * Multi-device Mode (BBSSD+ZNSSD) Definitions and Functions
 */

#ifndef MULTI_MODE_H
#define MULTI_MODE_H

#include <ncurses.h>

#include "../core/data_types.h"

/**
 * Initialize multi-device mode
 *
 * @param manager Device manager to initialize
 * @return 0 on success, non-zero on failure
 */
int multi_mode_init(device_manager_t *manager);

/**
 * Clean up multi-device mode resources
 *
 * @param manager Device manager to clean up
 */
void multi_mode_cleanup(device_manager_t *manager);

/**
 * Render multi-device overview
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param row Starting row
 * @param col Starting column
 * @param width Available width
 * @param height Available height
 * @return Next row after rendered elements
 */
int multi_mode_render_overview(WINDOW *win, device_manager_t *manager, int row,
                               int col, int width, int height);

#endif /* MULTI_MODE_H */