/*******************************************************************************
 * 
 * ttn-esp32 - The Things Network device library for ESP-IDF / SX127x
 * 
 * Sample program (ESP-IDF only) — envia JSON por HTTP ao receber downlink.
 *******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_client.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "nvs_flash.h"

#include "ttn.h"

// ===== Credenciais TTN (copie da console) =====
static const char *appEui = "9825cb8cc090d6e2";
static const char *devEui = "fb9a1c09942c20e2";
static const char *appKey = "8b8836912f9223c37987daa6ee49fa31";

// ===== Pinos SX127x =====
#define TTN_SPI_HOST      SPI2_HOST
#define TTN_SPI_DMA_CHAN  SPI_DMA_DISABLED
#define TTN_PIN_SPI_SCLK  5
#define TTN_PIN_SPI_MOSI  27
#define TTN_PIN_SPI_MISO  19
#define TTN_PIN_NSS       18
#define TTN_PIN_RXTX      TTN_NOT_CONNECTED
#define TTN_PIN_RST       14
#define TTN_PIN_DIO0      26
#define TTN_PIN_DIO1      35

// ===== Uplink periódico =====
#define TX_INTERVAL 30
static uint8_t msgData[] = "Hello, world";

// ===== Wi-Fi + HTTP =====
#define WIFI_SSID     "SEU_SSID"
#define WIFI_PASS     "SUA_SENHA"
#define HTTP_ENDPOINT "http://192.168.0.100:8080/lora"

static const char *TAG = "TTN_HTTP";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// ---------- Util: bytes -> HEX ----------
static char* bytes_to_hex(const uint8_t *data, size_t len) {
    char *hex = (char*)malloc(len * 2 + 1);
    if (!hex) return NULL;
    for (size_t i = 0; i < len; i) sprintf(hex + (i * 2), "%02X", data[i]);
    hex[len * 2] = '\0';
    return hex;
}

// ---------- HTTP POST JSON ----------
static bool http_post_json(const char *url, const char *json_body) {
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 7000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init falhou");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, strlen(json_body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_open falhou: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int wrote = esp_http_client_write(client, json_body, strlen(json_body));
    if (wrote < 0) {
        ESP_LOGE(TAG, "http_write falhou");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    char buf[256];
    int r = esp_http_client_read(client, buf, sizeof(buf) - 1);
    if (r > 0) {
        buf[r] = '\0';
        ESP_LOGI(TAG, "HTTP %d, resp: %s", status, buf);
    } else {
        ESP_LOGI(TAG, "HTTP %d", status);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status >= 200 && status < 300;
}

// ---------- Wi-Fi ----------
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi desconectado; tentando reconectar...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*) data;
        ESP_LOGI(TAG, "Wi-Fi OK, IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_and_connect(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t cfg = { 0 };
    strncpy((char*)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid)-1);
    strncpy((char*)cfg.sta.password, WIFI_PASS, sizeof(cfg.sta.password)-1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // espera até conectar (timeout curto; reconexão fica no background)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Wi-Fi não conectou no timeout; seguirá tentando.");
    }
}

// ---------- Uplink periódico ----------
static void sendMessages(void* pvParameter) {
    while (1) {
        ESP_LOGI(TAG, "Sending message...");
        ttn_response_code_t res = ttn_transmit_message(msgData, sizeof(msgData) - 1, /*port*/1, /*confirmed*/false);
        ESP_LOGI(TAG, (res == TTN_SUCCESSFUL_TRANSMISSION) ? "Message sent." : "Transmission failed.");
        vTaskDelay(TX_INTERVAL * pdMS_TO_TICKS(1000));
    }
}

// ---------- Downlink -> POST JSON ----------
static void messageReceived(const uint8_t* message, size_t length, ttn_port_t port) {
    ESP_LOGI(TAG, "Downlink: %zu bytes on port %u", length, (unsigned)port);
    for (size_t i = 0; i < length; i++) printf(" %02X", message[i]);
    printf("\n");

    char *hex = bytes_to_hex(message, length);
    if (!hex) {
        ESP_LOGE(TAG, "Sem memória para HEX");
        return;
    }

    // Exemplo de JSON esperado pelo seu backend
    // {"sensor_id":1,"valor":"A1B2...","tipo":"SALI"}
    char json[256 + 2*length];
    int n = snprintf(json, sizeof(json),
                     "{\"sensor_id\":1,\"valor\":\"%s\",\"tipo\":\"SALI\"}", hex);
    free(hex);

    if (n < 0 || n >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "JSON maior que buffer");
        return;
    }

    if (!http_post_json(HTTP_ENDPOINT, json)) {
        ESP_LOGW(TAG, "Falha ao POSTar JSON");
    }
}

void app_main(void) {
    // GPIO ISR (algumas libs usam)
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));

    // NVS (Wi-Fi + TTN)
    ESP_ERROR_CHECK(nvs_flash_init());

    // SPI para SX127x
    spi_bus_config_t spi_bus_config = {
        .miso_io_num = TTN_PIN_SPI_MISO,
        .mosi_io_num = TTN_PIN_SPI_MOSI,
        .sclk_io_num = TTN_PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TTN_SPI_HOST, &spi_bus_config, TTN_SPI_DMA_CHAN));

    // TTN
    ttn_init();
    ttn_configure_pins(TTN_SPI_HOST, TTN_PIN_NSS, TTN_PIN_RXTX, TTN_PIN_RST, TTN_PIN_DIO0, TTN_PIN_DIO1);
    ttn_provision(devEui, appEui, appKey); // depois da 1ª execução pode comentar
    ttn_on_message(messageReceived);
    // ttn_set_adr_enabled(false);
    // ttn_set_data_rate(TTN_DR_US915_SF7);
    ttn_set_max_tx_pow(20);

    // Wi-Fi para poder enviar o JSON no downlink
    wifi_init_and_connect();

    printf("Joining...\n");
    if (ttn_join()) {
        printf("Joined.\n");
        xTaskCreate(sendMessages, "send_messages", 4096, NULL, 3, NULL);
    } else {
        printf("Join failed. Goodbye\n");
    }
}
