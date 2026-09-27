#include <driver/i2s.h>

// ============================================================
// I2S Microphone Configuration
// ============================================================

#define I2S_PORT I2S_NUM_0

#define SAMPLE_RATE        16000
#define SAMPLE_BUFFER_SIZE 512

// INMP441 connections
#define I2S_SCK GPIO_NUM_26   // BCLK / SCK
#define I2S_WS  GPIO_NUM_25   // WS / LRCLK
#define I2S_SD  GPIO_NUM_33   // Serial Data

// L/R pin on INMP441:
// GND  = Left channel
// 3.3V = Right channel
#define I2S_CHANNEL I2S_CHANNEL_FMT_ONLY_LEFT


// ============================================================
// I2S Configuration
// ============================================================

i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),

    .sample_rate = SAMPLE_RATE,

    // INMP441 sends 24-bit audio inside a 32-bit I2S frame
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,

    .channel_format = I2S_CHANNEL,

    .communication_format = I2S_COMM_FORMAT_I2S,

    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count = 4,
    .dma_buf_len = 512,

    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
};


// ============================================================
// I2S Pin Configuration
// ============================================================

i2s_pin_config_t i2s_pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,

    // ESP32 is only receiving audio
    .data_out_num = I2S_PIN_NO_CHANGE,

    .data_in_num = I2S_SD
};


// Raw DMA buffer
int32_t raw_samples[SAMPLE_BUFFER_SIZE];


// ============================================================
// Setup
// ============================================================

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println("Starting INMP441...");

    // Install I2S driver
    esp_err_t result = i2s_driver_install(
        I2S_PORT,
        &i2s_config,
        0,
        NULL
    );

    if (result != ESP_OK)
    {
        Serial.printf("I2S driver install failed: %d\n", result);

        while (true)
        {
            delay(1000);
        }
    }

    // Connect I2S peripheral to GPIO pins
    result = i2s_set_pin(I2S_PORT, &i2s_pins);

    if (result != ESP_OK)
    {
        Serial.printf("I2S pin configuration failed: %d\n", result);

        while (true)
        {
            delay(1000);
        }
    }

    Serial.println("INMP441 ready.");
}


// ============================================================
// Main Loop
// ============================================================

void loop()
{
    size_t bytes_read = 0;

    esp_err_t result = i2s_read(
        I2S_PORT,
        raw_samples,
        sizeof(raw_samples),
        &bytes_read,
        portMAX_DELAY
    );

    if (result != ESP_OK)
    {
        Serial.println("I2S read failed");
        return;
    }

    int samples_read = bytes_read / sizeof(int32_t);

    int32_t peak = 0;

    for (int i = 0; i < samples_read; i++)
    {
        // Convert 32-bit I2S frame to 24-bit microphone sample
        int32_t sample = raw_samples[i] >> 8;

        // Get absolute amplitude
        int32_t amplitude = abs(sample);

        // Find largest sample in this block
        if (amplitude > peak)
        {
            peak = amplitude;
        }
    }

    Serial.println(peak);

    delay(50);
}