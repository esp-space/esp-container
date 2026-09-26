// SPDX-License-Identifier: Apache-2.0
// QEMU-only OpenETH setup while the ABI 2 guest is live. Never flash this.
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *const TAG = "five-capacity-openeth";
static esp_eth_handle_t s_eth;
static esp_eth_mac_t *s_mac;
static esp_eth_phy_t *s_phy;
static esp_netif_t *s_netif;
static esp_eth_netif_glue_handle_t s_glue;
static esp_event_handler_instance_t s_ip_handler;
static bool s_started;
static atomic_bool s_got_ip;
static atomic_uint s_failed_count;
static atomic_uint s_failed_size;
static atomic_uint s_failed_caps;

void capacity_openeth_report_heap(const char *phase, esp_err_t error)
{
    ESP_LOGI(TAG,
             "openeth phase=%s error=%d got_ip=%d free=%u largest=%u min=%u alloc_fail=%u last_size=%u last_caps=%u",
             phase, (int)error, atomic_load(&s_got_ip),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             atomic_load(&s_failed_count), atomic_load(&s_failed_size),
             atomic_load(&s_failed_caps));
}

static void failed_allocation(size_t size, uint32_t caps, const char *function_name)
{
    (void)function_name;
    atomic_fetch_add(&s_failed_count, 1);
    atomic_store(&s_failed_size, (unsigned)size);
    atomic_store(&s_failed_caps, caps);
}

static void got_ip(void *context, esp_event_base_t base, int32_t id, void *data)
{
    (void)context;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *event = data;
    if (event && event->esp_netif == s_netif) atomic_store(&s_got_ip, true);
}

static void cleanup(void)
{
    if (s_started) { (void)esp_eth_stop(s_eth); s_started = false; }
    if (s_ip_handler) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_glue) { (void)esp_eth_del_netif_glue(s_glue); s_glue = NULL; }
    if (s_netif) { esp_netif_destroy(s_netif); s_netif = NULL; }
    if (s_eth) { (void)esp_eth_driver_uninstall(s_eth); s_eth = NULL; }
    if (s_mac) { (void)s_mac->del(s_mac); s_mac = NULL; }
    if (s_phy) { (void)s_phy->del(s_phy); s_phy = NULL; }
    capacity_openeth_report_heap("after_cleanup", ESP_OK);
}

bool capacity_openeth_start(void)
{
    atomic_store(&s_got_ip, false);
    atomic_store(&s_failed_count, 0);
    atomic_store(&s_failed_size, 0);
    atomic_store(&s_failed_caps, 0);
    const esp_err_t hook = heap_caps_register_failed_alloc_callback(failed_allocation);
    capacity_openeth_report_heap("before", hook);
    if (hook != ESP_OK) return false;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1;
    s_mac = esp_eth_mac_new_openeth(&mac_config);
    s_phy = esp_eth_phy_new_generic(&phy_config);
    if (!s_mac || !s_phy) { capacity_openeth_report_heap("create_mac_phy", ESP_ERR_NO_MEM); cleanup(); return false; }
    capacity_openeth_report_heap("created_mac_phy", ESP_OK);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(s_mac, s_phy);
    esp_err_t error = esp_eth_driver_install(&eth_config, &s_eth);
    capacity_openeth_report_heap("driver_install", error);
    if (error != ESP_OK) { cleanup(); return false; }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_config);
    if (!s_netif) { capacity_openeth_report_heap("netif_new", ESP_ERR_NO_MEM); cleanup(); return false; }
    s_glue = esp_eth_new_netif_glue(s_eth);
    if (!s_glue) { capacity_openeth_report_heap("glue_new", ESP_ERR_NO_MEM); cleanup(); return false; }
    error = esp_netif_attach(s_netif, s_glue);
    capacity_openeth_report_heap("netif_attach", error);
    if (error != ESP_OK) { cleanup(); return false; }

    error = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                 got_ip, NULL, &s_ip_handler);
    capacity_openeth_report_heap("register_ip", error);
    if (error != ESP_OK) { cleanup(); return false; }
    error = esp_eth_start(s_eth);
    s_started = error == ESP_OK;
    capacity_openeth_report_heap("eth_start", error);
    if (error != ESP_OK) { cleanup(); return false; }
    for (unsigned waited_ms = 0; waited_ms < 10000 && !atomic_load(&s_got_ip); waited_ms += 20)
        vTaskDelay(pdMS_TO_TICKS(20));
    const bool ready = atomic_load(&s_got_ip);
    capacity_openeth_report_heap("dhcp_result", ready ? ESP_OK : ESP_ERR_TIMEOUT);
    if (!ready) cleanup();
    return ready;
}

void capacity_openeth_stop(void)
{
    cleanup();
}
