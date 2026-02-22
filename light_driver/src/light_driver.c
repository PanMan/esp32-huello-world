/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Zigbee light driver example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */


#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "light_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "light_driver";
static led_strip_handle_t s_led_strip;
static uint8_t s_red = 255, s_green = 255, s_blue = 255, s_level = 255;
static bool s_power = true;
static esp_timer_handle_t s_refresh_timer;
static esp_timer_handle_t s_power_timer;
static volatile bool s_fade_active = false;
static SemaphoreHandle_t s_led_mutex;
static void light_driver_apply(void);
static void light_driver_fade_in_from_center(void);
static void light_driver_fade_out_to_center(void);
static int light_driver_get_lit_count(void);
static void light_driver_suspend_refresh(void);
static void light_driver_resume_refresh(void);

#define POWER_ON_FADE_DURATION_MS 1000
#define POWER_ON_FADE_FRAMES 20
#define LIGHT_REFRESH_PERIOD_US 3000000

static void light_driver_refresh_cb(void *arg)
{
    (void)arg;
    if (s_fade_active)
    {
        return;
    }
    // Re-apply the current buffer to mitigate occasional pixel glitches.
    light_driver_apply();
}

static void light_driver_power_log_cb(void *arg)
{
    (void)arg;
    if (!s_power)
    {
        ESP_LOGI(TAG, "Estimated current: 0.0 mA (power off)");
        return;
    }
    int lit_count = (CONFIG_EXAMPLE_STRIP_LED_NUMBER * s_level) / 255;
    float per_led_ma = 20.0f * ((float)s_red + (float)s_green + (float)s_blue) / 255.0f;
    float total_ma = (float)lit_count * per_led_ma;
    ESP_LOGI(TAG, "Estimated current: %.1f mA (lit=%d, level=%u, RGB=%u/%u/%u)",
             (double)total_ma, lit_count, s_level, s_red, s_green, s_blue);
}

static void light_driver_render(int lit_count, uint8_t red, uint8_t green, uint8_t blue)
{
    int start = (CONFIG_EXAMPLE_STRIP_LED_NUMBER - lit_count) / 2;
    int end = start + lit_count;
    for (int i = 0; i < CONFIG_EXAMPLE_STRIP_LED_NUMBER; i++)
    {
        if (i >= start && i < end)
        {
            ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, i, red, green, blue));
        }
        else
        {
            ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, i, 0, 0, 0));
        }
    }
}

static int light_driver_get_lit_count(void)
{
    return s_power ? (CONFIG_EXAMPLE_STRIP_LED_NUMBER * s_level) / 255 : 0;
}

static void light_driver_suspend_refresh(void)
{
    if (!s_refresh_timer)
    {
        return;
    }
    esp_err_t err = esp_timer_stop(s_refresh_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "Failed to stop refresh timer: %s", esp_err_to_name(err));
    }
}

static void light_driver_resume_refresh(void)
{
    if (!s_refresh_timer)
    {
        return;
    }
    esp_err_t err = esp_timer_start_periodic(s_refresh_timer, LIGHT_REFRESH_PERIOD_US);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "Failed to start refresh timer: %s", esp_err_to_name(err));
    }
}

static void light_driver_apply(void)
{
    if (!s_led_mutex)
    {
        return;
    }
    if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        return;
    }
    int lit_count = light_driver_get_lit_count();
    light_driver_render(lit_count, s_red, s_green, s_blue);
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    xSemaphoreGive(s_led_mutex);
}

static void light_driver_fade_in_from_center(void)
{
    if (!s_led_mutex)
    {
        return;
    }

    int target_lit_count = light_driver_get_lit_count();
    if (target_lit_count <= 0)
    {
        light_driver_apply();
        return;
    }

    s_fade_active = true;
    light_driver_suspend_refresh();

    TickType_t frame_delay = pdMS_TO_TICKS(POWER_ON_FADE_DURATION_MS / POWER_ON_FADE_FRAMES);
    if (frame_delay == 0)
    {
        frame_delay = 1;
    }

    for (int frame = 0; frame <= POWER_ON_FADE_FRAMES; frame++)
    {
        if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            int lit_count = (target_lit_count * frame) / POWER_ON_FADE_FRAMES;
            uint8_t red = (uint8_t)(((uint16_t)s_red * frame) / POWER_ON_FADE_FRAMES);
            uint8_t green = (uint8_t)(((uint16_t)s_green * frame) / POWER_ON_FADE_FRAMES);
            uint8_t blue = (uint8_t)(((uint16_t)s_blue * frame) / POWER_ON_FADE_FRAMES);
            light_driver_render(lit_count, red, green, blue);
            ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
            xSemaphoreGive(s_led_mutex);
        }
        if (frame < POWER_ON_FADE_FRAMES)
        {
            vTaskDelay(frame_delay);
        }
    }

    light_driver_resume_refresh();
    s_fade_active = false;
}

static void light_driver_fade_out_to_center(void)
{
    if (!s_led_mutex)
    {
        return;
    }

    int start_lit_count = (CONFIG_EXAMPLE_STRIP_LED_NUMBER * s_level) / 255;
    if (start_lit_count <= 0)
    {
        light_driver_apply();
        return;
    }

    s_fade_active = true;
    light_driver_suspend_refresh();

    TickType_t frame_delay = pdMS_TO_TICKS(POWER_ON_FADE_DURATION_MS / POWER_ON_FADE_FRAMES);
    if (frame_delay == 0)
    {
        frame_delay = 1;
    }

    for (int frame = POWER_ON_FADE_FRAMES; frame >= 0; frame--)
    {
        if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            int lit_count = (start_lit_count * frame) / POWER_ON_FADE_FRAMES;
            uint8_t red = (uint8_t)(((uint16_t)s_red * frame) / POWER_ON_FADE_FRAMES);
            uint8_t green = (uint8_t)(((uint16_t)s_green * frame) / POWER_ON_FADE_FRAMES);
            uint8_t blue = (uint8_t)(((uint16_t)s_blue * frame) / POWER_ON_FADE_FRAMES);
            light_driver_render(lit_count, red, green, blue);
            ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
            xSemaphoreGive(s_led_mutex);
        }
        if (frame > 0)
        {
            vTaskDelay(frame_delay);
        }
    }

    light_driver_resume_refresh();
    s_fade_active = false;
}

