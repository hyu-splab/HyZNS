/**
 * view_interface.c
 * Common View Interface Implementation
 */

#include "../../include/views/view_interface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/core/device_manager.h"

/**
 * Default initialization function for views
 * Can be overridden by specific views
 *
 * @return 0 on success, non-zero on failure
 */
int view_interface_default_init(void) { return 0; }

/**
 * Default activation function for views
 * Can be overridden by specific views
 *
 * @param manager Device manager
 * @return 0 on success, non-zero on failure
 */
int view_interface_default_activate(device_manager_t *manager) { return 0; }

/**
 * Default deactivation function for views
 * Can be overridden by specific views
 *
 * @return 0 on success, non-zero on failure
 */
int view_interface_default_deactivate(void) { return 0; }

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
                                  int width, int height) {
  // Default implementation just shows a placeholder message
  mvwprintw(win, 3, 1, "This view has not been implemented yet.");
  return 0;
}

/**
 * Default input handler for views
 * Can be overridden by specific views
 *
 * @param ch Input character
 * @param manager Device manager
 * @return 1 if the input was handled, 0 if not
 */
int view_interface_default_handle_input(int ch, device_manager_t *manager) {
  // Default implementation doesn't handle any input
  return 0;
}

/**
 * Default cleanup function for views
 * Can be overridden by specific views
 */
void view_interface_default_cleanup(void) {
  // Default implementation doesn't do anything
}

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
                                       const char *key) {
  view_interface_t view = {.name = name,
                           .description = description,
                           .key = key,
                           .init = view_interface_default_init,
                           .activate = view_interface_default_activate,
                           .deactivate = view_interface_default_deactivate,
                           .render = view_interface_default_render,
                           .handle_input = view_interface_default_handle_input,
                           .cleanup = view_interface_default_cleanup};

  return view;
}