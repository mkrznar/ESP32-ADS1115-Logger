# ESP32 ADS1115 Data Logger with Web Interface

This project implements a system for logging analog data using an ESP32 microcontroller and ADS1115 ADC converters, featuring a web interface for configuration, monitoring, and file management on an SD card.

This project was supported and carried out as part of the Strengthening Research and Innovation Excellence in Autonomous Aerial Systems – AeroSTREAM, a Horizon Europe Coordination and Support Action (CSA) project coordinated by the Laboratory for Robotics and Intelligent Control Systems (LARICS) at the University of Zagreb Faculty of Electrical Engineering and Computing (UNIZG FER).

Call: HORIZON-WIDERA-2021-ACCESS-05
Grant Agreement Number: 101071270
Project Duration: July 1, 2022 – June 30, 2025
https://aerostream.fer.hr/aerostream

## Table of Contents
- [Features](#features)
- [Hardware](#hardware)
- [Software](#software)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Compiling and Flashing](#compiling-and-flashing)
- [Web Interface Overview](#web-interface-overview)
  - [Home Page (`/`)](#home-page--)
  - [File Management (`/list`)](#file-management-list)
  - [ADC Monitoring and Logging (`/logging.html`)](#adc-monitoring-and-logging-logginghtml)
  - [Settings (`/settings.html`)](#settings-settingshtml)
- [ADS1115 Integration](#ads1115-integration)
- [Project Structure](#project-structure)
- [License](#license)

## Features
* **Data Acquisition:** Reads analog values from two ADS1115 ADC converters (8 channels total).
* **SD Card Logging:** Automatically saves acquired data in CSV format to an SD card.
* **Wi-Fi Access Point:** ESP32 creates its own Wi-Fi Access Point for client connection.
* **Embedded Web Server:** Serves static web pages (HTML, CSS, JavaScript) directly from the firmware.
* **Interactive Web Interface:**
    * View, download, and delete individual files from the SD card.
    * **File Upload with Progress Bar:** Upload new files to the SD card with a visual percentage progress indicator.
    * **"Delete All" Functionality:** Option to delete all files from the SD card with a confirmation prompt.
    * **Logging Status Monitoring:** Displays current logging status (active/inactive) and the name of the active log file.
    * **Real-time ADC Monitoring:** Shows live ADC readings for all channels (numerical horizontal display and graphical representation).
    * **Configurable Settings:** Adjust scaling factors and measurement units for each ADC channel (saved persistently in NVS), and enable/disable automatic logging on boot.
* **Physical Button Control:** A dedicated physical button on the ESP32 to toggle logging on/off.
* **LED Indication:** WS2812 LED provides visual feedback on the logging status (e.g., green for active, red for inactive).

## Hardware
* **ESP32 Development Board:** (e.g., ESP32-WROOM-32 or ESP32-S3)
* **ADS1115 ADC Modules (x2):** 16-bit Analog-to-Digital Converters, connected via I2C.
* **SD Card and SD Card Module:** Connected via SPI.
* **WS2812B (NeoPixel) LED:** Connected to an RMT-capable GPIO pin.
* **Physical Button:** Connected to a configured GPIO pin (standard boot button or other GPIO).

## Software
* **ESP-IDF:** Espressif IoT Development Framework (v5.4 used in this project).
* **FreeRTOS:** Embedded RTOS for multitasking.
* **HTTPD:** ESP-IDF component for the web server.
* **FATFS:** Integrated for SD card file system operations.
* **NVS (Non-Volatile Storage):** For persistent storage of settings.
* **cJSON:** C library for parsing and generating JSON data.
* **Chart.js:** JavaScript library for drawing interactive charts on the web interface.

## Getting Started

### Prerequisites
1.  **ESP-IDF Installation:** Follow the [ESP-IDF Get Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) for your operating system. Recommended version is **v5.4**.
2.  **Cloning the Repository:**
    ```bash
    git clone <URL_OF_THIS_REPOSITORY>
    cd <REPOSITORY_NAME>
    ```
3.  **Initialize Submodules (if used for components like ADS1115, cJSON):**
    ```bash
    git submodule update --init --recursive
    ```
    (Alternatively, manually add necessary components if not using submodules.)

### Compiling and Flashing
1.  **Project Configuration:**
    * Open `menuconfig`: `idf.py menuconfig`
    * Navigate to `Component config` -> `ESP HTTP Server` and **increase `Max HTTP Request Header Length` to at least `4096` or `8192`** to avoid errors when uploading larger files.
    * Configure SPI pins for the SD card (`Component config` -> `SD/MMC Host Driver` -> `SDSPI: Pin assignments`).
    * Save and exit `menuconfig`.
2.  **Clean, Build, and Flash:**
    ```bash
    idf.py fullclean
    idf.py build
    idf.py -p <YOUR_ESP_PORT> flash monitor
    ```
    Replace `<YOUR_ESP_PORT>` with your ESP32's serial port (e.g., `COMx` on Windows, `/dev/ttyUSBx` on Linux/macOS).

## Web Interface Overview

After flashing, the ESP32 will start a Wi-Fi Access Point named **"ESP32\_SD\_AP"** with the password **"password123"**. Connect your computer or mobile device to this network.
Then, open a web browser and navigate to the ESP32's IP address (typically `192.168.4.1`).

### Home Page (`/`)
Provides general information about the system and navigation links to other parts of the web interface.

### File Management (`/list`)
This page allows you to:
* **Upload Files:** Select a file from your device and upload it to the SD card. Features include selected file name display and a **progress bar with percentage** during upload. If a file with the same name exists, it will prompt for overwrite confirmation.
* **List Files:** Displays a tabular list of all files present on the SD card, with options to download and individually delete each file.
* **"Delete All" Button:** Initiates the deletion of all files from the SD card after a confirmation prompt.

### ADC Monitoring and Logging (`/logging.html`)
This page displays:
* **Current Readings:** Real-time readings from 8 ADC channels (refreshed every 0.5 seconds), shown in a horizontal layout.
* **ADC Readings Graph:** A visual history of ADC readings using Chart.js.
* **Logging Status:** Indicates whether data logging to the SD card is active or inactive, and displays the name of the current log file.
* **"Start Logging" / "Stop Logging" Button:** Web control to activate/deactivate data logging to the SD card.

### Settings (`/settings.html`)
This page allows you to:
* **Log on Boot:** Enable or disable automatic logging when the ESP32 starts up.
* **Channel Configuration:** Adjust scaling factors and measurement units for each of the 8 ADC channels. These settings are permanently saved in NVS (Non-Volatile Storage) and applied to ADC readings. Decimal values for scaling factors will be displayed formatted correctly (e.g., `10.2000`).

## ADS1115 Integration
The project uses two ADS1115 modules on the I2C bus, reading 4 single-ended channels from each. Scaling factors are applied after reading raw voltages, converting them into the desired physical quantities.


## License
This project is licensed under the MIT License.
