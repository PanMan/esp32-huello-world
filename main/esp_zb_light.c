/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Zigbee HA_color_dimmable_light Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */

#include "esp_zb_light.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_color_control.h"

#if !defined CONFIG_ZB_ZCZR
#error Define ZB_ZCZR in idf.py menuconfig to compile light (Router) source code.
#endif

static const char *TAG = "ESP_ZB_COLOR_DIMM_LIGHT";
/********************* Define functions **************************/
static uint16_t s_color_temperature = ESP_ZB_ZCL_COLOR_CONTROL_COLOR_TEMPERATURE_DEF_VALUE;
static uint16_t s_color_temp_min = 153;  // ~6500K
static uint16_t s_color_temp_max = 454;  // ~2200K
static uint16_t s_color_temp_couple_level_min = 153; // keep CT in range when coupled to level
static uint16_t s_startup_color_temp = 0xffff; // use previous
static bool s_persist_power = false;
static uint8_t s_persist_level = 255;
static uint16_t s_persist_ct = ESP_ZB_ZCL_COLOR_CONTROL_COLOR_TEMPERATURE_DEF_VALUE;
static esp_timer_handle_t s_persist_timer;
static QueueHandle_t s_button_queue;
static esp_timer_handle_t s_button_long_timer;
static esp_timer_handle_t s_button_dim_timer;
static bool s_button_long_active = false;
static int8_t s_button_dim_direction = 1;
static volatile int64_t s_button_last_isr_us = 0;

typedef enum
{
    BUTTON_EVENT_PRESS,
    BUTTON_EVENT_RELEASE,
    BUTTON_EVENT_LONG_START,
    BUTTON_EVENT_DIM_STEP,
} button_event_t;

static void persist_state_now(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("light", NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    if (nvs_set_u8(handle, "power", s_persist_power ? 1 : 0) != ESP_OK ||
        nvs_set_u8(handle, "level", s_persist_level) != ESP_OK ||
        nvs_set_u16(handle, "ct", s_persist_ct) != ESP_OK ||
        nvs_commit(handle) != ESP_OK)
    {
        ESP_LOGW(TAG, "NVS write failed");
    }
    nvs_close(handle);
}

static void persist_timer_cb(void *arg)
{
    (void)arg;
    persist_state_now();
}

static void schedule_persist(void)
{
    if (s_persist_timer)
    {
        esp_timer_stop(s_persist_timer);
        esp_timer_start_once(s_persist_timer, 10000000);
    }
}

static void load_persisted_state(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("light", NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        return;
    }
    uint8_t power = 0;
    uint8_t level = 0;
    uint16_t ct = 0;
    if (nvs_get_u8(handle, "power", &power) == ESP_OK)
    {
        s_persist_power = power ? true : false;
    }
    if (nvs_get_u8(handle, "level", &level) == ESP_OK)
    {
        s_persist_level = level;
    }
    if (nvs_get_u16(handle, "ct", &ct) == ESP_OK)
    {
        s_persist_ct = ct;
    }
    nvs_close(handle);
}

static void zigbee_set_onoff(bool on)
{
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(100)))
    {
        esp_zb_zcl_set_attribute_val(HA_COLOR_TEMPERATURE_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &on, false);
        esp_zb_lock_release();
    }
    light_driver_set_power(on);
    s_persist_power = on;
    schedule_persist();
}

static void zigbee_set_level(uint8_t level)
{
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(100)))
    {
        esp_zb_zcl_set_attribute_val(HA_COLOR_TEMPERATURE_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID, &level, false);
        esp_zb_lock_release();
    }
    light_driver_set_level(level);
    s_persist_level = level;
    schedule_persist();
}

static void button_long_timer_cb(void *arg)
{
    (void)arg;
    button_event_t event = BUTTON_EVENT_LONG_START;
    if (s_button_queue)
    {
        xQueueSend(s_button_queue, &event, 0);
    }
}

