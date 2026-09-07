/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Drives one UPnP session: discover an IGD, read its description, optionally
 * subscribe to eventing, then add and delete a port mapping.
 *
 * Backend-neutral -- everything network-facing goes through the vtable handed
 * to upnp_client_start() and reaches the wire via upnp_transport.c.
 */
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "net_config.h"
#include "upnp_client.h"
#include "upnp_core.h"
#include "upnp_transport.h"

static const char *TAG = "upnp";

/* Discovery is a single multicast probe with a 3 s window, and routers commonly
 * miss the first one. Retry rather than giving up on one lost datagram. */
#define DISCOVER_ATTEMPTS  5

#define EVENT_PORT         5002
#define EVENT_BUF_SIZE     2048

typedef struct {
    const char *name;
    const void *ops;
    bool      (*is_up)(void);
    const char *(*local_ip)(void);
} upnp_client_ctx_t;

static const char HTTP_OK[] = "HTTP/1.1 200 OK\r\n\r\n";

/*
 * Wait for the IGD to call back with a notification.
 *
 * The original polled the chip's socket state machine in a switch and reopened
 * the socket by hand on SOCK_CLOSED. accept() covers all of that: it has BSD
 * semantics on both backends, so listen_fd stays listening across connections
 * and each accepted fd is closed on its own.
 */
static void listen_for_events(const char *name, uint32_t seconds)
{
    int listen_fd = upnp_transport_listen(EVENT_PORT);
    if (listen_fd < 0) {
        ESP_LOGW(TAG, "[%s] cannot listen on %d for eventing", name, EVENT_PORT);
        return;
    }
    ESP_LOGI(TAG, "[%s] waiting %us for eventing on port %d",
             name, (unsigned)seconds, EVENT_PORT);

    char *buf = malloc(EVENT_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory for the eventing buffer", name);
        upnp_transport_close(listen_fd);
        return;
    }

    uint32_t waited = 0;
    while (waited < seconds) {
        int fd = upnp_transport_accept(listen_fd, 1000);
        waited++;
        if (fd < 0) {
            continue;                   /* nothing yet */
        }

        /* Answer first, parse after. A NOTIFY that goes unacknowledged is
         * retried, and at least one router (ipTIME) stops serving control
         * actions while it is retrying — which showed up as AddPortMapping
         * timing out several seconds later. The reply costs nothing even when
         * the body was empty or unrecognised, so it is not conditional on
         * having read one. */
        int n = upnp_transport_recv(fd, buf, EVENT_BUF_SIZE, 1000);
        upnp_transport_send(fd, HTTP_OK, strlen(HTTP_OK));
        if (n > 0) {
            upnp_parse_eventing(buf);
        }
        upnp_transport_close(fd);
    }

    free(buf);
    upnp_transport_close(listen_fd);
}

