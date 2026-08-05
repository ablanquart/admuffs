// SPDX-License-Identifier: MIT
// i2c_sensors.h - readers for the ANAVI Infrared pHAT's optional I2C sensors.
//
// The pHAT's three sensor slots sit on the Pi's I2C bus (GPIO2/3), which is
// entirely separate from the IR transceiver (GPIO17/18, rc-core) -- enabling
// I2C does not affect IR in any way; simultaneous use is the designed setup.
//
// Supported modules (the ones ANAVI ships/documents):
//   HTU21D  @ 0x40  temperature + relative humidity
//   BH1750  @ 0x23  ambient light (lux)
//   BMP180  @ 0x77  barometric pressure + temperature
//
// All functions probe /dev/i2c-1 directly (no external library) and fail
// soft: a missing bus or absent module yields ok=false, never an error path.
#pragma once

#include <string>

namespace admuffs {

struct SensorReadings {
    bool i2c_present = false;      // /dev/i2c-1 exists and opens
    std::string i2c_error;         // when !i2c_present: the reason + the fix

    bool htu21d_ok = false;
    double temp_c = 0, humidity_pct = 0;

    bool bh1750_ok = false;
    double lux = 0;

    bool bmp180_ok = false;
    double pressure_hpa = 0, bmp_temp_c = 0;
};

// Probe the bus and read every module that answers. Total worst-case time
// ~0.4 s (conversion waits); called on-demand from the INFO endpoint only.
SensorReadings read_sensors(const std::string& bus = "/dev/i2c-1");

}  // namespace admuffs
