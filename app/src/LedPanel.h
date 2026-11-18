#pragma once
// The front panel's seven RGB LEDs (six modes and status), common cathode (see docs/wiring.md):
//
//   module A, MCP23017 at 0x21   DMR GPA0-2, D-Star GPA3-5, YSF GPA6-GPB0, P25 GPB1-3, NXDN GPB4-6
//   module B, MCP23017 at 0x22   M17 GPA0-2, status GPA3-5
//
// Without module B, the M17 and status LEDs can be wired to Pi GPIO instead (M17 GPIO 10 / 9 / 11,
// status GPIO 8 / 7 / 24); they are used only if module B doesn't answer.
//
// Each LED's legs are red, green, blue in that order. A high output lights a colour.
#include "FrontPanel.h"

#include <QString>
#include <cstdint>

class LedPanel : public Indicators {
public:
    ~LedPanel() override;
    bool open(QString *err = nullptr);   // true if any LED can be lit
    QString describe() const;            // what was found, for the event log

    void setLed(int led, int rgb) override;
    void commit() override;

private:
    struct Module { int fd = -1; uint16_t want = 0, shown = 0xFFFF; };   // output latch, bit 0 = GPA0
    static bool writeRegs(Module &m, uint8_t reg, uint8_t a, uint8_t b);

    Module mod_[2];                       // A (0x21), B (0x22)
    int gpio_ = -1;                       // Pi GPIO lines, if module B is missing
    uint64_t gpioWant_ = 0, gpioShown_ = ~0ULL;
};
