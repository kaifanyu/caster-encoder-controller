#include "telemetry.h"
#include "config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include <fcntl.h>
#include <cstring>
#include <cstdio>

namespace caster {
namespace {
std::atomic<bool> connected{false};
int udp_socket = -1;
sockaddr_in destination{};
char device_id[18]{};
void network_event(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        connected.store(false);
    if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)
        connected.store(false);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        connected.store(true);
}
}  // namespace

void transport_init() {
    // Do not automatically erase NVS on initialization errors.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t* netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(netif ? ESP_OK : ESP_ERR_NO_MEM);
    if (kLocalIpv4[0] != '\0') {
        esp_netif_ip_info_t address{};
        ESP_ERROR_CHECK(inet_pton(AF_INET, kLocalIpv4, &address.ip.addr) == 1 ? ESP_OK : ESP_ERR_INVALID_ARG);
        ESP_ERROR_CHECK(inet_pton(AF_INET, NETWORK_GATEWAY, &address.gw.addr) == 1 ? ESP_OK : ESP_ERR_INVALID_ARG);
        ESP_ERROR_CHECK(inet_pton(AF_INET, NETWORK_NETMASK, &address.netmask.addr) == 1 ? ESP_OK : ESP_ERR_INVALID_ARG);
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &address));
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi{};
    static_assert(sizeof(WIFI_SSID) <= sizeof(wifi.sta.ssid), "SSID too long");
    static_assert(sizeof(WIFI_PASSWORD) <= sizeof(wifi.sta.password), "Password too long");
    memcpy(wifi.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
    memcpy(wifi.sta.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD) - 1);
    wifi.sta.threshold.authmode = strlen(WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    snprintf(device_id, sizeof(device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(UDP_PORT);
    ESP_ERROR_CHECK(inet_pton(AF_INET, RECEIVER_IPV4, &destination.sin_addr) == 1 ? ESP_OK : ESP_ERR_INVALID_ARG);
    udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_ERROR_CHECK(udp_socket >= 0 ? ESP_OK : ESP_FAIL);
    const int flags = fcntl(udp_socket, F_GETFL, 0);
    ESP_ERROR_CHECK(flags >= 0 && fcntl(udp_socket, F_SETFL, flags | O_NONBLOCK) >= 0 ? ESP_OK : ESP_FAIL);
    transport_reconnect();
}

const char* transport_device_id() { return device_id; }

bool transport_connected() { return connected.load(); }
void transport_reconnect() {
    if (!transport_connected()) (void)esp_wifi_connect();
}
bool transport_send(const char* data) {
    if (!data || !transport_connected()) return false;
    const size_t length = strlen(data);
    return sendto(udp_socket, data, length, 0,
                  reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) == static_cast<ssize_t>(length);
}
}  // namespace caster
