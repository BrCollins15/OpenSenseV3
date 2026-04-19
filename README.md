# OpenSense V3 - UCF Senior Design Group 1 - Open Source Code 
Created by Brandon Collins and Sponsored by DracoLabs

## Project structure

```
OpenSenseV3/
├── main/
│   ├── app.js                  # Dashboard JS source
│   ├── web/
│   │   ├── app.js.gz           # Compressed JS served by HTTPD
│   │   ├── style.css.gz        # Compressed CSS served by HTTPD
│   │   └── index.html          # Dashboard HTML
│   │   └── right.png           # Dashboard Images
│   │   └── left.png            # Dashboard Images
│   ├── generic_sensor.c        # Generic I2C/ADC register driver
│   ├── sensor_task.c           # 10 Hz sampling loop
│   ├── opensense.h             # Global defines and types
│   ├── hih6130.c / .h          # HIH6130 preset driver
│   ├── sht30.c / .h            # SHT30 preset driver
│   ├── bme280.c / .h           # BME280 preset driver
│   ├── CMakeLists.txt
│   └── ...
├── partitions.csv              # Custom partition table
├── sdkconfig.defaults          # Build defaults including partition config
└── CMakeLists.txt
```

## Flashing the firmware

Open the **ESP-IDF terminal** in VS Code (`Ctrl+Shift+P` → `ESP-IDF: Open ESP-IDF Terminal`). Do not use regular PowerShell — the environment variables will not be set.

# Full erase + flash + open monitor (use after struct or partition changes)
    idf.py -p COM3 erase-flash flash monitor

# Flash only (no erase — safe for minor code changes)
    idf.py -p COM3 flash monitor

Replace `COM3` with the correct port from Device Manager.

> **When to erase flash:** Always erase when the partition table changes, when `sensor_config_t` or `reg_config_t` struct sizes change, or when NVS data from a previous build is suspected to be corrupted.

## Partition table setup

OpenSense uses a custom partition table to provide a large FAT storage partition for the SD card filesystem and NVS. Without this the build will fail or the device will boot incorrectly.

### partitions.csv

The file `partitions.csv` in the project root must contain:

    # Name,   Type, SubType, Offset,   Size
    nvs,      data, nvs,     0x9000,   0x6000,
    phy_init, data, phy,     0xF000,   0x1000,
    factory,  app,  factory, 0x10000,  0x1F0000,
    storage,  data, fat,     0x200000, 0x200000,

### sdkconfig.defaults

The file `sdkconfig.defaults` in the project root must contain the following lines to tell the build system to use the custom table:

    CONFIG_PARTITION_TABLE_CUSTOM=y
    CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
    CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
    CONFIG_PARTITION_TABLE_OFFSET=0x8000

### Applying the partition config

If the partition table settings are not being picked up:

1. Delete the build folder entirely
2. Rebuild from scratch 

## Web asset workflow

The dashboard JavaScript (`app.js`) and stylesheet (`style.css`) are served as gzip-compressed files embedded in the firmware. After editing either source file you must recompress it before building.

All commands below are run in **Git Bash** from the `main/` directory.

### Editing and recompressing app.js

# Navigate to the main folder
    cd main

# Edit app.js in VS Code, then compress and move into web/
    gzip -9 -k -f app.js
    mv app.js.gz web/app.js.gz

### Editing style.css

`style.css` lives compressed at `main/web/style.css.gz`. To edit it:

# Navigate to the web folder
    cd main/web

# Decompress to get the editable source
    gunzip -k style.css.gz

# Edit style.css in VS Code, then recompress
    gzip -9 -k -f style.css

> After recompressing, rebuild and reflash. No flash erase is needed for web asset changes only.

## Adding a new preset sensor driver

Preset drivers handle sensors with non-standard protocols (e.g. trigger-before-read, built-in calibration, CRC validation). Follow these steps to add a new one alongside the existing SHT30, BME280, and HIH6130 drivers.

### Step 1 — Create the driver files

    Create `main/your_sensor.h`:

        #pragma once
        #include "opensense.h"
        #include "esp_err.h"

        // values[0] = first output
        // values[1] = second output (if applicable)
        esp_err_t your_sensor_read(uint8_t port,
                                    const sensor_config_t *cfg,
                                    sensor_reading_t      *out);

    Create `main/your_sensor.c` modelled on `sht30.c`:

        #include "your_sensor.h"
        #include "i2c_bus.h"
        #include "tca9548a.h"
        #include "esp_log.h"
        #include "freertos/FreeRTOS.h"
        #include "freertos/task.h"
        #include <string.h>

        #define TAG "YOUR_SENSOR"

        esp_err_t your_sensor_read(uint8_t port,
                                    const sensor_config_t *cfg,
                                    sensor_reading_t      *out)
        {
            memset(out, 0, sizeof(*out));
            out->valid        = false;
            out->timestamp_us = esp_timer_get_time();
            if (!cfg->enabled) return ESP_OK;

            esp_err_t ret = tca9548a_select_channel(port);
            if (ret != ESP_OK) return ret;

            // --- your read logic here ---

            tca9548a_deselect_all();

            out->values[0] = /* first value */;
            out->valid     = true;
            return ESP_OK;
        }

