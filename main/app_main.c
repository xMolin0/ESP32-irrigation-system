#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "dht.h"

#define SENSOR_TYPE DHT_TYPE_DHT11
#define SENSOR_GPIO GPIO_NUM_4
#define WPUMP_GPIO GPIO_NUM_7 
#define SOIL_SENSOR_GPIO GPIO_NUM_3

#define DRY_VALUE 4090
#define WET_VALUE 1725


TaskHandle_t helloWorldTaskHandel = NULL;
TaskHandle_t soilTaskHandel = NULL;
static adc_oneshot_unit_handle_t soil_adc_handle = NULL;
static adc_channel_t soil_adc_channel;
static esp_mqtt_client_handle_t mqtt_client = NULL;


static const char *TAG = "mqtts_example";

extern const uint8_t client_cert_pem_start[] asm("_binary_client_crt_start");
extern const uint8_t client_cert_pem_end[] asm("_binary_client_crt_end");
extern const uint8_t client_key_pem_start[] asm("_binary_client_key_start");
extern const uint8_t client_key_pem_end[] asm("_binary_client_key_end");
extern const uint8_t server_cert_pem_start[] asm("_binary_amazon_com_crt_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_amazon_com_crt_end");


static void init_soil_adc(void)
{
    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &soil_adc_handle));

    adc_unit_t unit_id;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(SOIL_SENSOR_GPIO, &unit_id, &soil_adc_channel));

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(soil_adc_handle, soil_adc_channel, &chan_cfg));
}

static void suspend_sensor_tasks(void)
{
    if (helloWorldTaskHandel != NULL && eTaskGetState(helloWorldTaskHandel) != eSuspended)
    {
        vTaskSuspend(helloWorldTaskHandel);
    }

    if (soilTaskHandel != NULL && eTaskGetState(soilTaskHandel) != eSuspended)
    {
        vTaskSuspend(soilTaskHandel);
    }
}

void dhtTask(void *arg)
{
    float temperature, humidity;

    while (1)
    {
        esp_err_t dht_err = dht_read_float_data(SENSOR_TYPE, SENSOR_GPIO, &humidity, &temperature);
        if (dht_err != ESP_OK)
        {
            ESP_LOGE(TAG, "DHT read failed: %s", esp_err_to_name(dht_err));
        }
        else
        {
            ESP_LOGI(TAG, "DHT read OK: humidity=%.1f%%, temperature=%.1f°C", humidity, temperature);
            
            char payload[100];

            snprintf(payload, sizeof(payload),
                        "{\"temperature\": %.1f, \"humidity\": %.1f}",
                        temperature, humidity);

            esp_mqtt_client_publish(mqtt_client, "sensor/123", payload, 0, 0, 0);
            
        }

        vTaskDelay(30000 / portTICK_PERIOD_MS);
    }
}

void soilTask(void *arg)
{
    while (1)
    {
        int raw_value = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(soil_adc_handle, soil_adc_channel, &raw_value));
        // Convert ADC raw value to millivolts (Vmax=3100mV for ADC_ATTEN_DB_11, dmax=4095 for 12-bit)
        int voltage_mv = (raw_value * 3100) / 4095;
        ESP_LOGI(TAG, "Soil moisture raw: %d, voltage: %d mV", raw_value, voltage_mv);

        
        float moisture_percent = 100.0f * (DRY_VALUE - raw_value) / (DRY_VALUE - WET_VALUE);
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"soil_moisture\": %.2f}", moisture_percent);
        esp_mqtt_client_publish(mqtt_client, "sensor/soil/123", payload, 0, 0, 0);
    

        vTaskDelay(30000 / portTICK_PERIOD_MS);
    }
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static bool string_equals(const char *data, int len, const char *expected)
{
    size_t expected_len = strlen(expected);
    return len == (int)expected_len && strncmp(data, expected, len) == 0;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);

    esp_mqtt_event_handle_t event = event_data;
    int msg_id = 0;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");


        msg_id = esp_mqtt_client_subscribe(mqtt_client, "led/123", 0);
        ESP_LOGI(TAG, "subscribed to led/123, msg_id=%d", msg_id);

    
        vTaskResume(helloWorldTaskHandel);
        vTaskResume(soilTaskHandel);

        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");

        suspend_sensor_tasks();
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");

        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);

        if (string_equals(event->topic, event->topic_len, "led/123"))
        {
            if (string_equals(event->data, event->data_len, "on"))
            {
                gpio_set_level(WPUMP_GPIO, 1);
                sys_delay_ms(3000); 
                gpio_set_level(WPUMP_GPIO, 0);
                printf("Water Pump ran for 3 seconds\n");
            }
            else if (string_equals(event->data, event->data_len, "off"))
            {
                gpio_set_level(WPUMP_GPIO, 0);
                printf("Water Pump turned OFF\n");
            }
            else
            {
                ESP_LOGE(TAG, "Invalid payload: expected 'on' or 'off'");
            }
        }

        break;

    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "MQTT_EVENT_BEFORE_CONNECT");
        break;

    case MQTT_EVENT_ERROR:

        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        suspend_sensor_tasks();

        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        else
        {
            ESP_LOGI(TAG, "Other error type: %d", event->error_handle->error_type);
        }

        break;

    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtts://a1lxg6ppn3un5p-ats.iot.us-east-1.amazonaws.com:8883",
        .broker.verification.certificate = (const char *)server_cert_pem_start,
        .credentials = {
            .authentication = {
                .certificate = (const char *)client_cert_pem_start,
                .key = (const char *)client_key_pem_start,
            },
            .client_id = "esp32_client",
        }};

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(example_connect());

    gpio_reset_pin(SENSOR_GPIO);
    gpio_set_direction(SENSOR_GPIO, GPIO_MODE_INPUT);

    gpio_reset_pin(WPUMP_GPIO);
    gpio_set_direction(WPUMP_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(WPUMP_GPIO, 0);

    init_soil_adc();

    mqtt_app_start();
    
    xTaskCreate(dhtTask, "DHT Task", 4096, NULL, 5, &helloWorldTaskHandel);
    xTaskCreate(soilTask, "Soil Task", 4096, NULL, 5, &soilTaskHandel);

    vTaskSuspend(helloWorldTaskHandel);
    vTaskSuspend(soilTaskHandel);

    
}