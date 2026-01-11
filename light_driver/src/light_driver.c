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
#include "led_strip.h"
#include "light_driver.h"

static led_strip_handle_t s_led_strip;
static uint8_t s_red = 255, s_green = 255, s_blue = 255, s_level = 255;
static bool s_power = true;

static void light_driver_apply(void)
{
    int lit_count = s_power ? (CONFIG_EXAMPLE_STRIP_LED_NUMBER * s_level) / 255 : 0;
    for (int i = 0; i < CONFIG_EXAMPLE_STRIP_LED_NUMBER; i++)
    {
        if (i < lit_count)
        {
            ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, i, s_red, s_green, s_blue));
        }
        else
        {
            ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, i, 0, 0, 0));
        }
    }
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
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
    float ratio = (float)s_level / 255;
    s_red = (uint8_t)(red_f * (float)255);
    s_green = (uint8_t)(green_f * (float)255);
    s_blue = (uint8_t)(blue_f * (float)255);
    light_driver_apply();
}

void light_driver_set_color_hue_sat(uint8_t hue, uint8_t sat)
{
    float red_f, green_f, blue_f;
    HSV_to_RGB(hue, sat, UINT8_MAX, red_f, green_f, blue_f);
    float ratio = (float)s_level / 255;
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
    s_power = power;
    light_driver_apply();
}

void light_driver_set_level(uint8_t level)
{
    s_level = level;
    light_driver_apply();
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

    s_power = power;
    light_driver_apply();
}
