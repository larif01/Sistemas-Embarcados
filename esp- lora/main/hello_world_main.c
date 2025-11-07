#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "lora.h"

/* ================== Constantes/Config ================== */
static const char *TAG       = "MAIN";
static const char *HTTP_TAG  = "HTTP_POST";

#define PIN_DIO0 26

/* Wi-Fi + HTTP */
#define WIFI_SSID     "wifi"
#define WIFI_PASS     "senha"
#define HTTP_ENDPOINT "https://tf7vk6z6-8000.brs.devtunnels.ms/api/leituras/"  

/* ADC (ESP32: usando ADC1_CH6 = GPIO34. OBS: ADC2 conflita com Wi‑Fi) */
#define ADC_UNIT_USED    ADC_UNIT_1
#define ADC_CHANNEL_USED ADC_CHANNEL_6   // ADC1_CH6 = GPIO34
#define ADC_ATTEN_USED   ADC_ATTEN_DB_11

#define SAMPLES            64
#define TEMP_C             25.0f

/* EventGroup para Wi-Fi */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_GOT_IP_BIT    BIT1

/* ADC handles */
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static bool s_cali_enabled = false;

/* Fila para desacoplar RX (LoRa) do HTTP (TLS) */
typedef struct { char *body; size_t len; } HttpMsg;
static QueueHandle_t http_q = NULL; // capacidade definida no app_main

/* ================== Wi-Fi ================== */
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t cfg = { 0 };
    // Use snprintf para garantir NUL-termination
    snprintf((char*)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", WIFI_SSID);
    snprintf((char*)cfg.sta.password, sizeof(cfg.sta.password), "%s", WIFI_PASS);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // ajuste conforme sua rede

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    if ((bits & WIFI_GOT_IP_BIT) == 0) {
        ESP_LOGE(TAG, "Wi‑Fi não obteve IP (timeout). HTTP pode falhar.");
    } else {
        ESP_LOGI(TAG, "Wi‑Fi pronto (com IP).");
    }
}

/* ================== HTTP (task dedicada) ================== */
static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        ESP_LOGD(HTTP_TAG, "Body chunk (%d bytes)", evt->data_len);
    }
    return ESP_OK;
}

static void task_http(void *arg)
{
    // Aguarda Wi‑Fi com IP
    if (s_wifi_event_group) {
        xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    }

    esp_http_client_config_t config = {
        .url = HTTP_ENDPOINT,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .event_handler = http_evt,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        // Buffers um pouco maiores ajudam com TLS/chunking
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(HTTP_TAG, "esp_http_client_init falhou");
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", "application/json"));

    for (;;) {
        HttpMsg msg;
        if (xQueueReceive(http_q, &msg, portMAX_DELAY)) {
            // Seta o corpo da requisição
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_post_field(client, msg.body, (int)msg.len));

            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                int status = esp_http_client_get_status_code(client);
                ESP_LOGI(HTTP_TAG, "POST %s -> HTTP %d", HTTP_ENDPOINT, status);
            } else {
                ESP_LOGE(HTTP_TAG, "Erro no POST: %s", esp_err_to_name(err));
            }

            free(msg.body);

            // Telemetria de pilha (ajuda a calibrar tamanhos)
            UBaseType_t wm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGD(HTTP_TAG, "HighWaterMark HTTP: %u words (~%u bytes)", wm, (unsigned)(wm * sizeof(StackType_t)));
        }
    }
}

/* ================== ADC (Oneshot + Calib) ================== */
static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_USED,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,   // 12 bits
        .atten    = ADC_ATTEN_USED,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_USED, &chan_cfg));

    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_USED,
        .atten    = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali_handle) == ESP_OK) {
        s_cali_enabled = true;
        ESP_LOGI(TAG, "ADC calibração habilitada (line fitting).");
    } else {
        s_cali_enabled = false;
        ESP_LOGW(TAG, "ADC calibração indisponível (segue sem).");
    }
}

static esp_err_t adc_read_mv(int *out_mv)
{
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, ADC_CHANNEL_USED, &raw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc read fail: %s", esp_err_to_name(err));
        return err;
    }
    if (s_cali_enabled) {
        int mv = 0;
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_cali_handle, raw, &mv));
        *out_mv = mv;
    } else {
        // fallback simples (aproximação para 11dB ~ 3.3V)
        *out_mv = (int)((raw * 3300) / 4095);
    }
    return ESP_OK;
}

