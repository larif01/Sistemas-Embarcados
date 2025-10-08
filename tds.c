#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"

// -----------------------------
// CONFIGURAÇÕES
// -----------------------------
#define TDS_PIN             ADC2_CHANNEL_0   // GPIO4 = ADC2_CH0
#define DEFAULT_VREF        1100             // Valor médio do Vref interno (mV)
#define SAMPLES             64               // Número de amostras para média
#define TEMP_C              25.0             // Temperatura padrão se não houver sensor (°C)

// -----------------------------
// FUNÇÃO PRINCIPAL
// -----------------------------
void app_main(void)
{
    esp_adc_cal_characteristics_t adc_chars;

    // Configura ADC2 canal 0 (GPIO4)
    adc2_config_channel_atten(TDS_PIN, ADC_ATTEN_DB_11); // até ~3.3V
    esp_adc_cal_characterize(ADC_UNIT_2, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12,
                             DEFAULT_VREF, &adc_chars);

    while (1)
    {
        uint32_t adc_reading = 0;

        // Média de múltiplas amostras para estabilidade
        for (int i = 0; i < SAMPLES; i++) {
            int raw;
            if (adc2_get_raw(TDS_PIN, ADC_WIDTH_BIT_12, &raw) == ESP_OK) {
                adc_reading += raw;
            }
        }
        adc_reading /= SAMPLES;

        // Converte leitura para milivolts
        uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, &adc_chars);
        float voltage_v = voltage / 1000.0; // mV -> V

        // -----------------------------
        // Compensação de temperatura
        // -----------------------------
        // Fórmula de compensação (25°C é referência)
        // TempCoef = 1 + 0.02*(Temp - 25)
        float tempCoef = 1.0 + 0.02 * (TEMP_C - 25.0);

        // Corrige tensão medida conforme temperatura
        float voltage_comp = voltage_v / tempCoef;

        // -----------------------------
        // Conversão para TDS (ppm)
        // Fórmula da DFRobot, ajustada
        // -----------------------------
        float tds_value = (133.42 * voltage_comp * voltage_comp * voltage_comp
                        - 255.86 * voltage_comp * voltage_comp
                        + 857.39 * voltage_comp) * 0.5;

        printf("ADC: %lu | %lu mV | %.2f V | TDS: %.2f ppm (T=%.1f°C)\n",
               adc_reading, voltage, voltage_v, tds_value, TEMP_C);

        vTaskDelay(pdMS_TO_TICKS(1000)); // atualiza a cada 1s
    }
}