static void button_dim_timer_cb(void *arg)
{
    (void)arg;
    button_event_t event = BUTTON_EVENT_DIM_STEP;
    if (s_button_queue)
    {
        xQueueSend(s_button_queue, &event, 0);
    }
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    (void)arg;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_button_last_isr_us < (int64_t)BUTTON_DEBOUNCE_MS * 1000)
    {
        return;
    }
    s_button_last_isr_us = now_us;
    int level = gpio_get_level(BUTTON_INPUT_GPIO);
    button_event_t event = (level == BUTTON_ACTIVE_LEVEL) ? BUTTON_EVENT_PRESS : BUTTON_EVENT_RELEASE;
    BaseType_t higher_wakeup = pdFALSE;
    if (s_button_queue)
    {
        xQueueSendFromISR(s_button_queue, &event, &higher_wakeup);
    }
    if (higher_wakeup)
    {
        portYIELD_FROM_ISR();
    }
}

static void button_task(void *arg)
{
    (void)arg;
    button_event_t event;
    while (xQueueReceive(s_button_queue, &event, portMAX_DELAY))
    {
        switch (event)
        {
        case BUTTON_EVENT_PRESS:
            s_button_long_active = false;
            esp_timer_stop(s_button_long_timer);
            esp_timer_start_once(s_button_long_timer, BUTTON_LONG_PRESS_MS * 1000);
            break;
        case BUTTON_EVENT_RELEASE:
            esp_timer_stop(s_button_long_timer);
            esp_timer_stop(s_button_dim_timer);
            if (!s_button_long_active)
            {
                zigbee_set_onoff(!s_persist_power);
            }
            s_button_long_active = false;
            break;
        case BUTTON_EVENT_LONG_START:
            s_button_long_active = true;
            if (!s_persist_power)
            {
                zigbee_set_onoff(true);
            }
            esp_timer_start_periodic(s_button_dim_timer, BUTTON_DIM_INTERVAL_MS * 1000);
            break;
        case BUTTON_EVENT_DIM_STEP:
        {
            if (!s_button_long_active)
            {
                break;
            }
            int level = s_persist_level + (BUTTON_DIM_STEP * s_button_dim_direction);
            if (level <= 0)
            {
                level = 0;
                s_button_dim_direction = 1;
            }
            else if (level >= 255)
            {
                level = 255;
                s_button_dim_direction = -1;
            }
            zigbee_set_level((uint8_t)level);
            if (level == 0 && s_persist_power)
            {
                zigbee_set_onoff(false);
            }
            else if (level > 0 && !s_persist_power)
            {
                zigbee_set_onoff(true);
            }
            break;
        }
        default:
            break;
        }
    }
}

