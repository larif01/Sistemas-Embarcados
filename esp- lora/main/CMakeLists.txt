idf_component_register(
  SRCS "hello_world_main.c"
  INCLUDE_DIRS "."
  PRIV_REQUIRES esp_adc driver
  REQUIRES esp_event esp_netif esp_wifi esp_http_client nvs_flash esp-tls mbedtls lora
)
