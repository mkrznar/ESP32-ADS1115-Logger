#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "web_server.h"
#include "settings.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "ws2812.h"
#include "driver/rmt.h"
#include "ads1115.h"

// --- Definitions and Constants ---

static const char *TAG = "app_main"; // Tag for ESP logging
#define MOUNT_POINT "/sdcard"        // Mount point for the SD card

// Wi-Fi Access Point (AP) configuration
#define WIFI_SSID "ESP32_SD_AP"
#define WIFI_PASSWORD "password123"
#define WIFI_CHANNEL 6
#define WIFI_MAX_CONN 4

// SPI pins for SD card (configured via menuconfig)
#define PIN_NUM_MISO CONFIG_EXAMPLE_PIN_MISO
#define PIN_NUM_MOSI CONFIG_EXAMPLE_PIN_MOSI
#define PIN_NUM_CLK CONFIG_EXAMPLE_PIN_CLK
#define PIN_NUM_CS CONFIG_EXAMPLE_PIN_CS

// I2C Master configuration for ADS1115
#define I2C_MASTER_SCL_IO 17
#define I2C_MASTER_SDA_IO 16
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000 // 400kHz I2C frequency

// ADS1115 ADC configuration
#define ADC1_ADDRESS (0x48)      // ADS1115 ADDR pin to GND
#define ADC2_ADDRESS (0x49)      // ADS1115 ADDR pin to VDD
#define ADC_GAIN ADS1115_FSR_4_096 // Full Scale Range: +/-4.096V
#define ADC_DATA_RATE ADS1115_SPS_860 // Samples Per Second: 860
#define VOLTS_PER_BIT (4.096f / 32767.0f) // Conversion factor from raw ADC value to Volts

// Boot button configuration
#if CONFIG_IDF_TARGET_ESP32S3
#define BOOT_BUTTON_NUM 0 // GPIO for the boot button on ESP32-S3
#else
#define BOOT_BUTTON_NUM 10 // Default GPIO for other ESP32 targets
#endif
#define BUTTON_ACTIVE_LEVEL 0 // Button is active when low (pulled down)

// Logging task parameters
#define LOGGING_TASK_STACK_SIZE 8192 // Stack size for the ADC logging task
#define LOGGING_TASK_INTERVAL_MS 10  // Interval between ADC readings in milliseconds


// --- Global variables for ADS1115 handles ---
static ads1115_t ads1; // Handle for the first ADS1115 module
static ads1115_t ads2; // Handle for the second ADS1115 module

// Global vars for log file name and its mutex
#define MAX_LOG_FILE_PATH_LEN 128
char g_current_log_filepath[MAX_LOG_FILE_PATH_LEN] = "N/A";
SemaphoreHandle_t g_log_file_path_mutex = NULL;


// --- External Functions (from web_server.c) ---
extern void set_last_voltages(const float *voltages); // Updates voltages for web server display
extern bool is_logging_enabled(void);                 // Checks current logging status
extern void set_logging_active(bool active);          // Sets logging status


// --- Callback Functions ---

/**
 * @brief Callback function for the boot button press.
 * Toggles the logging state (active/inactive) and updates the WS2812 LED color.
 * @param handle Button handle.
 * @param args Additional arguments (not used here).
 */
static void button_toggle_cb(void *handle, void *args)
{
    bool new_state = !is_logging_enabled();
    set_logging_active(new_state); // Set the new logging state
    ESP_LOGI(TAG, "Logging state toggled to: %s", new_state ? "ENABLED (ON)" : "DISABLED (OFF)");
}

// --- Utility Functions ---

/**
 * @brief Gets the current timestamp in milliseconds.
 * @return uint32_t Current time in milliseconds since boot.
 */
static uint32_t get_timestamp_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

/**
 * @brief Logs ADC values to a file on the SD card in CSV format.
 * @param file Pointer to the opened file.
 * @param timestamp Timestamp for the log entry.
 * @param values Array of ADC float values.
 * @param count Number of values in the array.
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_ARG if parameters are invalid.
 */
