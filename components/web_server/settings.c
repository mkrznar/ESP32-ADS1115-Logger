// settings.c
// Implements a module for managing application settings, including logging preferences
// and channel configurations, using ESP-IDF's Non-Volatile Storage (NVS) component.

#include "settings.h"
#include "nvs_flash.h" // For NVS initialization and deinitialization
#include "nvs.h"       // For NVS read/write operations
#include "esp_log.h"   // For ESP-IDF logging
#include "esp_err.h"   // For esp_err_t error codes
#include <string.h>    // Required for memcpy for safe structure copying

// --- Module Constants ---
static const char *TAG = "settings";
#define NAMESPACE "app_settings" // NVS namespace for organizing keys

// Keys for storing values in NVS.
#define KEY_LOG_ON_BOOT "log_on_boot"   // Key for the boolean logging flag.
#define KEY_CHAN_CONFIGS "chan_configs" // Key for storing all channel configurations as a blob.

// --- Global Static Variables ---
/**
 * @brief Internal array in RAM that holds configurations for all NUM_CHANNELS.
 * This array is populated from NVS during `settings_init()`.
 * Accessing this RAM variable for queries is much faster than direct NVS reads
 * and reduces flash wear. 'static' limits its scope to this file.
 */
static channel_config_t channel_configs[NUM_CHANNELS];

// --- Private Utility Function ---
/**
 * @brief Sets default values for all channel configurations.
 * This function is called exclusively if no configurations are found in NVS
 * (e.g., at first boot or after flash erase). It sets safe initial values:
 * scaling factor 1.0 and measurement unit "V" (Volt), which the user can
 * later modify via the web interface.
 */
static void set_default_channel_configs() {
    ESP_LOGW(TAG, "Channel configurations not found in NVS. Setting default values.");
    for (int i = 0; i < NUM_CHANNELS; i++) {
        channel_configs[i].scaling_factor = 1.0f;
        // Use snprintf for safe string copy into the 'unit' field.
        snprintf(channel_configs[i].unit, MAX_UNIT_LEN, "V");
    }
}

// --- Public Function Implementations ---

/**
 * @brief Initializes the settings module and loads all settings from NVS.
 * Handles NVS initialization, opening the namespace, and loading both
 * 'log_on_boot' and channel configurations. Sets defaults if not found or on error.
 */
void settings_init(void) {
    // Initialize NVS flash
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition is corrupt or has a new version, erase and re-initialize.
        ESP_LOGW(TAG, "NVS partition problem detected, erasing and re-initializing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err); // Propagate any persistent NVS initialization errors.

    // Open NVS for reading all settings.
    nvs_handle_t nvs_handle;
    err = nvs_open(NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s. Using default settings for all.", esp_err_to_name(err));
        set_default_channel_configs(); // Fallback to defaults if NVS cannot be opened.
        return; // Exit as further reading is not possible.
    }

    // 1. Load 'log_on_boot' setting
    uint8_t log_on_boot_val = 0; // Default value if not found
    err = nvs_get_u8(nvs_handle, KEY_LOG_ON_BOOT, &log_on_boot_val);
    if (err == ESP_OK) {
        // Current implementation of settings_get_log_on_boot reads from NVS directly.
        // If it were to read from a global variable, we would update it here.
        // For now, this is just for logging the loaded value.
        ESP_LOGI(TAG, "Loaded setting 'log_on_boot' = %d", log_on_boot_val);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "'log_on_boot' setting not found, defaulting to 0.");
    } else {
        ESP_LOGE(TAG, "Error reading 'log_on_boot': %s", esp_err_to_name(err));
    }


    // 2. Load channel configurations as a 'blob' (binary object).
    // 'Blob' is simply an array of bytes. Here, we read the entire 'channel_configs' array at once.
    size_t required_size = sizeof(channel_configs); // Expected size of the blob.
    // Attempt to read the blob from NVS into our global variable.
    err = nvs_get_blob(nvs_handle, KEY_CHAN_CONFIGS, channel_configs, &required_size);

    if (err != ESP_OK || required_size != sizeof(channel_configs)) {
        // If an error occurred OR if the size of the saved blob is different from expected.
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            // Key not found - this is expected on first boot.
            set_default_channel_configs();
        } else {
            // Another error occurred (e.g., data corruption).
            ESP_LOGE(TAG, "Error reading channel configurations: %s. Setting default values.", esp_err_to_name(err));
            set_default_channel_configs();
        }
    } else {
        ESP_LOGI(TAG, "Channel configurations successfully loaded from NVS.");
    }

    // Close the NVS handle after all readings are complete.
    nvs_close(nvs_handle);
}

/**
 * @brief Retrieves the current 'log_on_boot' setting from NVS.
 * Note: This function reads directly from NVS each time it is called.
 * @return bool True if automatic logging on boot is enabled, false otherwise.
 */
bool settings_get_log_on_boot(void) {
    nvs_handle_t nvs_handle;
    uint8_t val = 0; // Default to false if not found or error
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS in settings_get_log_on_boot: %s", esp_err_to_name(err));
        return false;
    }
    nvs_get_u8(nvs_handle, KEY_LOG_ON_BOOT, &val); // Read the value
    nvs_close(nvs_handle);
    return (val != 0); // Convert u8 to bool
}

/**
 * @brief Sets the 'log_on_boot' setting and saves it to NVS.
 * @param enabled Boolean value; true to enable, false to disable.
 */
void settings_set_log_on_boot(bool enabled) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing in settings_set_log_on_boot: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs_handle, KEY_LOG_ON_BOOT, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle); // Commit changes to flash
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Saved: log_on_boot = %d", enabled);
        } else {
            ESP_LOGE(TAG, "Error committing 'log_on_boot': %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Error setting 'log_on_boot' in NVS: %s", esp_err_to_name(err));
    }
    nvs_close(nvs_handle);
}

/**
 * @brief Retrieves the configurations for all NUM_CHANNELS.
 * Returns a constant pointer to the internal, already-loaded global variable.
 * This is an efficient read as it avoids flash access.
 * @return const channel_config_t* Pointer to an array of channel_config_t structures.
 */
const channel_config_t* settings_get_channel_configs(void) {
    return channel_configs;
}

/**
 * @brief Saves new channel configurations to NVS flash memory.
 * The entire array of NUM_CHANNELS structures is saved as a single blob.
 * The internal RAM copy (`channel_configs`) is also updated after successful NVS write.
 * @param configs Pointer to an array of NUM_CHANNELS `channel_config_t` structures to save.
 * @return esp_err_t Returns ESP_OK on success, or an error code otherwise.
 */
esp_err_t settings_save_channel_configs(const channel_config_t* configs) {
    if (!configs) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing channel configs: %s", esp_err_to_name(err));
        return err;
    }

    // Write the entire array of structures as a single 'blob' to NVS.
    err = nvs_set_blob(nvs_handle, KEY_CHAN_CONFIGS, configs, sizeof(channel_configs));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle); // Commit changes to physical flash memory.
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Channel configurations successfully saved to NVS.");
            // Update the internal global variable in RAM to be synchronized
            // with what was just saved to NVS.
            memcpy(channel_configs, configs, sizeof(channel_configs));
        } else {
            ESP_LOGE(TAG, "Error committing channel configurations: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Error setting channel configurations blob in NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return err;
}