static void button_init(void)
{
    s_button_queue = xQueueCreate(8, sizeof(button_event_t));
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BUTTON_INPUT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (BUTTON_ACTIVE_LEVEL != 0) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(isr_err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_INPUT_GPIO, button_isr_handler, NULL));

    esp_timer_create_args_t long_args = {
        .callback = button_long_timer_cb,
        .name = "button_long",
    };
    esp_timer_create_args_t dim_args = {
        .callback = button_dim_timer_cb,
        .name = "button_dim",
    };
    ESP_ERROR_CHECK(esp_timer_create(&long_args, &s_button_long_timer));
    ESP_ERROR_CHECK(esp_timer_create(&dim_args, &s_button_dim_timer));

    xTaskCreate(button_task, "button_task", 3072, NULL, 4, NULL);
}
static esp_err_t deferred_driver_init(void)
{
    light_driver_init(s_persist_power ? LIGHT_DEFAULT_ON : LIGHT_DEFAULT_OFF);
    light_driver_set_level(s_persist_level);
    light_driver_set_color_temperature(s_persist_ct);
    return ESP_OK;
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type)
    {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK)
        {
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new())
            {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            }
            else
            {
                ESP_LOGI(TAG, "Device rebooted");
            }
        }
        else
        {
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK)
        {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
        }
        else
        {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (err_status == ESP_OK)
        {
            if (*(uint8_t *)esp_zb_app_signal_get_params(p_sg_p))
            {
                ESP_LOGI(TAG, "Network(0x%04hx) is open for %d seconds", esp_zb_get_pan_id(), *(uint8_t *)esp_zb_app_signal_get_params(p_sg_p));
            }
            else
            {
                ESP_LOGW(TAG, "Network(0x%04hx) closed, devices joining not allowed.", esp_zb_get_pan_id());
            }
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        // https://github.com/espressif/esp-zigbee-sdk/issues/66#issuecomment-1667314481
        esp_zb_zdo_signal_leave_params_t *leave_params = (esp_zb_zdo_signal_leave_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        if (leave_params)
        {
            if (leave_params->leave_type == ESP_ZB_NWK_LEAVE_TYPE_RESET)
            {
                ESP_LOGI(TAG, "ZDO leave: with reset, status: %s", esp_err_to_name(err_status));
                esp_zb_nvram_erase_at_start(true);                                          // erase previous network information.
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING); // steering a new network.
            }
            else
            {
                ESP_LOGI(TAG, "ZDO leave: leave_type: %d, status: %s", leave_params->leave_type, esp_err_to_name(err_status));
            }
        }
        else
        {
            ESP_LOGI(TAG, "ZDO leave: (no params), status: %s", esp_err_to_name(err_status));
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    bool light_state = 0;
    uint8_t light_level = 0;
    uint16_t light_color_x = 0;
    uint16_t light_color_y = 0;
    uint16_t light_color_temperature = 0;
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)",
                        message->info.status);
    ESP_LOGI(TAG, "Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)", message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.size);
    if (message->info.dst_endpoint == HA_COLOR_TEMPERATURE_LIGHT_ENDPOINT)
    {
        switch (message->info.cluster)
        {
        case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL)
            {
                light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : light_state;
                ESP_LOGI(TAG, "Light sets to %s", light_state ? "On" : "Off");
                light_driver_set_power(light_state);
                s_persist_power = light_state;
                schedule_persist();
            }
            else
            {
                ESP_LOGW(TAG, "On/Off cluster data: attribute(0x%x), type(0x%x)", message->attribute.id, message->attribute.data.type);
            }
            break;
        case ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID &&
                message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16)
            {
                light_color_temperature = message->attribute.data.value ? *(uint16_t *)message->attribute.data.value : light_color_temperature;
                ESP_LOGI(TAG, "Light color temperature changes to 0x%x mireds", light_color_temperature);
                light_driver_set_color_temperature(light_color_temperature);
                s_persist_ct = light_color_temperature;
                schedule_persist();
                break;
            }
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16)
            {
                light_color_x = message->attribute.data.value ? *(uint16_t *)message->attribute.data.value : light_color_x;
                light_color_y = *(uint16_t *)esp_zb_zcl_get_attribute(message->info.dst_endpoint, message->info.cluster,
                                                                      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID)
                                     ->data_p;
                ESP_LOGI(TAG, "Light color x changes to 0x%x", light_color_x);
                light_driver_set_color_xy(light_color_x, light_color_y);
            }
            else if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID &&
                     message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16)
            {
                light_color_y = message->attribute.data.value ? *(uint16_t *)message->attribute.data.value : light_color_y;
                light_color_x = *(uint16_t *)esp_zb_zcl_get_attribute(message->info.dst_endpoint, message->info.cluster,
                                                                      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID)
                                     ->data_p;
                ESP_LOGI(TAG, "Light color y changes to 0x%x", light_color_y);
                light_driver_set_color_xy(light_color_x, light_color_y);
            }
            else
            {
                ESP_LOGW(TAG, "Color control cluster data: attribute(0x%x), type(0x%x)", message->attribute.id, message->attribute.data.type);
            }
            break;
        case ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL:
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8)
            {
                light_level = message->attribute.data.value ? *(uint8_t *)message->attribute.data.value : light_level;
                light_driver_set_level((uint8_t)light_level);
                ESP_LOGI(TAG, "Light level changes to %d", light_level);
                s_persist_level = light_level;
                schedule_persist();
            }
            else
            {
                ESP_LOGW(TAG, "Level Control cluster data: attribute(0x%x), type(0x%x)", message->attribute.id, message->attribute.data.type);
            }
            break;
        default:
            ESP_LOGI(TAG, "Message data: cluster(0x%x), attribute(0x%x)  ", message->info.cluster, message->attribute.id);
        }
    }
    return ret;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id)
    {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ret;
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    // allow joining the Philips Hue network(s)
    esp_zb_enable_joining_to_distributed(true);
    uint8_t secret_zll_trust_center_key[] = {
        // FIXME: this is not the correct key, replace it with the proper one
        // 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        // 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
        // ORIGINAL:
        // FIXME: this is not the correct key, replace it with the proper one
        // 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        // 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
        // ZLL Commissioning trust centre link key for Hue
        // Source: https://peeveeone.com/2016/11/breakout-breakthrough/
        // 81 42 86 86 5D C1 C8 B2 C8 CB C5 2E 5D 65 D1 B8
        0x81, 0x42, 0x86, 0x86, 0x5D, 0xC1, 0xC8, 0xB2,
        0xC8, 0xCB, 0xC5, 0x2E, 0x5D, 0x65, 0xD1, 0xB8};
    esp_zb_secur_TC_standard_distributed_key_set(secret_zll_trust_center_key);

esp_zb_color_dimmable_light_cfg_t light_cfg = ESP_ZB_DEFAULT_COLOR_DIMMABLE_LIGHT_CONFIG();
light_cfg.color_cfg.color_capabilities = (1 << 4); // Color temperature only
light_cfg.color_cfg.color_mode = 0x02;             // Color temperature mode
light_cfg.color_cfg.enhanced_color_mode = 0x02;    // Enhanced color temperature mode
esp_zb_ep_list_t *esp_zb_color_temperature_light_ep = NULL;
esp_zb_color_temperature_light_ep = esp_zb_ep_list_create();
esp_zb_endpoint_config_t endpoint_config = {
    .endpoint = HA_COLOR_TEMPERATURE_LIGHT_ENDPOINT,
    .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
    .app_device_id = ESP_ZB_HA_COLOR_TEMPERATURE_LIGHT_DEVICE_ID,
    .app_device_version = 1, // maybe important for Hue? Oh HELL yes.
};
esp_zb_ep_list_add_ep(esp_zb_color_temperature_light_ep, esp_zb_color_dimmable_light_clusters_create(&light_cfg), endpoint_config);
zcl_basic_manufacturer_info_t info = {
    .manufacturer_name = ESP_MANUFACTURER_NAME,
    .model_identifier = ESP_MODEL_IDENTIFIER,
};

esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_color_temperature_light_ep, HA_COLOR_TEMPERATURE_LIGHT_ENDPOINT, &info);

