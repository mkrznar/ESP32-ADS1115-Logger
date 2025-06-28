// web_server.h
// This header defines the public API for the ESP32 HTTP web server module.
// It provides functions to start/stop the server and control logging status.

#ifndef WEB_SERVER_H_
#define WEB_SERVER_H_

#include "esp_err.h" // For esp_err_t (standard ESP-IDF error type)
#include <stdbool.h> // For boolean type

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Starts the HTTP web server on ESP32.
 * This function initializes the server configuration, starts the server task,
 * and registers all defined URI handlers (functions that process specific HTTP requests).
 * @return esp_err_t Returns ESP_OK on successful server startup and handler registration,
 * or an appropriate error code otherwise.
 */
esp_err_t start_webserver(void);

/**
 * @brief Sets the global logging status to active or inactive.
 * This function controls whether data is logged to the SD card.
 * It uses an internal mutex for thread-safe access to the logging status variable.
 * @param active Boolean value; set to true to enable logging, false to disable.
 */
void set_logging_active(bool active);

/**
 * @brief Retrieves the current logging status.
 * This function returns the current state of the global flag indicating whether logging is active.
 * It uses an internal mutex for thread-safe reading of the status.
 * @return bool Returns true if logging is currently active, false otherwise.
 */
bool is_logging_enabled(void);

/**
 * @brief Updates the last read voltage values from ADS1115 modules.
 * This function receives an array of 8 float values representing measured voltages
 * from the 8 channels of two ADS1115 modules. These values are stored in an internal
 * global variable within `web_server.c` (`last_voltages`) using a mutex for
 * thread-safe access, allowing the `/adc` handler to retrieve them for web display.
 * @param voltages Pointer to an array of 8 float values with the new voltage readings.
 */
void set_last_voltages(const float *voltages);

/**
 * @brief Retrieves the name of the currently active log file.
 * This function should be implemented in web_server.c and provides the filename
 * that app_main.c is currently logging to.
 * @return const char* Null-terminated string with the name of the current log file.
 * Returns an empty string or "N/A" if no file is currently open or logging is inactive.
 */
const char* get_current_log_file_name(void); 


/**
 * @brief Stops the running instance of the HTTP web server.
 * Safely shuts down the HTTP server, stops associated tasks, and frees allocated resources.
 * This should be called during system shutdown or restart if the server was running.
 */
void stop_webserver(void);

#ifdef __cplusplus
} // end extern "C"
#endif

#endif /* WEB_SERVER_H_ */