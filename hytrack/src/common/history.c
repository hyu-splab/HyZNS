/**
 * history.c
 * Data History Management Implementation
 */

#include "../../include/common/history.h"

#include <string.h>

/**
 * Update history data
 *
 * @param history History structure
 * @param value New value
 * @param name History dataset name (optional)
 */
void update_history(History *history, int value, const char *name) {
  // Update max value if needed
  if (value > history->max_value) {
    history->max_value = value;
  }

  // Store name if provided
  if (name && history->name[0] == '\0') {
    strncpy(history->name, name, sizeof(history->name) - 1);
    history->name[sizeof(history->name) - 1] = '\0';
  }

  // Add value to history
  if (history->count < HISTORY_SIZE) {
    history->data[history->count++] = value;
  } else {
    history->data[history->index] = value;
    history->index = (history->index + 1) % HISTORY_SIZE;
  }
}

/**
 * Initialize history set
 *
 * @param histories History set to initialize
 */
void history_set_init(history_set_t *histories) {
  // Clear all history data
  memset(histories, 0, sizeof(history_set_t));

  // Initialize history names
  strncpy(histories->free_line_history.name, "Free Lines",
          sizeof(histories->free_line_history.name) - 1);
  strncpy(histories->victim_line_history.name, "Victim Lines",
          sizeof(histories->victim_line_history.name) - 1);
  strncpy(histories->full_line_history.name, "Full Lines",
          sizeof(histories->full_line_history.name) - 1);
  strncpy(histories->rzone_history.name, "RZone Lines",
          sizeof(histories->rzone_history.name) - 1);
  strncpy(histories->active_rzone_history.name, "Active RZones",
          sizeof(histories->active_rzone_history.name) - 1);
  strncpy(histories->active_szone_history.name, "Active SZones",
          sizeof(histories->active_szone_history.name) - 1);

  // ZNSSD specific histories
  strncpy(histories->zns_slba_history.name, "ZNS SLBA",
          sizeof(histories->zns_slba_history.name) - 1);
  strncpy(histories->zns_capacity_history.name, "ZNS Capacity",
          sizeof(histories->zns_capacity_history.name) - 1);

  // Initialize HYSSD mode ZNS histories
  strncpy(histories->mode.hyssd.hyssd_wp_history.name, "Write Pointer Line",
          sizeof(histories->mode.hyssd.hyssd_wp_history.name) - 1);
  strncpy(histories->mode.hyssd.zns_total_zones_history.name, "Total Zones",
          sizeof(histories->mode.hyssd.zns_total_zones_history.name) - 1);
  strncpy(histories->mode.hyssd.zns_open_zones_history.name, "Open Zones",
          sizeof(histories->mode.hyssd.zns_open_zones_history.name) - 1);
  strncpy(histories->mode.hyssd.zns_full_zones_history.name, "Full Zones",
          sizeof(histories->mode.hyssd.zns_full_zones_history.name) - 1);
  strncpy(histories->mode.hyssd.zns_rand_zones_history.name, "Random Zones",
          sizeof(histories->mode.hyssd.zns_rand_zones_history.name) - 1);
}

/**
 * Reset history set
 *
 * @param histories History set to reset
 */
void history_set_reset(history_set_t *histories) {
  char names[13][32];  // Increased array size to accommodate new fields

  // Save names for general histories
  strncpy(names[0], histories->free_line_history.name, sizeof(names[0]));
  strncpy(names[1], histories->victim_line_history.name, sizeof(names[1]));
  strncpy(names[2], histories->full_line_history.name, sizeof(names[2]));
  strncpy(names[3], histories->rzone_history.name, sizeof(names[3]));
  strncpy(names[4], histories->active_rzone_history.name, sizeof(names[4]));
  strncpy(names[5], histories->active_szone_history.name, sizeof(names[5]));
  strncpy(names[6], histories->zns_slba_history.name, sizeof(names[6]));
  strncpy(names[7], histories->zns_capacity_history.name, sizeof(names[7]));

  // Save names for HYSSD mode ZNS histories
  strncpy(names[8], histories->mode.hyssd.hyssd_wp_history.name,
          sizeof(names[8]));
  strncpy(names[9], histories->mode.hyssd.zns_total_zones_history.name,
          sizeof(names[9]));
  strncpy(names[10], histories->mode.hyssd.zns_open_zones_history.name,
          sizeof(names[10]));
  strncpy(names[11], histories->mode.hyssd.zns_full_zones_history.name,
          sizeof(names[11]));
  strncpy(names[12], histories->mode.hyssd.zns_rand_zones_history.name,
          sizeof(names[12]));

  // Clear all history data
  memset(histories, 0, sizeof(history_set_t));

  // Restore names for general histories
  strncpy(histories->free_line_history.name, names[0],
          sizeof(histories->free_line_history.name) - 1);
  strncpy(histories->victim_line_history.name, names[1],
          sizeof(histories->victim_line_history.name) - 1);
  strncpy(histories->full_line_history.name, names[2],
          sizeof(histories->full_line_history.name) - 1);
  strncpy(histories->rzone_history.name, names[3],
          sizeof(histories->rzone_history.name) - 1);
  strncpy(histories->active_rzone_history.name, names[4],
          sizeof(histories->active_rzone_history.name) - 1);
  strncpy(histories->active_szone_history.name, names[5],
          sizeof(histories->active_szone_history.name) - 1);
  strncpy(histories->zns_slba_history.name, names[6],
          sizeof(histories->zns_slba_history.name) - 1);
  strncpy(histories->zns_capacity_history.name, names[7],
          sizeof(histories->zns_capacity_history.name) - 1);

  // Restore names for HYSSD mode ZNS histories
  strncpy(histories->mode.hyssd.hyssd_wp_history.name, names[8],
          sizeof(histories->mode.hyssd.hyssd_wp_history.name) - 1);
  strncpy(histories->mode.hyssd.zns_total_zones_history.name, names[9],
          sizeof(histories->mode.hyssd.zns_total_zones_history.name) - 1);
  strncpy(histories->mode.hyssd.zns_open_zones_history.name, names[10],
          sizeof(histories->mode.hyssd.zns_open_zones_history.name) - 1);
  strncpy(histories->mode.hyssd.zns_full_zones_history.name, names[11],
          sizeof(histories->mode.hyssd.zns_full_zones_history.name) - 1);
  strncpy(histories->mode.hyssd.zns_rand_zones_history.name, names[12],
          sizeof(histories->mode.hyssd.zns_rand_zones_history.name) - 1);
}