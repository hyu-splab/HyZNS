/**
 * debug_view.h
 * Debug View Definition
 */

#ifndef DEBUG_VIEW_H
#define DEBUG_VIEW_H

#include "view_interface.h"

// Global debug view interface
extern view_interface_t debug_view;

// Debug log management
#define DEBUG_LOG_SIZE 1024

/**
 * Initialize debug view
 *
 * @return 0 on success, non-zero on failure
 */
int debug_view_init(void);

/**
 * Add a log entry to the debug log
 *
 * @param format Format string
 * @param ... Additional arguments
 */
void debug_log_add(const char *format, ...);

/**
 * Clear the debug log
 */
void debug_log_clear(void);

/**
 * Get number of entries in debug log
 *
 * @return Number of log entries
 */
int debug_log_get_count(
    void);  // 이름 변경: debug_log_count -> debug_log_get_count

/**
 * Get log entry by index
 *
 * @param index Log entry index
 * @return Log entry string or NULL if invalid
 */
const char *debug_log_get(int index);

#endif /* DEBUG_VIEW_H */