// https://github.com/espressif/esp-zigbee-sdk/issues/457#issuecomment-2426128314
uint16_t on_off_on_time = 0;
bool on_off_global_scene_control = 0;
esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(esp_zb_color_temperature_light_ep, HA_COLOR_TEMPERATURE_LIGHT_ENDPOINT);
esp_zb_attribute_list_t *onoff_attr_list =
    esp_zb_cluster_list_get_cluster(cluster_list, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

esp_zb_on_off_cluster_add_attr(onoff_attr_list, ESP_ZB_ZCL_ATTR_ON_OFF_ON_TIME, &on_off_on_time);
esp_zb_on_off_cluster_add_attr(onoff_attr_list, ESP_ZB_ZCL_ATTR_ON_OFF_GLOBAL_SCENE_CONTROL,
                               &on_off_global_scene_control);
esp_zb_attribute_list_t *color_attr_list =
    esp_zb_cluster_list_get_cluster(cluster_list, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
if (esp_zb_color_control_cluster_add_attr(color_attr_list, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &s_color_temperature) != ESP_OK)
{
    ESP_LOGW(TAG, "Failed to add color temperature attribute");
}
if (esp_zb_color_control_cluster_add_attr(color_attr_list, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID, &s_color_temp_min) != ESP_OK)
{
    ESP_LOGW(TAG, "Failed to add color temperature min attribute");
}
if (esp_zb_color_control_cluster_add_attr(color_attr_list, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID, &s_color_temp_max) != ESP_OK)
{
    ESP_LOGW(TAG, "Failed to add color temperature max attribute");
}
if (esp_zb_color_control_cluster_add_attr(color_attr_list, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COUPLE_COLOR_TEMP_TO_LEVEL_MIN_MIREDS_ID,
                                          &s_color_temp_couple_level_min) != ESP_OK)
{
    ESP_LOGW(TAG, "Failed to add color temperature couple-to-level attribute");
}
if (esp_zb_color_control_cluster_add_attr(color_attr_list, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_START_UP_COLOR_TEMPERATURE_MIREDS_ID, &s_startup_color_temp) != ESP_OK)
{
    ESP_LOGW(TAG, "Failed to add startup color temperature attribute");
}
// .

esp_zb_device_register(esp_zb_color_temperature_light_ep);
esp_zb_core_action_handler_register(zb_action_handler);
esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
ESP_ERROR_CHECK(esp_zb_start(false));
esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    load_persisted_state();
    esp_timer_create_args_t timer_args = {
        .callback = persist_timer_cb,
        .name = "light_persist",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_persist_timer));
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    button_init();
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
