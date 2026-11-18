#pragma once
#include <QObject>
#include <QString>
#include <QTimer>

#include "FrontPanel.h"

// Adafruit RGB LCD Pi Plate: a 16x2 HD44780 LCD, an RGB backlight and five buttons, all on an
// MCP23017 I/O expander on I2C bus 1 at address 0x20.
class LcdPlate : public QObject, public Display {
    Q_OBJECT
public:
    explicit LcdPlate(QObject *parent = nullptr);
    ~LcdPlate() override;

    bool open(int bus = 1, int address = 0x20, QString *err = nullptr);

    int columns() const override { return 16; }
    int rows() const override { return 2; }
    void show(const QStringList &lines) override;
    void setColour(int rgb) override;

signals:
    void pressed(FrontPanel::Key key);
    void released(FrontPanel::Key key);

private:
    bool writeReg(uint8_t reg, uint8_t value);
    bool readReg(uint8_t reg, uint8_t &value);
    void writeNibble(uint8_t nibble, bool rs);
    void writeByte(uint8_t value, bool rs);
    void command(uint8_t c) { writeByte(c, false); }
    void pollButtons();

    int fd_ = -1;
    uint8_t latB_ = 0;           // output latch of port B: LCD lines and the blue LED
    uint8_t latA_ = 0;           // output latch of port A: red and green LEDs
    int colour_ = -1;
    QString shown_[2];           // what is on the display now
    uint8_t buttons_ = 0;        // pressed buttons, one bit each (port A bits 0-4)
    QTimer poll_;
};
