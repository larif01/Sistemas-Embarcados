#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_SCK  18
#define PIN_NUM_CS    5
#define PIN_NUM_RST  14
#define PIN_NUM_DIO0 26

static const char *TAG = "LORA_IDF";
static spi_device_handle_t lora_dev;

static void lora_write_reg(uint8_t addr, uint8_t val) {
    spi_transaction_t t = {0};
    uint8_t buf[2] = { (uint8_t)(addr | 0x80), val }; // bit7=1 => write
    t.length = 16;
    t.tx_buffer = buf;
    spi_device_polling_transmit(lora_dev, &t);
}

static uint8_t lora_read_reg(uint8_t addr) {
    spi_transaction_t t = {0};
    uint8_t tx[2] = { (uint8_t)(addr & 0x7F), 0x00 }; // bit7=0 => read
    uint8_t rx[2] = {0};
    t.length = 16;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    spi_device_polling_transmit(lora_dev, &t);
    return rx[1];
}

static void lora_reset(void) {
    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void lora_set_freq_915(void) {
    // Freq = 915 MHz -> FRF = freq * (2^19 / 32e6) ≈ 0xE4C000
    lora_write_reg(0x06, 0xE4); // FrfMsb
    lora_write_reg(0x07, 0xC0); // FrfMid
    lora_write_reg(0x08, 0x00); // FrfLsb
}

void app_main(void) {
    // SPI host
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

    // Dispositivo SX1276
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000, // 4 MHz
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 4,
    };
    spi_bus_add_device(SPI2_HOST, &devcfg, &lora_dev);

    // Reset e modo LoRa
    lora_reset();
    lora_write_reg(0x01, 0x80); // RegOpMode: Sleep + LoRa
    vTaskDelay(pdMS_TO_TICKS(10));
    lora_write_reg(0x01, 0x81); // Standby + LoRa

    // Freq 915 MHz, SF7, BW125k, CRC on (regs básicos)
    lora_set_freq_915();
    lora_write_reg(0x1D, 0x72); // RegModemConfig1: BW=125k (0x70), CR=4/5 (0x02)
    lora_write_reg(0x1E, 0x74); // RegModemConfig2: SF7 (0x70), RxCrcOn=1 (0x04)
    lora_write_reg(0x26, 0x04); // RegModemConfig3: LnaBoost off, LowDataRateOptimize off

    const char *msg = "Mensagem";
    uint8_t size = (uint8_t)strlen(msg);

    uint8_t fifoTxBase = 0x80;
    lora_write_reg(0x0E, fifoTxBase);     // RegFifoTxBaseAddr
    lora_write_reg(0x0D, fifoTxBase);     // RegFifoAddrPtr

    // Escreve bytes no FIFO (burst write: addr=0x00 com bit7=1)
    for (int i = 0; i < size; i++) lora_write_reg(0x00, (uint8_t)msg[i]);

    lora_write_reg(0x22, size);           // RegPayloadLength
    lora_write_reg(0x01, 0x83);           // RegOpMode: TX (LoRa)

    ESP_LOGI(TAG, "Pacote TX disparado.");
}
