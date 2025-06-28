// settings.h
// This header defines data structures and public API for managing application settings,
// including logging preferences and channel configurations, persistently in NVS flash.

#ifndef SETTINGS_H_
#define SETTINGS_H_

#include <stdbool.h> // For boolean type 'bool', 'true', 'false'
#include "esp_err.h" // For esp_err_t and ESP_OK, ESP_FAIL etc.

#ifdef __cplusplus
extern "C" {
#endif

// --- Configuration Definitions ---

/**
 * @def NUM_CHANNELS
 * @brief Total number of measurement channels for which configuration is stored.
 * Currently, two ADS1115 modules are used, each with 4 channels, totaling 8 channels.
 */
#define NUM_CHANNELS 8

/**
 * @def MAX_UNIT_LEN
 * @brief Maximum length of the string (including null terminator) for a measurement unit.
 * 10 characters should be sufficient for common units (e.g., "V", "A", "degC", "bar").
 */
#define MAX_UNIT_LEN 10

/**
 * @struct channel_config_t
 * @brief Structure describing the configuration for a single measurement channel.
 * Contains parameters required to convert raw voltage readings into a scaled
 * physical quantity with its corresponding unit.
 */
typedef struct {
    /**
     * @var scaling_factor
     * @brief Factor by which the measured voltage (in Volts) is multiplied.
     * Used for calibration or scaling. E.g., for a 10:1 voltage divider,
     * this factor would be 10.0 to get the true input voltage. Default is 1.0.
     */
    float scaling_factor;

    /**
     * @var unit
     * @brief String representing the measurement unit to display to the user.
     * E.g., "V", "A", "°C", "bar". This string will be shown on the web interface
     * and potentially in the CSV file header. Default is "V".
     */
    char unit[MAX_UNIT_LEN];
} channel_config_t;


// --- Public Function Declarations ---

/**
 * @brief Initializes the settings module.
 * This crucial function must be called once at application startup.
 * It initializes the ESP-IDF NVS component and loads ALL saved settings
 * (both 'log_on_boot' and configurations for all 8 channels) from flash memory into RAM.
 * If settings are not found (e.g., first boot), safe default values are set.
 */
void settings_init(void);

/**
 * @brief Retrieves the current value of the 'log_on_boot' setting.
 * This function reads the value directly from NVS each time it's called.
 * @return bool True if automatic logging on boot is enabled, false otherwise.
 */
bool settings_get_log_on_boot(void);

/**
 * @brief Sets the 'log_on_boot' setting and persistently saves it to NVS flash.
 * @param enabled Boolean value; true to enable, false to disable.
 */
void settings_set_log_on_boot(bool enabled);

/**
 * @brief Retrieves the configurations for all 8 channels.
 * This function does not read from NVS on each call; instead, it returns a
 * constant pointer to an internal array of structures that was populated
 * during the `settings_init()` call. This ensures efficient access without
 * repeated flash memory operations.
 * @return const channel_config_t* Pointer to an array of NUM_CHANNELS `channel_config_t` structures.
 */
const channel_config_t* settings_get_channel_configs(void);

/**
 * @brief Saves new channel configurations to NVS flash memory.
 * This function takes a pointer to an array of NUM_CHANNELS `channel_config_t`
 * structures and saves the entire array as a single "blob" in NVS. After
 * successful flash write, it also updates the internal RAM copy to ensure
 * settings are immediately active.
 * @param configs Pointer to an array of NUM_CHANNELS `channel_config_t` structures to save.
 * @return esp_err_t Returns ESP_OK on success, or an error code otherwise.
 */
esp_err_t settings_save_channel_configs(const channel_config_t* configs);


#ifdef __cplusplus
}
#endif

#endif // SETTINGS_H_