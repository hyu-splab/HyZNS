/**
 * view_interface.h
 * View Interface Definition
 */

#ifndef VIEW_INTERFACE_H
#define VIEW_INTERFACE_H

#include <ncurses.h>

#include "../core/device_manager.h"

// View type enumeration
typedef enum {
  VIEW_DASHBOARD = 1,
  VIEW_STATS,
  VIEW_GRAPH,
  VIEW_LINE,
  VIEW_DEBUG,
  VIEW_MULTI,  // View for multi-device visualization
  VIEW_LOG,    // New view for logging data to file
  VIEW_COUNT   // Always keep this as the last item
} view_type_t;

// View interface structure
typedef struct {
  const char *name;         // View name
  const char *description;  // View description
  const char *key;          // Key to activate this view

  // View function pointers
  int (*init)(void);
  int (*activate)(
      device_manager_t *manager);  // Modified to accept device manager
  int (*deactivate)(void);
  int (*render)(WINDOW *win, device_manager_t *manager, int width, int height);
  int (*handle_input)(int ch, device_manager_t *manager);
  void (*cleanup)(void);
} view_interface_t;

/**
 * Default initialization function for views
 * Can be overridden by specific views
 *
 * @return 0 on success, non-zero on failure
 */
int view_interface_default_init(void);

/**
 * Default activation function for views
 * Can be overridden by specific views
 *
 * @param manager Device manager
 * @return 0 on success, non-zero on failure
 */
int view_interface_default_activate(device_manager_t *manager);

/**
 * Default deactivation function for views
 * Can be overridden by specific views
 *
 * @return 0 on success, non-zero on failure
 */
int view_interface_default_deactivate(void);

/**
 * Default rendering function for views
 * Should be overridden by specific views
 *
 * @param win Window to render to
 * @param manager Device manager
 * @param width Window width
 * @param height Window height
 * @return 0 on success, non-zero on failure
 */
int view_interface_default_render(WINDOW *win, device_manager_t *manager,
                                  int width, int height);

/**
 * Default input handler for views
 * Can be overridden by specific views
 *
 * @param ch Input character
 * @param manager Device manager
 * @return 1 if the input was handled, 0 if not
 */
int view_interface_default_handle_input(int ch, device_manager_t *manager);

/**
 * Default cleanup function for views
 * Can be overridden by specific views
 */
void view_interface_default_cleanup(void);

/**
 * Create a new view with default function implementations
 *
 * @param name View name
 * @param description View description
 * @param key View activation key
 * @return Initialized view interface
 */
view_interface_t view_interface_create(const char *name,
                                       const char *description,
                                       const char *key);

#endif /* VIEW_INTERFACE_H */