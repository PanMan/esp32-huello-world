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

static void light_driver_set_all_pixels(uint8_t red, uint8_t green, uint8_t blue)
{
    for (int i = 0; i < CONFIG_EXAMPLE_STRIP_LED_NUMBER; i++)
    {
        ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, i, red, green, blue));
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

static void color_temperature_to_rgb(uint16_t color_temperature_mireds, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (color_temperature_mireds == 0)
    {
        color_temperature_mireds = 1;
    }
    float kelvin = 1000000.0f / (float)color_temperature_mireds;
    if (kelvin < 1000.0f)
    {
        kelvin = 1000.0f;
    }
    if (kelvin > 40000.0f)
    {
        kelvin = 40000.0f;
    }

    float temp = kelvin / 100.0f;
    float r, g, b;

    if (temp <= 66.0f)
    {
        r = 255.0f;
        g = 99.4708025861f * logf(temp) - 161.1195681661f;
        if (temp <= 19.0f)
        {
            b = 0.0f;
        }
        else
        {
            b = 138.5177312231f * logf(temp - 10.0f) - 305.0447927307f;
        }
    }
    else
    {
        r = 329.698727446f * powf(temp - 60.0f, -0.1332047592f);
        g = 288.1221695283f * powf(temp - 60.0f, -0.0755148492f);
        b = 255.0f;
    }

    *red = clamp_u8(r);
    *green = clamp_u8(g);
    *blue = clamp_u8(b);
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
    light_driver_set_all_pixels(s_red * ratio, s_green * ratio, s_blue * ratio);
}

void light_driver_set_color_hue_sat(uint8_t hue, uint8_t sat)
{
    float red_f, green_f, blue_f;
    HSV_to_RGB(hue, sat, UINT8_MAX, red_f, green_f, blue_f);
    float ratio = (float)s_level / 255;
    s_red = (uint8_t)red_f;
    s_green = (uint8_t)green_f;
    s_blue = (uint8_t)blue_f;
    light_driver_set_all_pixels(s_red * ratio, s_green * ratio, s_blue * ratio);
}

void light_driver_set_color_temperature(uint16_t color_temperature_mireds)
{
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    color_temperature_to_rgb(color_temperature_mireds, &red, &green, &blue);
    light_driver_set_color_RGB(red, green, blue);
}

void light_driver_set_color_RGB(uint8_t red, uint8_t green, uint8_t blue)
{
    float ratio = (float)s_level / 255;
    s_red = red;
    s_green = green;
    s_blue = blue;
    light_driver_set_all_pixels(red * ratio, green * ratio, blue * ratio);
}

void light_driver_set_power(bool power)
{
    light_driver_set_all_pixels(s_red * power, s_green * power, s_blue * power);
}

void light_driver_set_level(uint8_t level)
{
    s_level = level;
    float ratio = (float)s_level / 255;
    light_driver_set_all_pixels(s_red * ratio, s_green * ratio, s_blue * ratio);
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

    light_driver_set_power(power);
}
