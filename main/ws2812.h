// ws2812.h

#ifndef WS2812_H
#define WS2812_H

#include <stdint.h> // For uint8_t

// --- Public Function Declarations for WS2812 LED Driver ---

/**
 * @brief Initializes the WS2812 LED driver.
 * Configures the ESP32's RMT (Remote Control) peripheral to generate the
 * precise timing signals required by WS2812 LEDs. This function must be
 * called once before using any other WS2812 functions.
 */
void ws2812_init(void);

/**
 * @brief Sets the WS2812 LED to a predefined red color.
 */
void ws2812_set_red(void);

/**
 * @brief Sets the WS2812 LED to a predefined green color.
 */
void ws2812_set_green(void);

/**
 * @brief Sets the WS2812 LED to a predefined blue color.
 */
void ws2812_set_blue(void);

/**
 * @brief Turns off the WS2812 LED (sets color to black).
 */
void ws2812_clear(void);

#endif // WS2812_H