static void upnp_client_task(void *arg)
{
    upnp_client_ctx_t *c = (upnp_client_ctx_t *)arg;

    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    upnp_transport_bind(c->ops);

    /* Step 1 -- find an Internet Gateway Device. */
    int ret = UPNP_ERR_TIMEOUT;
    for (int i = 1; i <= DISCOVER_ATTEMPTS && ret != UPNP_OK; i++) {
        ESP_LOGI(TAG, "[%s] sending M-SEARCH (%d/%d)", c->name, i, DISCOVER_ATTEMPTS);
        ret = upnp_discover();
    }
    if (ret != UPNP_OK) {
        ESP_LOGE(TAG, "[%s] no IGD answered — is UPnP enabled on the router?",
                 c->name);
        goto done;
    }
    ESP_LOGI(TAG, "[%s] IGD found at %s:%u", c->name,
             upnp_igd_ip(), upnp_igd_port());

    /* Step 2 -- read the description to learn where actions are posted. */
    if ((ret = upnp_get_description()) != UPNP_OK) {
        ESP_LOGE(TAG, "[%s] description failed (%d)", c->name, ret);
        goto done;
    }
    ESP_LOGI(TAG, "[%s] controlURL   %s", c->name, upnp_control_url());
    ESP_LOGI(TAG, "[%s] eventSubURL  %s", c->name, upnp_event_sub_url());

    /* Step 3 -- eventing is optional; a failure here does not stop the mapping. */
    if (UPNP_EVENT_LISTEN_SEC > 0) {
        if ((ret = upnp_subscribe(c->local_ip(), EVENT_PORT)) == UPNP_OK) {
            ESP_LOGI(TAG, "[%s] subscribed to eventing", c->name);
            listen_for_events(c->name, UPNP_EVENT_LISTEN_SEC);
        } else {
            ESP_LOGW(TAG, "[%s] subscribe failed (%d) — continuing", c->name, ret);
        }
    }

    /* Step 4 -- the actual point of the example.
     *
     * An empty UPNP_MAP_INT_IP means "this interface", which is the useful
     * default: forwarding to the Ethernet address while the session runs over
     * Wi-Fi would open a port onto the wrong interface. */
    const char *internal_ip = UPNP_MAP_INT_IP[0] ? UPNP_MAP_INT_IP : c->local_ip();

    ret = upnp_add_port(UPNP_MAP_PROTOCOL, UPNP_MAP_EXT_PORT, internal_ip,
                        UPNP_MAP_INT_PORT, UPNP_MAP_DESCRIPTION);
    if (ret == UPNP_OK) {
        ESP_LOGI(TAG, "[%s] mapped %s %d -> %s:%d (\"%s\")", c->name,
                 UPNP_MAP_PROTOCOL, UPNP_MAP_EXT_PORT, internal_ip,
                 UPNP_MAP_INT_PORT, UPNP_MAP_DESCRIPTION);
    } else if (ret == UPNP_ERR_TIMEOUT) {
        /* Not the same as a refusal. The request reached the router and it may
         * well have applied it -- an ipTIME did exactly that while its response
         * went unread -- so say what is actually known instead of calling it a
         * failure and leaving a mapping nobody expects. */
        ESP_LOGW(TAG, "[%s] no answer to AddPortMapping — the mapping may still "
                      "have been created; check the router's admin page",
                 c->name);
        goto done;
    } else {
        /* A positive value is the router's own UPnP error code. */
        ESP_LOGE(TAG, "[%s] AddPortMapping failed (%d)", c->name, ret);
        if (ret == 718) {
            /* Worth spelling out: the usual cause is a mapping this example
             * left behind on an earlier run with UPNP_DELETE_AFTER_ADD at 0,
             * pointing at the other interface. Deleting it here is not an
             * option -- 718 means the entry belongs to a different host, and
             * removing someone else's mapping is not ours to do. */
            ESP_LOGE(TAG, "[%s] external port %d is already mapped to another "
                          "host — remove it in the router's admin page",
                     c->name, UPNP_MAP_EXT_PORT);
        }
        goto done;
    }

#if UPNP_DELETE_AFTER_ADD
    ret = upnp_delete_port(UPNP_MAP_PROTOCOL, UPNP_MAP_EXT_PORT);
    if (ret == UPNP_OK) {
        ESP_LOGI(TAG, "[%s] removed the mapping again", c->name);
    } else {
        ESP_LOGE(TAG, "[%s] DeletePortMapping failed (%d)", c->name, ret);
    }
#else
    ESP_LOGI(TAG, "[%s] mapping left in place — check the router's admin page",
             c->name);
#endif

done:
    ESP_LOGI(TAG, "[%s] session finished", c->name);
    free(c);
    vTaskDelete(NULL);
}

void upnp_client_start(const char *name, const void *ops,
                       bool (*is_up)(void), const char *(*local_ip)(void))
{
    upnp_client_ctx_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name = name;
    c->ops = ops;
    c->is_up = is_up;
    c->local_ip = local_ip;

    /* The buffers in upnp_core.c are static, but the parsers recurse through
     * strstr on multi-KB documents, so keep the stack generous. */
    if (xTaskCreate(upnp_client_task, name, 8192, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
