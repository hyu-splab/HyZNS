/**
 * view_manager.h
 * View Manager Definition
 */

#ifndef VIEW_MANAGER_H
#define VIEW_MANAGER_H

#include <ncurses.h>

#include "../core/device_manager.h"
#include "view_interface.h"

/**
 * Initialize the view manager
 *
 * @param starting_view Index of the starting view
 * @return 0 on success, non-zero on failure
 */
int view_manager_init(int starting_view);

/**
 * Switch to a different view
 *
 * @param view_index Index of the view to switch to
 * @param manager Device manager to pass to the activate function
 * @return 0 on success, non-zero on failure
 */
int view_manager_switch(int view_index, device_manager_t *manager);

/**
 * Get the number of available views
 *
 * @return Number of views
 */
int view_manager_count(void);

/**
 * Get view by index
 *
 * @param index View index
 * @return Pointer to the view or NULL if not found
 */
const view_interface_t *view_manager_get(int index);

/**
 * Get current active view
 *
 * @return Pointer to the active view
 */
const view_interface_t *view_manager_get_active(void);

/**
 * Get current active view index
 *
 * @return Index of the active view
 */
int view_manager_get_active_index(void);

/**
 * Handle input and route to the appropriate view
 *
 * @param ch Input character
 * @param manager Device manager
 * @return 1 if handled, 0 if not
 */
int view_manager_handle_input(int ch, device_manager_t *manager);

/**
 * Render the current view
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param width Window width
 * @param height Window height
 * @return 0 on success, non-zero on failure
 */
int view_manager_render(WINDOW *win, device_manager_t *manager, int width,
                        int height);

/**
 * Draw the common header with view selection menu
 *
 * @param win Window to draw on
 * @param manager Device manager
 * @param width Window width
 */
void view_manager_draw_header(WINDOW *win, device_manager_t *manager,
                              int width);

/**
 * Cleanup and free resources used by the view manager
 */
void view_manager_cleanup(void);

/**
 * Register all available views
 * This function registers views based on the current operating mode
 *
 * @param mode Operating mode
 */
void view_manager_register_views(hytrack_mode_t mode);

#endif /* VIEW_MANAGER_H */