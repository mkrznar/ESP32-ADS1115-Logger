// ws2812.c
// Implements a driver for WS2812 (NeoPixel) LEDs using the ESP32 RMT (Remote Control) peripheral.
// The RMT peripheral is ideal for generating the specific timed signals required by WS2812 LEDs.

#include "ws2812.h"        // Header for this WS2812 driver
#include "driver/rmt.h"    // ESP-IDF RMT peripheral library
#include "esp_log.h"       // ESP-IDF logging library
#include "freertos/task.h" // FreeRTOS for vTaskDelay

// Tag for ESP logging specific to the WS2812 driver.
#define TAG "WS2812"

// --- RMT Channel and GPIO Pin Configuration ---

// RMT channel to be used for transmitting data (TX).
#define RMT_TX_CHANNEL RMT_CHANNEL_0
// GPIO pin connected to the WS2812 data input.
#define RMT_TX_GPIO    48

// Static array to hold 24 RMT items, encoding one WS2812 LED's GRB color data.
// Each rmt_item32_t represents a single bit (0 or 1) according to WS2812 protocol timing.
static rmt_item32_t ws2812_bits[24];

/**
 * @brief Initializes the RMT peripheral for WS2812 LED control.
 * Configures the RMT channel and installs the RMT driver.
 */
void ws2812_init()
{
    // Initialize RMT configuration for transmit mode with default settings.
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(RMT_TX_GPIO, RMT_TX_CHANNEL);

    // Adjust RMT clock frequency. WS2812 timing is precise.
    // clk_div = 2 means RMT clock runs at 80 MHz / 2 = 40 MHz.
    // One RMT tick (duration unit) is 1 / 40 MHz = 25 ns.
    config.clk_div = 2;

    // Apply the configuration to the RMT channel.
    ESP_ERROR_CHECK(rmt_config(&config));

    // Install the RMT driver for the configured channel.
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    ESP_LOGI(TAG, "WS2812 RMT driver initialized on GPIO %d, channel %d", RMT_TX_GPIO, RMT_TX_CHANNEL);
}

/**
 * @brief Converts RGB color values into RMT items and sends them to the WS2812 LED.
 * This function is static, meaning it's internal to this file.
 * @param red Red component value (0-255).
 * @param green Green component value (0-255).
 * @param blue Blue component value (0-255).
 */
static void ws2812_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    // WS2812 expects colors in GRB (Green, Red, Blue) order.
    uint32_t color = ((uint32_t)green << 16) | ((uint32_t)red << 8) | blue;

    // Iterate through all 24 bits of the color (8 Green + 8 Red + 8 Blue).
    for (int i = 0; i < 24; i++)
    {
        // Check if the current bit (from MSB, bit 23) is set to 1.
        if (color & (1 << (23 - i)))
        {
            // If bit is 1 (Logic 1 for WS2812): HIGH (0.8us) followed by LOW (0.45us)
            ws2812_bits[i].duration0 = 32; // 32 * 25 ns = 800 ns = 0.8 us
            ws2812_bits[i].level0 = 1;     // First part of pulse is HIGH
            ws2812_bits[i].duration1 = 18; // 18 * 25 ns = 450 ns = 0.45 us
            ws2812_bits[i].level1 = 0;     // Second part of pulse is LOW
        }
        else
        {
            // If bit is 0 (Logic 0 for WS2812): HIGH (0.4us) followed by LOW (0.85us)
            ws2812_bits[i].duration0 = 16; // 16 * 25 ns = 400 ns = 0.4 us
            ws2812_bits[i].level0 = 1;     // First part of pulse is HIGH
            ws2812_bits[i].duration1 = 34; // 34 * 25 ns = 850 ns = 0.85 us
            ws2812_bits[i].level1 = 0;     // Second part of pulse is LOW
        }
    }

    // Send the generated RMT items (pulses) using the RMT driver.
    ESP_ERROR_CHECK(rmt_write_items(RMT_TX_CHANNEL, ws2812_bits, 24, true));

    // A short pause after transmitting each frame.
    // WS2812 protocol requires a reset period (low signal) of at least 50us.
    // A 5ms delay is sufficient and acts as a reset signal for the LEDs.
    vTaskDelay(pdMS_TO_TICKS(5));
}

// --- Predefined Color Setting Functions ---
// These are simple wrappers around the internal ws2812_set_color function.

/**
 * @brief Sets the WS2812 LED to green (0, 255, 0).
 */
void ws2812_set_green() { ws2812_set_color(0, 255, 0); ESP_LOGD(TAG, "Set GREEN color"); }

/**
 * @brief Sets the WS2812 LED to red (255, 0, 0).
 */
void ws2812_set_red()   { ws2812_set_color(255, 0, 0); ESP_LOGD(TAG, "Set RED color"); }

/**
 * @brief Sets the WS2812 LED to blue (0, 0, 255).
 */
void ws2812_set_blue()  { ws2812_set_color(0, 0, 255); ESP_LOGD(TAG, "Set BLUE color"); }

/**
 * @brief Turns off the WS2812 LED (sets color to black: 0, 0, 0).
 */
void ws2812_clear()     { ws2812_set_color(0, 0, 0); ESP_LOGD(TAG, "Cleared WS2812 LED"); }