/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#ifndef CS_FW_SRC_MGOS_SERVICE_FILESYSTEM_H_
#define CS_FW_SRC_MGOS_SERVICE_FILESYSTEM_H_

#include <stdbool.h>

#include "fw/src/mgos_init.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/*
 * Initialises mg_rpc handlers for FS commands
 */
bool mgos_rpc_service_fs_init(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CS_FW_SRC_MGOS_SERVICE_FILESYSTEM_H_ */
