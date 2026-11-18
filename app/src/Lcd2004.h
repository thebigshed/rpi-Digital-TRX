#pragma once
// A 20x4 HD44780 character display with a PCF8574 I2C backpack, on I2C bus 1 at 0x27 (PCF8574)
// or 0x3F (PCF8574A), through a level shifter (see docs/wiring.md).
//
// The usual backpack wiring: P0 RS, P1 RW, P2 E, P3 backlight, P4-P7 D4-D7.
#include "FrontPanel.h"

#include <QString>
#include <cstdint>

class Lcd2004 : public Display {
public:
    ~Lcd2004() override;
    bool open(QString *err = nullptr);   // finds the backpack at 0x27 or 0x3F
    int address() const { return address_; }

    int columns() const override { return 20; }
    int rows() const override { return 4; }
    void show(const QStringList &lines) override;
    void setColour(int rgb) override;    // single-colour backlight: off, or on for any colour

private:
    bool put(uint8_t bits);
    void writeNibble(uint8_t nibble, bool rs);
    void writeByte(uint8_t value, bool rs);
    void command(uint8_t c) { writeByte(c, false); }

    int fd_ = -1;
    int address_ = 0;
    uint8_t backlight_ = 0x08;
    QString shown_[4];
};
