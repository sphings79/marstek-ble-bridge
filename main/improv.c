#include "improv.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi.h"

static const char *TAG = "improv";

#define IMPROV_UART UART_NUM_0
#define IMPROV_RX_BUFFER 1024

// "IMPROV" plus the version byte. Every packet in both directions starts with this.
static const uint8_t HEADER[] = { 'I', 'M', 'P', 'R', 'O', 'V', 0x01 };
#define HEADER_LEN (sizeof(HEADER))

#define TYPE_CURRENT_STATE 0x01
#define TYPE_ERROR_STATE 0x02
#define TYPE_RPC 0x03
#define TYPE_RPC_RESULT 0x04

#define STATE_READY 0x02
#define STATE_PROVISIONING 0x03
#define STATE_PROVISIONED 0x04

#define ERROR_NONE 0x00
#define ERROR_INVALID_RPC 0x01
#define ERROR_UNKNOWN_RPC 0x02
#define ERROR_UNABLE_TO_CONNECT 0x03

#define RPC_WIFI_SETTINGS 0x01
#define RPC_IDENTIFY 0x02
#define RPC_GET_CURRENT_STATE 0x03
#define RPC_GET_DEVICE_INFO 0x04
#define RPC_GET_WIFI_NETWORKS 0x05

// The longest payload worth accepting is a set of credentials: two length-prefixed strings, each
// bounded by what the WiFi driver takes.
#define MAX_PAYLOAD 192

static uint8_t s_packet[HEADER_LEN + 2 + MAX_PAYLOAD + 1];

static void send_packet(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t out[HEADER_LEN + 2 + 255 + 1];

    memcpy(out, HEADER, HEADER_LEN);
    out[HEADER_LEN] = type;
    out[HEADER_LEN + 1] = len;
    if (len) {
        memcpy(out + HEADER_LEN + 2, payload, len);
    }

    const size_t body = HEADER_LEN + 2 + len;

    uint8_t checksum = 0;
    for (size_t i = 0; i < body; i++) {
        checksum += out[i];
    }
    out[body] = checksum;

    uart_write_bytes(IMPROV_UART, out, body + 1);

    // The installer looks for a newline between packets; without it the next one can be read as a
    // continuation of this one.
    uart_write_bytes(IMPROV_UART, "\n", 1);
}

static void send_state(uint8_t state)
{
    send_packet(TYPE_CURRENT_STATE, &state, 1);
}

static void send_error(uint8_t error)
{
    send_packet(TYPE_ERROR_STATE, &error, 1);
}

/** Append a length-prefixed string to an RPC result. Returns the new length. */
static uint8_t append_string(uint8_t *buffer, uint8_t at, const char *text)
{
    const size_t len = strlen(text);
    buffer[at] = (uint8_t) len;
    memcpy(buffer + at + 1, text, len);
    return at + 1 + (uint8_t) len;
}

static void send_rpc_result(uint8_t command, const uint8_t *strings, uint8_t strings_len)
{
    uint8_t payload[MAX_PAYLOAD + 2];

    payload[0] = command;
    payload[1] = strings_len;
    if (strings_len) {
        memcpy(payload + 2, strings, strings_len);
    }

    send_packet(TYPE_RPC_RESULT, payload, strings_len + 2);
}

static uint8_t current_state(void)
{
    return wifi_is_connected() ? STATE_PROVISIONED : STATE_READY;
}

/** The address the browser should open once provisioning is done. */
static void send_provisioned_result(void)
{
    char url[64];
    snprintf(url, sizeof(url), "http://%s/", wifi_ip());

    uint8_t strings[MAX_PAYLOAD];
    const uint8_t len = append_string(strings, 0, url);

    send_state(STATE_PROVISIONED);
    send_rpc_result(RPC_WIFI_SETTINGS, strings, len);
}

static void handle_wifi_settings(const uint8_t *data, uint8_t len)
{
    char ssid[WIFI_SSID_MAX] = { 0 };
    char password[WIFI_PASSWORD_MAX] = { 0 };

    if (len < 1) {
        send_error(ERROR_INVALID_RPC);
        return;
    }

    const uint8_t ssid_len = data[0];
    if (ssid_len + 1u > len || ssid_len >= sizeof(ssid)) {
        send_error(ERROR_INVALID_RPC);
        return;
    }
    memcpy(ssid, data + 1, ssid_len);

    const uint8_t rest = len - 1 - ssid_len;
    if (rest >= 1) {
        const uint8_t password_len = data[1 + ssid_len];
        if (password_len + 2u + ssid_len > len || password_len >= sizeof(password)) {
            send_error(ERROR_INVALID_RPC);
            return;
        }
        memcpy(password, data + 2 + ssid_len, password_len);
    }

    send_state(STATE_PROVISIONING);

    if (wifi_provision(ssid, password) == ESP_OK) {
        send_provisioned_result();
    } else {
        send_error(ERROR_UNABLE_TO_CONNECT);
        send_state(STATE_READY);
    }
}

