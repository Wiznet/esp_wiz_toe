/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * esp_eth PHY driver for the WIZnet W6300 internal PHY (the W6300 counterpart of
 * esp_eth_phy_w5500.h). Unlike the W5500, whose whole PHY is the single PHYCFGR
 * register, the W6300 splits it into PHYSR (status, read-only) and
 * PHYCR0/PHYCR1 (operation mode / reset & power-down), and those control
 * registers are lock-protected — the MAC unlocks them during init.
 */

#pragma once

#include "esp_eth_com.h"
#include "esp_eth_phy.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
* @brief Create a PHY instance of W6300
*
* @param[in] config: configuration of PHY
*
* @return
*      - instance: create PHY instance successfully
*      - NULL: create PHY instance failed because some error occurred
*/
esp_eth_phy_t *esp_eth_phy_new_w6300(const eth_phy_config_t *config);

#ifdef __cplusplus
}
#endif
