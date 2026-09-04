#include "status_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_BRIDGE_STATUS_LED_ENABLED

static const char *TAG = "led";

#define LED_GPIO ((gpio_num_t) CONFIG_BRIDGE_STATUS_LED_GPIO)

// Each pattern is a list of millisecond durations, alternating on and off, repeated forever.
static const uint16_t PATTERN_NO_WIFI[] = { 150, 150 };
static const uint16_t PATTERN_ONLINE[]  = { 60, 1940 };
static const uint16_t PATTERN_AP[]      = { 100, 150, 100, 1150 };

static const uint16_t *s_pattern = PATTERN_NO_WIFI;
static size_t s_pattern_len = sizeof(PATTERN_NO_WIFI) / sizeof(uint16_t);
static size_t s_step = 0;

static esp_timer_handle_t s_timer;

static void write_led(bool on)
{
#if CONFIG_BRIDGE_STATUS_LED_ACTIVE_LOW
    gpio_set_level(LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(LED_GPIO, on ? 1 : 0);
#endif
}

static void advance(void *arg)
{
    (void) arg;

    // Even steps are on, odd steps are off.
    write_led((s_step % 2) == 0);

    const uint16_t hold = s_pattern[s_step];
    s_step = (s_step + 1) % s_pattern_len;

    esp_timer_start_once(s_timer, (uint64_t) hold * 1000);
}

esp_err_t status_led_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_BRIDGE_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot drive GPIO %d: %s", CONFIG_BRIDGE_STATUS_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    write_led(false);

    const esp_timer_create_args_t args = { .callback = &advance, .name = "status_led" };
    err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Status LED on GPIO %d", CONFIG_BRIDGE_STATUS_LED_GPIO);
    advance(NULL);

    return ESP_OK;
}

void status_led_set(status_led_state_t state)
{
    const uint16_t *pattern;
    size_t len;

    switch (state) {
        case STATUS_LED_ONLINE:
            pattern = PATTERN_ONLINE;
            len = sizeof(PATTERN_ONLINE) / sizeof(uint16_t);
            break;
        case STATUS_LED_AP:
            pattern = PATTERN_AP;
            len = sizeof(PATTERN_AP) / sizeof(uint16_t);
            break;
        default:
            pattern = PATTERN_NO_WIFI;
            len = sizeof(PATTERN_NO_WIFI) / sizeof(uint16_t);
            break;
    }

    if (pattern == s_pattern) {
        return;
    }

    s_pattern = pattern;
    s_pattern_len = len;
    s_step = 0;

    // Restart rather than wait out the current step, so a state change is visible at once.
    esp_timer_stop(s_timer);
    advance(NULL);
}

#else /* !CONFIG_BRIDGE_STATUS_LED_ENABLED */

esp_err_t status_led_init(void) { return ESP_OK; }
void status_led_set(status_led_state_t state) { (void) state; }

#endif
