/**
 * hytrack.h
 * HYTRACK - HYSSD/BBSSD/Multi-Device Monitoring Tool Main Header
 */

#ifndef HYTRACK_H
#define HYTRACK_H

#include "common/ui_common.h"
#include "core/data_types.h"
#include "core/device_manager.h"
#include "core/nvme_cmd.h"
#include "views/view_manager.h"

// Version information
#define HYTRACK_VERSION "1.0.0"
#define HYTRACK_NAME "HYTRACK"
#define HYTRACK_DESCRIPTION "HYSSD/BBSSD/Multi-Device Monitoring Tool"

// Function prototypes
int hytrack_init(int argc, char *argv[]);
void hytrack_cleanup(void);

#endif /* HYTRACK_H */