static uint8_t clamp_u8(float value)
{
    if (value < 0.0f)
    {
        return 0;
    }
    if (value > 255.0f)
    {
        return 255;
    }
    return (uint8_t)(value);
}

static void color_temperature_to_wwa(uint16_t color_temperature_mireds, uint8_t *warm, uint8_t *cold, uint8_t *amber)
{
    if (color_temperature_mireds == 0)
    {
        color_temperature_mireds = 1;
    }
    float kelvin = 1000000.0f / (float)color_temperature_mireds;
    if (kelvin < 2200.0f)
    {
        kelvin = 2200.0f;
    }
    if (kelvin > 6500.0f)
    {
        kelvin = 6500.0f;
    }

    float t = (kelvin - 2200.0f) / (6500.0f - 2200.0f);
    float warm_f = (1.0f - t) * 255.0f;
    float cold_f = t * 255.0f;
    float amber_f = 0.0f;
    if (kelvin < 3000.0f)
    {
        amber_f = ((3000.0f - kelvin) / (3000.0f - 2200.0f)) * 255.0f;
    }

    *warm = clamp_u8(warm_f);
    *cold = clamp_u8(cold_f);
    *amber = clamp_u8(amber_f);
}

void light_driver_set_color_xy(uint16_t color_current_x, uint16_t color_current_y)
{
    float red_f = 0, green_f = 0, blue_f = 0, color_x, color_y;
    color_x = (float)color_current_x / 65535;
    color_y = (float)color_current_y / 65535;
    /* assume color_Y is full light level value 1  (0-1.0) */
    float color_X = color_x / color_y;
    float color_Z = (1 - color_x - color_y) / color_y;
    /* change from xy to linear RGB NOT sRGB */
    XYZ_to_RGB(color_X, 1, color_Z, red_f, green_f, blue_f);
    s_red = (uint8_t)(red_f * (float)255);
    s_green = (uint8_t)(green_f * (float)255);
    s_blue = (uint8_t)(blue_f * (float)255);
    light_driver_apply();
}

void light_driver_set_color_hue_sat(uint8_t hue, uint8_t sat)
{
    float red_f, green_f, blue_f;
    HSV_to_RGB(hue, sat, UINT8_MAX, red_f, green_f, blue_f);
    s_red = (uint8_t)red_f;
    s_green = (uint8_t)green_f;
    s_blue = (uint8_t)blue_f;
    light_driver_apply();
}

void light_driver_set_color_temperature(uint16_t color_temperature_mireds)
{
    uint8_t warm = 0;
    uint8_t cold = 0;
    uint8_t amber = 0;
    color_temperature_to_wwa(color_temperature_mireds, &warm, &cold, &amber);
    light_driver_set_color_RGB(warm, cold, amber);
}

void light_driver_set_color_RGB(uint8_t red, uint8_t green, uint8_t blue)
{
    s_red = red;
    s_green = green;
    s_blue = blue;
    light_driver_apply();
}

void light_driver_set_power(bool power)
{
    int previous_lit_count = light_driver_get_lit_count();
    s_power = power;
    int current_lit_count = light_driver_get_lit_count();
    if (previous_lit_count == 0 && current_lit_count > 0)
    {
        light_driver_fade_in_from_center();
    }
    else if (previous_lit_count > 0 && current_lit_count == 0)
    {
        light_driver_fade_out_to_center();
    }
    else
    {
        light_driver_apply();
    }
}

void light_driver_set_level(uint8_t level)
{
    int previous_lit_count = light_driver_get_lit_count();
    s_level = level;
    int current_lit_count = light_driver_get_lit_count();
    if (previous_lit_count == 0 && current_lit_count > 0)
    {
        light_driver_fade_in_from_center();
    }
    else if (previous_lit_count > 0 && current_lit_count == 0)
    {
        light_driver_fade_out_to_center();
    }
    else
    {
        light_driver_apply();
    }
}

void light_driver_init(bool power)
{
    led_strip_config_t led_strip_conf = {
        .max_leds = CONFIG_EXAMPLE_STRIP_LED_NUMBER,
        .strip_gpio_num = CONFIG_EXAMPLE_STRIP_LED_GPIO,
    };
    led_strip_rmt_config_t rmt_conf = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&led_strip_conf, &rmt_conf, &s_led_strip));

    esp_timer_create_args_t timer_args = {
        .callback = light_driver_refresh_cb,
        .name = "led_refresh",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_refresh_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_refresh_timer, LIGHT_REFRESH_PERIOD_US));

    esp_timer_create_args_t power_args = {
        .callback = light_driver_power_log_cb,
        .name = "led_power_log",
    };
    ESP_ERROR_CHECK(esp_timer_create(&power_args, &s_power_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_power_timer, 5000000));

    s_led_mutex = xSemaphoreCreateMutex();
    if (!s_led_mutex)
    {
        ESP_LOGE(TAG, "Failed to create LED mutex");
    }
    s_power = power;
    light_driver_apply();
}