static esp_err_t log_adc_to_sd(FILE *file, uint32_t timestamp, float *values, size_t count)
{
    if (!file || !values)
        return ESP_ERR_INVALID_ARG;

    // Write timestamp
    fprintf(file, "%lu", (unsigned long)timestamp);
    // Write each ADC value separated by semicolon
    for (size_t i = 0; i < count; i++)
    {
        fprintf(file, ";%.6f", values[i]);
    }
    fprintf(file, "\n"); // Newline for the next log entry
    fflush(file); // Flush immediately
    return ESP_OK;
}

/**
 * @brief Opens the next available log file on the SD card (e.g., log_1.csv, log_2.csv).
 * Adds a CSV header to the new file.
 * @param out_path Buffer to store the full path of the opened file.
 * @param path_len Length of the out_path buffer.
 * @return FILE* Pointer to the opened file, or NULL if unable to open.
 */
static FILE *open_next_log_file(char *out_path, size_t path_len)
{
    for (int i = 1; i < 1000; ++i)
    {
        snprintf(out_path, path_len, MOUNT_POINT "/log_%d.csv", i);
        FILE *test = fopen(out_path, "r"); // Try to open for reading to check existence
        if (!test)
        { // If file does not exist, open for writing
            FILE *f = fopen(out_path, "w");
            if (f)
            {
                // Write CSV header
                fprintf(f, "timestamp;adc0;adc1;adc2;adc3;adc4;adc5;adc6;adc7\n");
                fflush(f); // Flush header immediately
                // NOVO: Pohrani ime datoteke u globalnu varijablu uz mutex zaštitu
                if (g_log_file_path_mutex && xSemaphoreTake(g_log_file_path_mutex, pdMS_TO_TICKS(100)) == pdTRUE) { // Increased timeout slightly
                    strncpy(g_current_log_filepath, out_path, MAX_LOG_FILE_PATH_LEN - 1);
                    g_current_log_filepath[MAX_LOG_FILE_PATH_LEN - 1] = '\0';
                    xSemaphoreGive(g_log_file_path_mutex);
                } else {
                    ESP_LOGE(TAG, "Failed to acquire mutex or mutex not created for log file path in open_next_log_file!");
                }
                return f;
            }
            else
            {
                ESP_LOGE(TAG, "Failed to open new log file: %s", out_path);
                return NULL;
            }
        }
        else
        {
            fclose(test); // Close the test file handle
        }
    }
    ESP_LOGE(TAG, "No available name for log file found!");
    return NULL;
}

// --- Main Tasks ---

/**
 * @brief FreeRTOS task for reading ADS1115 data and logging it.
 * This task continuously reads from both ADS1115 modules, applies scaling factors
 * from settings, updates values for the web server, and logs them to an SD card
 * if logging is enabled.
 * @param pvParam Task parameters (not used).
 */
