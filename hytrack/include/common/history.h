/**
 * history.h
 * Data History Management for HYTRACK
 */

#ifndef HISTORY_H
#define HISTORY_H

#include "../common/config.h"

// History data structure for trend graphs
typedef struct {
  int data[HISTORY_SIZE];
  int count;
  int index;
  int max_value; /* Track maximum value for scaling */
  char name[32]; /* Name for the history dataset */
} History;

// Define history_set_t here directly to avoid circular dependencies
typedef struct history_set_s {
  History free_line_history;
  History victim_line_history;
  History full_line_history;
  History rzone_history;
  History active_rzone_history;
  History active_szone_history;
  // Additional history fields for different device types
  History zns_slba_history;
  History zns_capacity_history;
  // Mode-specific histories
  union {
    // HYSSD specific histories
    struct {
      History hyssd_wp_history;
      // ZNS-related histories for HYSSD mode
      History zns_total_zones_history;
      History zns_open_zones_history;
      History zns_full_zones_history;
      History zns_rand_zones_history;
    } hyssd;

    // BBSSD specific histories
    struct {
      History bbssd_specific_history;
    } bbssd;

    // ZNSSD specific histories
    struct {
      History znssd_specific_history;
    } znssd;
  } mode;
} history_set_t;

/**
 * Update history data
 *
 * @param history History structure
 * @param value New value
 * @param name History dataset name (optional)
 */
void update_history(History *history, int value, const char *name);

/**
 * Initialize history set
 *
 * @param histories History set to initialize
 */
void history_set_init(history_set_t *histories);

/**
 * Reset history set
 *
 * @param histories History set to reset
 */
void history_set_reset(history_set_t *histories);

#endif /* HISTORY_H */