// SPDX-License-Identifier: MIT
#include "sensors/i2c_sensors.h"
#include "common.h"

#include <cerrno>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace admuffs {

namespace {

class I2cDev {
public:
    explicit I2cDev(const std::string& bus) { fd_ = open(bus.c_str(), O_RDWR); }
    ~I2cDev() { if (fd_ >= 0) close(fd_); }
    bool ok() const { return fd_ >= 0; }

    bool select(uint8_t addr) { return ioctl(fd_, I2C_SLAVE, addr) >= 0; }
    bool wr(const uint8_t* d, size_t n) { return write(fd_, d, n) == (ssize_t)n; }
    bool rd(uint8_t* d, size_t n) { return read(fd_, d, n) == (ssize_t)n; }
    bool wr1(uint8_t b) { return wr(&b, 1); }

private:
    int fd_ = -1;
};

// ---- HTU21D @0x40: no-hold measurements (Pi handles clock stretching badly)
bool read_htu21d(I2cDev& i2c, double& temp, double& rh) {
    if (!i2c.select(0x40)) return false;
    uint8_t buf[3];

    if (!i2c.wr1(0xF3)) return false;            // trigger temp, no-hold
    sleep_ms(55);
    if (!i2c.rd(buf, 3)) return false;
    uint16_t raw = ((uint16_t)buf[0] << 8) | (buf[1] & 0xFC);
    temp = -46.85 + 175.72 * raw / 65536.0;

    if (!i2c.wr1(0xF5)) return false;            // trigger humidity, no-hold
    sleep_ms(20);
    if (!i2c.rd(buf, 3)) return false;
    raw = ((uint16_t)buf[0] << 8) | (buf[1] & 0xFC);
    rh = -6.0 + 125.0 * raw / 65536.0;
    if (rh < 0) rh = 0;
    if (rh > 100) rh = 100;
    return true;
}

// ---- BH1750 @0x23: one-time high-res mode
bool read_bh1750(I2cDev& i2c, double& lux) {
    if (!i2c.select(0x23)) return false;
    if (!i2c.wr1(0x01)) return false;            // power on
    if (!i2c.wr1(0x20)) return false;            // one-time H-res (auto power-down)
    sleep_ms(180);
    uint8_t buf[2];
    if (!i2c.rd(buf, 2)) return false;
    lux = (((uint16_t)buf[0] << 8) | buf[1]) / 1.2;
    return true;
}

// ---- BMP180 @0x77: full datasheet algorithm, oversampling 0
bool read_bmp180(I2cDev& i2c, double& press_hpa, double& temp_c) {
    if (!i2c.select(0x77)) return false;

    uint8_t id_reg = 0xD0, id = 0;
    if (!i2c.wr(&id_reg, 1) || !i2c.rd(&id, 1) || id != 0x55) return false;

    // Calibration EEPROM 0xAA..0xBF
    uint8_t start = 0xAA, cal[22];
    if (!i2c.wr(&start, 1) || !i2c.rd(cal, 22)) return false;
    auto s16 = [&](int i) { return (int16_t)((cal[i] << 8) | cal[i + 1]); };
    auto u16 = [&](int i) { return (uint16_t)((cal[i] << 8) | cal[i + 1]); };
    int16_t  AC1 = s16(0), AC2 = s16(2), AC3 = s16(4);
    uint16_t AC4 = u16(6), AC5 = u16(8), AC6 = u16(10);
    int16_t  B1 = s16(12), B2 = s16(14), MB = s16(16), MC = s16(18), MD = s16(20);
    (void)MB;

    // Raw temperature
    uint8_t cmd_t[2] = {0xF4, 0x2E};
    if (!i2c.wr(cmd_t, 2)) return false;
    sleep_ms(6);
    uint8_t reg = 0xF6, d[3];
    if (!i2c.wr(&reg, 1) || !i2c.rd(d, 2)) return false;
    int32_t UT = (d[0] << 8) | d[1];

    // Raw pressure (oss = 0)
    uint8_t cmd_p[2] = {0xF4, 0x34};
    if (!i2c.wr(cmd_p, 2)) return false;
    sleep_ms(6);
    if (!i2c.wr(&reg, 1) || !i2c.rd(d, 3)) return false;
    int32_t UP = ((d[0] << 16) | (d[1] << 8) | d[2]) >> 8;

    // Datasheet integer pipeline
    int32_t X1 = ((UT - AC6) * AC5) >> 15;
    int32_t X2 = ((int32_t)MC << 11) / (X1 + MD);
    int32_t B5 = X1 + X2;
    temp_c = ((B5 + 8) >> 4) / 10.0;

    int32_t B6 = B5 - 4000;
    X1 = (B2 * ((B6 * B6) >> 12)) >> 11;
    X2 = (AC2 * B6) >> 11;
    int32_t X3 = X1 + X2;
    int32_t B3 = ((((int32_t)AC1 * 4 + X3)) + 2) / 4;
    X1 = (AC3 * B6) >> 13;
    X2 = (B1 * ((B6 * B6) >> 12)) >> 16;
    X3 = ((X1 + X2) + 2) >> 2;
    uint32_t B4 = (AC4 * (uint32_t)(X3 + 32768)) >> 15;
    uint32_t B7 = ((uint32_t)UP - B3) * 50000;
    int32_t p = (B7 < 0x80000000) ? (B7 * 2) / B4 : (B7 / B4) * 2;
    X1 = (p >> 8) * (p >> 8);
    X1 = (X1 * 3038) >> 16;
    X2 = (-7357 * p) >> 16;
    p = p + ((X1 + X2 + 3791) >> 4);
    press_hpa = p / 100.0;
    return true;
}

}  // namespace

SensorReadings read_sensors(const std::string& bus) {
    SensorReadings r;
    errno = 0;
    I2cDev i2c(bus);
    if (!i2c.ok()) {
        // Fail soft, but say WHY -- "missing bus" and "no permission" have
        // completely different fixes.
        if (errno == EACCES || errno == EPERM)
            r.i2c_error = "permission denied on " + bus +
                          " -- add the user admuffs runs as to the i2c group "
                          "(sudo usermod -aG i2c <user>), then log out/in or "
                          "restart the service";
        else if (errno == ENOENT)
            r.i2c_error = bus + " does not exist -- enable I2C in raspi-config "
                          "(Interface Options > I2C) and reboot";
        else
            r.i2c_error = "cannot open " + bus + ": " + strerror(errno);
        return r;
    }
    r.i2c_present = true;

    r.htu21d_ok = read_htu21d(i2c, r.temp_c, r.humidity_pct);
    r.bh1750_ok = read_bh1750(i2c, r.lux);
    r.bmp180_ok = read_bmp180(i2c, r.pressure_hpa, r.bmp_temp_c);
    return r;
}

}  // namespace admuffs