/* ================== Tasks LoRa/Medida ================== */
#if CONFIG_SENDER
static void task_tx(void *pv)
{
    ESP_LOGI(pcTaskGetName(NULL), "Start");

    // aguarda Wi‑Fi com IP (segurança extra)
    if (s_wifi_event_group) {
        xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    }

    uint8_t buf[256];

    for (;;) {
        // Média de SAMPLES leituras (mV)
        uint32_t acc_mv = 0;
        for (int i = 0; i < SAMPLES; i++) {
            int mv = 0;
            if (adc_read_mv(&mv) == ESP_OK) acc_mv += (uint32_t)mv;
        }
        uint32_t voltage = acc_mv / SAMPLES;
        float voltage_v = voltage / 1000.0f;

        // Compensação de temperatura (coef ~2%/°C relativo a 25°C)
        float tempCoef = 1.0f + 0.02f * (TEMP_C - 25.0f);
        float voltage_comp = voltage_v / tempCoef;

        // TDS (ppm) — modelo DFRobot ajustado
        float tds_value = (133.42f * voltage_comp * voltage_comp * voltage_comp
                        - 255.86f * voltage_comp * voltage_comp
                        + 857.39f * voltage_comp) * 0.5f;

        printf("ADC(mV): %lu | %.3f V | TDS: %.2f ppm (T=%.1f °C)\n",
               (unsigned long)voltage, voltage_v, tds_value, (double)TEMP_C);

        // JSON para LoRa
        int send_len = snprintf((char*)buf, sizeof(buf),
                                "{\"sensor_id\":1,\"valor\":%.2f,\"tipo\":\"SALI\"}",
                                tds_value);

        // LoRa
        lora_send_packet(buf, send_len);
        ESP_LOGI(pcTaskGetName(NULL), "%d byte packet sent (LoRa)...", send_len);
        int lost = lora_packet_lost();
        if (lost != 0) {
            ESP_LOGW(pcTaskGetName(NULL), "%d packets lost", lost);
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
#endif

#if CONFIG_RECEIVER
static void task_rx(void *pv)
{
    ESP_LOGI(pcTaskGetName(NULL), "Start");
    uint8_t buf[256];

    for (;;) {
        lora_receive(); // modo RX
        if (lora_received()) {
            int rxLen = lora_receive_packet(buf, sizeof(buf));
            if (rxLen > 0) {
                size_t safe = (rxLen < sizeof(buf)) ? (size_t)rxLen : (sizeof(buf) - 1);
                buf[safe] = '\0';

                ESP_LOGI(pcTaskGetName(NULL), "%d byte packet received:[%.*s]", rxLen, (int)safe, buf);

                // Copia para heap e envia à fila HTTP
                char *body = (char*)malloc(safe + 1);
                if (body) {
                    memcpy(body, buf, safe);
                    body[safe] = '\0';
                    HttpMsg msg = { .body = body, .len = safe };
                    if (xQueueSend(http_q, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                        ESP_LOGW(HTTP_TAG, "Fila HTTP cheia — descartando payload");
                        free(body);
                    }
                } else {
                    ESP_LOGE(TAG, "malloc falhou para body HTTP");
                }

                // Telemetria de pilha
                UBaseType_t wm = uxTaskGetStackHighWaterMark(NULL);
                ESP_LOGD("RX", "HighWaterMark RX: %u words (~%u bytes)", wm, (unsigned)(wm * sizeof(StackType_t)));
            }
        }
        vTaskDelay(1);
    }
}
#endif

/* ================== app_main ================== */
void app_main(void)
{
    // NVS para Wi‑Fi
    ESP_ERROR_CHECK(nvs_flash_init());

    // Wi‑Fi primeiro (evita assert do LWIP no POST)
    wifi_init_sta();

    // LoRa init/config
    if (lora_init() == 0) {
        ESP_LOGE(TAG, "Módulo LoRa não reconhecido");
        while (1) vTaskDelay(1);
    }
    lora_explicit_header_mode();
    lora_set_sync_word(0x34);
    lora_enable_crc();

#if CONFIG_433MHZ
    ESP_LOGI(TAG, "Frequency is 433MHz");
    lora_set_frequency(433e6);
#elif CONFIG_866MHZ
    ESP_LOGI(TAG, "Frequency is 866MHz");
    lora_set_frequency(866e6);
#elif CONFIG_915MHZ
    ESP_LOGI(TAG, "Frequency is 915MHz");
    lora_set_frequency(915e6);
#elif CONFIG_OTHER
    ESP_LOGI(TAG, "Frequency is %dMHz", CONFIG_OTHER_FREQUENCY);
    long frequency = CONFIG_OTHER_FREQUENCY * 1000000;
    lora_set_frequency(frequency);
#endif

    int cr = 1, bw = 7, sf = 7;
#if CONFIG_ADVANCED
    cr = CONFIG_CODING_RATE;
    bw = CONFIG_BANDWIDTH;
    sf = CONFIG_SF_RATE;
#endif
    lora_set_coding_rate(cr);
    lora_set_bandwidth(bw);
    lora_set_spreading_factor(sf);

    ESP_LOGI(TAG, "coding_rate=%d", cr);
    ESP_LOGI(TAG, "bandwidth=%d", bw);
    ESP_LOGI(TAG, "spreading_factor=%d", sf);

    // ADC
    adc_init();

    // Fila HTTP (até 10 mensagens pendentes)
    http_q = xQueueCreate(10, sizeof(HttpMsg));
    configASSERT(http_q != NULL);

#if CONFIG_SENDER
    xTaskCreatePinnedToCore(task_tx,   "TX",    6 * 1024,  NULL, 5, NULL, tskNO_AFFINITY);
#endif
#if CONFIG_RECEIVER
    // RX pode ser menor agora que não faz TLS
    xTaskCreatePinnedToCore(task_rx,   "RX",    6 * 1024,  NULL, 5, NULL, tskNO_AFFINITY);
    // HTTP precisa de pilha generosa por causa do mbedTLS
    xTaskCreatePinnedToCore(task_http, "HTTP", 16 * 1024,  NULL, 5, NULL, tskNO_AFFINITY);
#endif
}