static void handle_device_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    uint8_t strings[MAX_PAYLOAD];
    uint8_t len = 0;

    len = append_string(strings, len, "Marstek BLE Bridge");
    len = append_string(strings, len, app->version);
    len = append_string(strings, len, CONFIG_IDF_TARGET);
    len = append_string(strings, len, "Marstek BLE Bridge");

    send_rpc_result(RPC_GET_DEVICE_INFO, strings, len);
}

static void handle_wifi_networks(void)
{
    wifi_scan_entry_t networks[12];
    const size_t found = wifi_scan(networks, 12);

    for (size_t i = 0; i < found; i++) {
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", networks[i].rssi);

        uint8_t strings[MAX_PAYLOAD];
        uint8_t len = 0;

        len = append_string(strings, len, networks[i].ssid);
        len = append_string(strings, len, rssi);
        len = append_string(strings, len, networks[i].secured ? "YES" : "NO");

        send_rpc_result(RPC_GET_WIFI_NETWORKS, strings, len);
    }

    // An empty result terminates the list.
    send_rpc_result(RPC_GET_WIFI_NETWORKS, NULL, 0);
}

static void handle_rpc(const uint8_t *payload, uint8_t len)
{
    if (len < 2) {
        send_error(ERROR_INVALID_RPC);
        return;
    }

    const uint8_t command = payload[0];
    const uint8_t data_len = payload[1];

    if (data_len + 2u > len) {
        send_error(ERROR_INVALID_RPC);
        return;
    }

    const uint8_t *data = payload + 2;

    switch (command) {
        case RPC_WIFI_SETTINGS:
            handle_wifi_settings(data, data_len);
            break;

        case RPC_GET_CURRENT_STATE:
            send_state(current_state());
            // A device that is already on a network answers with the address as well, so the
            // installer can offer the link rather than asking for credentials it does not need.
            if (wifi_is_connected()) {
                char url[64];
                snprintf(url, sizeof(url), "http://%s/", wifi_ip());

                uint8_t strings[MAX_PAYLOAD];
                const uint8_t written = append_string(strings, 0, url);
                send_rpc_result(RPC_WIFI_SETTINGS, strings, written);
            }
            break;

        case RPC_GET_DEVICE_INFO:
            handle_device_info();
            break;

        case RPC_GET_WIFI_NETWORKS:
            handle_wifi_networks();
            break;

        case RPC_IDENTIFY:
            // Nothing to flash that is not already blinking. Answering at all is what matters.
            send_error(ERROR_NONE);
            break;

        default:
            send_error(ERROR_UNKNOWN_RPC);
            break;
    }
}

/**
 * Collect one packet from the byte stream.
 *
 * The console shares this port, so most of what arrives is not Improv at all. Rather than trying
 * to tell the two apart, the reader simply hunts for the header and drops anything that does not
 * checksum - log noise fails that test on its own.
 */
static void improv_task(void *arg)
{
    (void) arg;

    size_t have = 0;

    while (true) {
        uint8_t byte;
        if (uart_read_bytes(IMPROV_UART, &byte, 1, portMAX_DELAY) != 1) {
            continue;
        }

        if (have < HEADER_LEN) {
            if (byte == HEADER[have]) {
                s_packet[have++] = byte;
            } else {
                // Restart, but allow the byte that broke the run to begin a new one.
                have = (byte == HEADER[0]) ? 1 : 0;
                s_packet[0] = byte;
            }
            continue;
        }

        s_packet[have++] = byte;

        if (have < HEADER_LEN + 2) {
            continue;
        }

        const uint8_t type = s_packet[HEADER_LEN];
        const uint8_t len = s_packet[HEADER_LEN + 1];

        if (len > MAX_PAYLOAD) {
            have = 0;
            continue;
        }

        if (have < HEADER_LEN + 2 + (size_t) len + 1) {
            continue;
        }

        uint8_t checksum = 0;
        for (size_t i = 0; i < have - 1; i++) {
            checksum += s_packet[i];
        }

        if (checksum == s_packet[have - 1] && type == TYPE_RPC) {
            handle_rpc(s_packet + HEADER_LEN + 2, len);
        }

        have = 0;
    }
}

esp_err_t improv_start(void)
{
    const uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(IMPROV_UART, IMPROV_RX_BUFFER, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No serial provisioning: %s", esp_err_to_name(err));
        return err;
    }

    uart_param_config(IMPROV_UART, &config);

    if (xTaskCreate(improv_task, "improv", 4096, NULL, 4, NULL) != pdPASS) {
        uart_driver_delete(IMPROV_UART);
        return ESP_ERR_NO_MEM;
    }

    // Announced unprompted, because the installer may already be listening by the time the first
    // boot gets here.
    send_state(current_state());

    ESP_LOGI(TAG, "Serial provisioning ready");
    return ESP_OK;
}
