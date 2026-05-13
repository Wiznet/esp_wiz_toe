#include "esp_wiz_toe_internal.h"
#include "esp_wiz_toe_spi.h"

#include "esp_log.h"

#if __has_include("wizchip_conf.h")
#include "wizchip_conf.h"
#define ESP_WIZ_TOE_HAS_IOLIB 1
#else
#define ESP_WIZ_TOE_HAS_IOLIB 0
#endif

static const char *TAG = "esp_wiz_toe_port";

esp_err_t esp_wiz_toe_port_init(const esp_wiz_toe_config_t *cfg)
{
    return esp_wiz_toe_spi_init(cfg);
}

esp_err_t esp_wiz_toe_port_deinit(void)
{
    return esp_wiz_toe_spi_deinit();
}

esp_err_t esp_wiz_toe_port_register_iolib_callbacks(void)
{
    return esp_wiz_toe_spi_register_iolib_callbacks();
}

bool esp_wiz_toe_port_link_is_up(void)
{
#if ESP_WIZ_TOE_HAS_IOLIB
    uint8_t link_state = 0;

    if (ctlwizchip(CW_GET_PHYLINK, (void *)&link_state) != 0) {
        ESP_LOGW(TAG, "CW_GET_PHYLINK failed");
        return false;
    }
    return link_state != 0;
#else
    return false;
#endif
}