static void ads1115_log_task(void *pvParam)
{
    float final_values[NUM_CHANNELS] = {0}; // Array for scaled ADC values
    char log_path[MAX_LOG_FILE_PATH_LEN];                     // Buffer for log file path
    FILE *file = NULL;                      // File pointer for the current log file

    // ADC multiplexer configurations for single-ended readings
    ads1115_mux_t channels[] = {
        ADS1115_MUX_0_GND,
        ADS1115_MUX_1_GND,
        ADS1115_MUX_2_GND,
        ADS1115_MUX_3_GND};

    // Get a pointer to the channel configurations from the settings module.
    // This pointer is valid throughout the task's lifetime as settings data is in RAM.
    const channel_config_t *configs = settings_get_channel_configs();

    while (1)
    {
        int16_t raw_adc; // Raw ADC reading

        // Read 4 channels from the first ADS1115 (ADC1)
        for (int i = 0; i < 4; i++)
        {
            ads1115_set_mux(&ads1, channels[i]);
            raw_adc = ads1115_get_raw(&ads1);
            if (raw_adc > -32768) // Check for valid reading (not default error value)
            {
                // Convert raw ADC value to voltage
                float raw_voltage = (float)raw_adc * VOLTS_PER_BIT;

                // Apply scaling factor from the settings for this specific channel
                final_values[i] = raw_voltage * configs[i].scaling_factor;
            }
            else
            {
                ESP_LOGE(TAG, "Error reading ADC1 (0x%02X), channel %d", ads1.address, i);
                goto read_error_cycle; // Jump to error handling
            }
        }

        // Read 4 channels from the second ADS1115 (ADC2)
        for (int i = 0; i < 4; i++)
        {
            ads1115_set_mux(&ads2, channels[i]);
            raw_adc = ads1115_get_raw(&ads2);
            if (raw_adc > -32768) // Check for valid reading
            {
                // Convert raw ADC value to voltage
                float raw_voltage = (float)raw_adc * VOLTS_PER_BIT;

                // Apply scaling factor from settings. Note index offset for ADC2 channels.
                final_values[i + 4] = raw_voltage * configs[i + 4].scaling_factor;
            }
            else
            {
                ESP_LOGE(TAG, "Error reading ADC2 (0x%02X), channel %d", ads2.address, i);
                goto read_error_cycle; // Jump to error handling
            }
        }

        // Pass the final, scaled values to the web server for display
        set_last_voltages(final_values);

        // Logic for logging to SD card
        if (is_logging_enabled())
        {
            if (!file) // If no log file is currently open
            {
                file = open_next_log_file(log_path, sizeof(log_path)); // Open a new one
                if (file)
                {
                    ESP_LOGI(TAG, "Log datoteka otvorena: %s", log_path);
                    ws2812_set_green(); // Indicate logging is active with green LED
                }
                else
                {
                    ESP_LOGE(TAG, "Could not open new log file, retrying...");
                    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait longer on file open error
                    continue;
                }
            }
            log_adc_to_sd(file, get_timestamp_ms(), final_values, NUM_CHANNELS); // Log data
        }
        else // If logging is disabled
        {
            if (file) // If a log file was open, close it
            {
                fclose(file);
                ESP_LOGI(TAG, "Log datoteka zatvorena: %s", log_path);
                // NOVO: Postavi ime datoteke na "N/A" u globalnoj varijabli uz mutex zaštitu
                if (g_log_file_path_mutex && xSemaphoreTake(g_log_file_path_mutex, pdMS_TO_TICKS(100)) == pdTRUE) { // Increased timeout slightly
                    strncpy(g_current_log_filepath, "N/A", MAX_LOG_FILE_PATH_LEN - 1);
                    g_current_log_filepath[MAX_LOG_FILE_PATH_LEN - 1] = '\0';
                    xSemaphoreGive(g_log_file_path_mutex);
                }
                file = NULL;           // Reset file pointer
                ws2812_set_red();      // Indicate logging is inactive with red LED
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LOGGING_TASK_INTERVAL_MS)); // Delay for the specified interval
        continue; // Continue to next loop iteration

    read_error_cycle:
        // If an ADC read error occurred, wait for a longer period before retrying
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Clean up if task exits (though it's an infinite loop)
    if (file)
        fclose(file);
    vTaskDelete(NULL);
}

// --- Initialization Functions ---

/**
 * @brief Initializes the ESP32 as a Wi-Fi Access Point (AP).
 * @return esp_err_t ESP_OK on success, error code otherwise.
 */
static esp_err_t wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASSWORD,
            .max_connection = WIFI_MAX_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(WIFI_PASSWORD) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN; // Open authentication if no password is set
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi AP started. SSID:%s password:%s channel:%d", WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
    return ESP_OK;
}

/**
 * @brief Initializes and mounts the SD card using SPI interface.
 * @return esp_err_t ESP_OK on success, error code otherwise.
 */
static esp_err_t init_sd_card(void)
{
    ESP_LOGI(TAG, "Initializing SD card...");
    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, // Do not format if mount fails
        .max_files = 5,                  // Max 5 open files
        .allocation_unit_size = 16 * 1024 // Allocation unit size for FATFS
    };

    sdmmc_card_t *card;          // SD card handle
    sdmmc_host_t host = SDSPI_HOST_DEFAULT(); // Default SPI host configuration

    // SPI bus configuration
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1, // Not used for standard SPI
        .quadhd_io_num = -1, // Not used for standard SPI
        .max_transfer_sz = 4096, // Max transfer size for SPI bus
    };

    // Initialize SPI bus
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus (%s).", esp_err_to_name(ret));
        return ret;
    }

    // SD card device configuration for SPI
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS; // Chip Select pin
    slot_config.host_id = host.slot;  // SPI host ID

    // Mount FATFS on SD card via SPI
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS on SD card (%s).", esp_err_to_name(ret));
        spi_bus_free(host.slot); // Free SPI bus resources on mount failure
        return ret;
    }

    ESP_LOGI(TAG, "SD card successfully mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, card); // Print SD card information
    return ESP_OK;
}