### Step 2 — Register the sensor type enum

    In `main/opensense.h` add your sensor to the `sensor_type_t` enum:

        typedef enum {
            SENSOR_TYPE_I2C     = 0,
            SENSOR_TYPE_ADC     = 1,
            SENSOR_TYPE_SHT30   = 2,
            SENSOR_TYPE_BME280  = 3,
            SENSOR_TYPE_HIH6130 = 4,
            SENSOR_TYPE_YOUR    = 5,   // ← add here
        } sensor_type_t;

### Step 3 — Add the read call to sensor_task.c

In `main/sensor_task.c`, add the include and a case in the switch:

    #include "your_sensor.h"

        // inside the switch(s_cfg[p].sensor_type) block:
        case SENSOR_TYPE_YOUR:
            your_sensor_read(p, &s_cfg[p], &fresh[p]);
            break;

### Step 4 — Add the driver to CMakeLists.txt

    In `main/CMakeLists.txt` add `"your_sensor.c"` to the `SRCS` list:

        set(SRCS
            ...
            "hih6130.c"
            "your_sensor.c"   # ← add here
        )

### Step 5 — Add the UI option in app.js

    In `main/app.js` add a constant and update three locations:

    // 1. Add constant near the top with the others
        const SENSOR_TYPE_YOUR = 5;

    // 2. Add to the sensor type dropdown in buildModalForm
        <option value="${SENSOR_TYPE_YOUR}" ...>Your Sensor — Description</option>

    // 3. Add to MANAGED_SENSOR_INFO
        [SENSOR_TYPE_YOUR]: {
        title: 'Your Sensor — Description',
        desc:  'Brief description of what the driver does automatically.',
        outputs: ['Output 1 (unit)', 'Output 2 (unit)'],
        defaultAddr: '0xXX',
        addrOptions: null,  // or provide [{value, label}] for selectable addresses
        },

    // 4. Add to isManaged checks in onSensorTypeChange and saveCfgModal
        const isManaged = (stype === SENSOR_TYPE_SHT30 || ... || stype === SENSOR_TYPE_YOUR);

    After editing `app.js` recompress it in the git bash terminal:

        gzip -9 -k -f app.js && mv app.js.gz web/app.js.gz

## Re-enabling port voltage switching

Ports 4 and 5 have hardware support for switching between 3.3V and 5V via a power multiplexer (PWRMUX), analog switch (SW), and level translator (TRAN). This is partially implemented in `main/port_manager.c` and is ready to be re-enabled for future hardware revisions once the level translator signal integrity is improved upon. Keep in mind voltage switching was not required for the poject, and was treated as a strech goal for the group, with that being said the code and sequences were created for a potential hardware iteration with voltage switching capabilities.

### What is already implemented

    `port_manager.c` contains the full switching sequence:

        // GPIO pin assignments (main/opensense.h)
        #define PWRMUX1_SW_GPIO  13   // HIGH = 3.3V (safe),  LOW = 5V (hardware-inverted)
        #define SW1_GPIO          5   // HIGH = enable signal path
        #define TRAN1_EN_GPIO    12   // HIGH = enable level translator

        #define PWRMUX2_SW_GPIO  16
        #define SW2_GPIO         14
        #define TRAN2_EN_GPIO    15


### Re-enabling in firmware and UI

Three changes are required to restore voltage switching for the user.

**1. Restore the voltage badge in `main/app.js`**

Find this line:

    const voltBadge='';

Replace it with:

    const voltBadge = (s.port >= 3)
    ? `<button class="btn secondary sm sc-volt${s.voltage_5v?' v5':''}"
        onclick="toggleVolt(${s.port})">${s.voltage_5v?'5V':'3.3V'}</button>`
    : '';


Then add the toggle function anywhere near `setChartMode`:

    function toggleVolt(port) {
    const cfg = portRawCfgs[port] || {};
    const newVolt = !cfg.voltage_5v;
    const payload = Object.assign({}, cfg, { port, voltage_5v: newVolt });
    fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    }).then(r => r.json()).then(d => {
        if(d.ok) { portRawCfgs[port] = payload; }
    });
    }

**2. Pass the current voltage state when assigning a config**

In `confirmAssign()` in `app.js`, change:

    voltage_5v:false,

To read the existing port state so toggling survives reassignment:

    voltage_5v: portRawCfgs[assigningPort]?.voltage_5v || false,


**3. Recompress app.js after making both edits**

    cd main
    gzip -9 -k -f app.js
    mv app.js.gz web/app.js.gz


The voltage badge will then appear on Port 4 and Port 5 cards only, showing the current supply voltage as a toggleable button. Clicking it sends a POST to `/api/config` which calls `port_manager_update()` in the firmware to perform the physical switching sequence.

If you want to change the default boot voltage, modify `port_manager_init()` in `port_manager.c`:

// Current boot state — safe 3.3V on both ports
gpio_set_level(PWRMUX1_SW_GPIO, 1);  // HIGH = 3.3V
gpio_set_level(PWRMUX2_SW_GPIO, 1);
