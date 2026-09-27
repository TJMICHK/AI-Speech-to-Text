#include <driver/i2s.h>

#define I2S_SCK 26
#define I2S_WS  25
#define I2S_SD  33

#define I2S_PORT I2S_NUM_0

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Starting INMP441...");

  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);

  Serial.println("Mic ready.");
}

void loop() {

  int32_t samples[64];
  size_t bytesRead;

  i2s_read(
    I2S_PORT,
    samples,
    sizeof(samples),
    &bytesRead,
    portMAX_DELAY
  );

  int sampleCount = bytesRead / sizeof(int32_t);

  // Find peak amplitude in this block
  int32_t peak = 0;

  for (int i = 0; i < sampleCount; i++) {

    // INMP441 gives 24-bit samples inside 32-bit words
    int32_t sample = samples[i] >> 8;

    int32_t amplitude = abs(sample);

    if (amplitude > peak)
      peak = amplitude;
  }

  Serial.println(peak);
}