/**
 * @brief Initializes the onboard boot button for logging control.
 */
static void init_button(void)
{
    button_config_t btn_cfg = {0};
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BOOT_BUTTON_NUM,
        .active_level = BUTTON_ACTIVE_LEVEL,
    };
    button_handle_t btn;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn));

    // Register button callback for press-up event to toggle logging.
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_PRESS_UP, NULL, button_toggle_cb, NULL));

    ESP_LOGI(TAG, "Button initialized on GPIO%d.", BOOT_BUTTON_NUM);
}

/**
 * @brief Initializes the I2C master interface.
 * Configures the I2C driver for communication with ADS1115 modules.
 * @return esp_err_t ESP_OK on success, error code otherwise.
 */
static esp_err_t i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

// --- Main Application Entry Point ---
void app_main(void)
{
    ws2812_init();
    ws2812_set_blue();

    ESP_ERROR_CHECK(nvs_flash_init());
    settings_init(); // Initialize settings module and load stored settings

    // NOVO: Inicijaliziraj mutex za globalnu putanju log datoteke
    g_log_file_path_mutex = xSemaphoreCreateMutex();
    if (g_log_file_path_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create g_log_file_path_mutex in app_main!");
    }

    ESP_LOGI(TAG, "Starting Wi-Fi AP...");
    ESP_ERROR_CHECK(wifi_init_softap()); // Start Wi-Fi in Access Point mode

    ESP_LOGI(TAG, "Mounting SD card...");
    if (init_sd_card() != ESP_OK)
    {
        ESP_LOGE(TAG, "SD card not mounted. Logging to card will not work.");
    }

    ESP_LOGI(TAG, "Initializing I2C for ADS1115...");
    ESP_ERROR_CHECK(i2c_master_init()); // Initialize I2C bus

    ESP_LOGI(TAG, "Configuring ADS1115 modules...");
    // Configure ADS1115 modules with their I2C addresses and settings
    ads1 = ads1115_config(I2C_MASTER_NUM, ADC1_ADDRESS);
    ads2 = ads1115_config(I2C_MASTER_NUM, ADC2_ADDRESS);

    ads1115_set_max_ticks(&ads1, pdMS_TO_TICKS(50)); // Povećaj timeout na 50ms
    ads1115_set_max_ticks(&ads2, pdMS_TO_TICKS(50)); // Povećaj timeout na 50ms

    // Apply PGA (Programmable Gain Amplifier) and SPS (Samples Per Second) settings
    ads1115_set_pga(&ads1, ADC_GAIN);
    ads1115_set_sps(&ads1, ADC_DATA_RATE);

    ads1115_set_pga(&ads2, ADC_GAIN);
    ads1115_set_sps(&ads2, ADC_DATA_RATE);

    ESP_LOGI(TAG, "Starting Web server...");
    ESP_ERROR_CHECK(start_webserver()); // Start the HTTP web server

    init_button(); // Initialize the user button

    // Set initial logging state based on NVS settings and update LED
    if (settings_get_log_on_boot())
    {
        set_logging_active(true);
        ws2812_set_green(); // Green LED if logging is enabled on boot
    }
    else
    {
        set_logging_active(false);
        ws2812_set_red(); // Red LED if logging is disabled on boot
        // NOVO: Postavi na N/A ako nije aktivno pri bootu
        if (g_log_file_path_mutex && xSemaphoreTake(g_log_file_path_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            strncpy(g_current_log_filepath, "N/A", MAX_LOG_FILE_PATH_LEN - 1);
            g_current_log_filepath[MAX_LOG_FILE_PATH_LEN - 1] = '\0';
            xSemaphoreGive(g_log_file_path_mutex);
        } else {
            ESP_LOGE(TAG, "Failed to acquire mutex for initial log file path in app_main!");
        }
    }

    // Create and start the FreeRTOS task for ADS1115 data logging
    xTaskCreate(&ads1115_log_task, "ads1115_log_task", LOGGING_TASK_STACK_SIZE, NULL, 5, NULL);
}