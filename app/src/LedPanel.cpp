#include "LedPanel.h"
#include "Gpio.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr uint8_t IODIRA = 0x00, OLATA = 0x14;   // IOCON.BANK = 0: A and B registers alternate
constexpr int kAddress[2] = {0x21, 0x22};

// Pi GPIO lines used without module B, in order: M17 blue, green, red, then status blue, green, red
const QList<unsigned> kGpioLines = {10, 9, 11, 8, 7, 25};

// Each LED's module and its blue, green and red pins (0-7 = GPA0-7, 8-15 = GPB0-7), as wired on
// the panel: P25 and NXDN run red, blue, green. M17 and status move to the GPIO lines
// (index = pin) when module B is missing.
struct Place { int module; int pin[3]; };
constexpr Place kPlace[Indicators::Count] = {
    {0, {0, 1, 2}}, {0, {3, 4, 5}}, {0, {6, 7, 8}},   // DMR, D-Star, YSF
    {0, {10, 11, 9}}, {0, {13, 14, 12}},              // P25, NXDN
    {1, {0, 1, 2}}, {1, {3, 4, 5}}};                  // M17, status
}

LedPanel::~LedPanel()
{
    for (Module &m : mod_) {
        if (m.fd < 0) continue;
        writeRegs(m, OLATA, 0, 0);
        ::close(m.fd);
    }
    if (gpio_ >= 0) {
        Gpio::setValues(gpio_, 0, (1ULL << kGpioLines.size()) - 1);
        ::close(gpio_);
    }
}

bool LedPanel::open(QString *err)
{
    QStringList errors;
    for (int i = 0; i < 2; ++i) {
        Module &m = mod_[i];
        m.fd = ::open("/dev/i2c-1", O_RDWR | O_CLOEXEC);
        // All 16 pins outputs, low (LEDs off; spare pins stay low too).
        if (m.fd < 0 || ioctl(m.fd, I2C_SLAVE, kAddress[i]) < 0 || !writeRegs(m, OLATA, 0, 0) ||
            !writeRegs(m, IODIRA, 0, 0)) {
            if (m.fd >= 0) ::close(m.fd);
            m.fd = -1;
            if (i == 0) errors << "no LED module A (MCP23017) at 0x21";
            continue;
        }
        m.shown = 0;
    }
    if (mod_[1].fd < 0) {   // no module B: the M17 and status LEDs may be on Pi GPIO
        QString gpioErr;
        gpio_ = Gpio::requestOutputs(kGpioLines, &gpioErr);
        if (gpio_ < 0) errors << "no LED module B (MCP23017) at 0x22, and " + gpioErr;
        else gpioShown_ = 0;
    }
    if (err) *err = errors.join("; ");
    return mod_[0].fd >= 0 || mod_[1].fd >= 0 || gpio_ >= 0;
}

QString LedPanel::describe() const
{
    QStringList parts;
    if (mod_[0].fd >= 0) parts << "module A at 0x21";
    if (mod_[1].fd >= 0) parts << "module B at 0x22";
    else if (gpio_ >= 0) parts << "M17 and status on GPIO 7-11 and 25";
    return parts.join(", ");
}

void LedPanel::setLed(int led, int rgb)
{
    if (led < 0 || led >= Count) return;
    const Place p = kPlace[led];
    const bool onGpio = p.module == 1 && mod_[1].fd < 0;
    const int bits[3] = {rgb & 1, (rgb >> 1) & 1, (rgb >> 2) & 1};   // blue, green, red
    for (int c = 0; c < 3; ++c) {
        if (onGpio) {
            const uint64_t mask = 1ULL << p.pin[c];
            gpioWant_ = bits[c] ? gpioWant_ | mask : gpioWant_ & ~mask;
        } else {
            uint16_t &w = mod_[p.module].want;
            const uint16_t mask = uint16_t(1u << p.pin[c]);
            w = bits[c] ? uint16_t(w | mask) : uint16_t(w & ~mask);
        }
    }
}

void LedPanel::commit()
{
    for (Module &m : mod_)
        if (m.fd >= 0 && m.want != m.shown && writeRegs(m, OLATA, uint8_t(m.want & 0xFF), uint8_t(m.want >> 8)))
            m.shown = m.want;
    if (gpio_ >= 0 && gpioWant_ != gpioShown_ && Gpio::setValues(gpio_, gpioWant_, (1ULL << kGpioLines.size()) - 1))
        gpioShown_ = gpioWant_;
}

bool LedPanel::writeRegs(Module &m, uint8_t reg, uint8_t a, uint8_t b)
{
    const uint8_t buf[3] = {reg, a, b};
    return m.fd >= 0 && ::write(m.fd, buf, 3) == 3;
}
