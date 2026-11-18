#include "LcdPlate.h"
#include <QThread>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

// MCP23017 registers (IOCON.BANK = 0, the power-up default)
namespace {
constexpr uint8_t IODIRA = 0x00, IODIRB = 0x01, GPPUA = 0x0C, GPIOA = 0x12, OLATA = 0x14, OLATB = 0x15;

// Adafruit's wiring of the plate:
//   port A: bits 0-4 buttons (Select, Right, Down, Up, Left; low = pressed), bit 6 red LED,
//           bit 7 green LED
//   port B: bit 0 blue LED, bit 1 D7, bit 2 D6, bit 3 D5, bit 4 D4, bit 5 E, bit 6 RW, bit 7 RS
// The LEDs are lit by a low output.
constexpr uint8_t A_RED = 0x40, A_GREEN = 0x80;
constexpr uint8_t B_BLUE = 0x01, B_EN = 0x20, B_RS = 0x80;
constexpr uint8_t kButtonMask = 0x1F;
// Port A bits 0-4, in order
constexpr FrontPanel::Key kButtons[5] = {FrontPanel::Key::Select, FrontPanel::Key::Right, FrontPanel::Key::Down,
                                         FrontPanel::Key::Up, FrontPanel::Key::Left};

uint8_t nibbleBits(uint8_t n)   // D4..D7 are wired in reverse order on port B
{
    return uint8_t(((n & 1) ? 0x10 : 0) | ((n & 2) ? 0x08 : 0) | ((n & 4) ? 0x04 : 0) | ((n & 8) ? 0x02 : 0));
}
}

LcdPlate::LcdPlate(QObject *parent) : QObject(parent)
{
    poll_.setInterval(30);
    connect(&poll_, &QTimer::timeout, this, &LcdPlate::pollButtons);
}

LcdPlate::~LcdPlate()
{
    if (fd_ >= 0 && keep_) {
        ::close(fd_);
    } else if (fd_ >= 0) {
        setColour(Off);
        command(0x01);   // clear
        ::close(fd_);
    }
}

bool LcdPlate::open(int bus, int address, QString *err)
{
    const QByteArray dev = QString("/dev/i2c-%1").arg(bus).toLatin1();
    fd_ = ::open(dev.constData(), O_RDWR);
    if (fd_ < 0) { if (err) *err = "cannot open " + QString::fromLatin1(dev); return false; }
    if (ioctl(fd_, I2C_SLAVE, address) < 0) {
        ::close(fd_);
        fd_ = -1;
        if (err) *err = QString("no I2C device at 0x%1").arg(address, 2, 16, QChar('0'));
        return false;
    }
    // Port A: buttons in with pull-ups, LEDs out (off). Port B: all out.
    latA_ = A_RED | A_GREEN;
    latB_ = B_BLUE;
    if (!writeReg(IODIRA, 0x3F) || !writeReg(GPPUA, kButtonMask) || !writeReg(OLATA, latA_) ||
        !writeReg(IODIRB, 0x00) || !writeReg(OLATB, latB_)) {
        ::close(fd_);
        fd_ = -1;
        if (err) *err = "the LCD plate does not answer on I2C";
        return false;
    }
    // HD44780 into 4-bit mode
    QThread::msleep(50);
    writeNibble(0x3, false); QThread::msleep(5);
    writeNibble(0x3, false); QThread::usleep(200);
    writeNibble(0x3, false); QThread::usleep(200);
    writeNibble(0x2, false);
    command(0x28);   // 4-bit, 2 lines, 5x8
    command(0x0C);   // display on, no cursor
    command(0x06);   // move right after each character
    command(0x01);   // clear
    QThread::msleep(2);
    shown_[0] = shown_[1] = QString(16, ' ');
    poll_.start();
    return true;
}

void LcdPlate::show(const QStringList &text)
{
    const QString lines[2] = {fit(text.value(0), 16), fit(text.value(1), 16)};
    if (fd_ < 0) return;
    // Write only the characters that changed: each one costs several I2C transfers.
    for (int row = 0; row < 2; ++row) {
        int cursor = -1;
        for (int col = 0; col < 16; ++col) {
            const QChar ch = lines[row][col];
            if (ch == shown_[row][col]) continue;
            if (cursor != col) command(uint8_t(0x80 | (row ? 0x40 : 0x00) | col));
            writeByte(uint8_t(ch.toLatin1()), true);
            cursor = col + 1;
        }
        shown_[row] = lines[row];
    }
}

void LcdPlate::setColour(int rgb)
{
    if (rgb == colour_) return;
    colour_ = rgb;
    if (fd_ < 0) return;
    // LEDs are lit by a low output
    latA_ = uint8_t((latA_ & ~(A_RED | A_GREEN)) | ((rgb & 4) ? 0 : A_RED) | ((rgb & 2) ? 0 : A_GREEN));
    latB_ = uint8_t((latB_ & ~B_BLUE) | ((rgb & 1) ? 0 : B_BLUE));
    writeReg(OLATA, latA_);
    writeReg(OLATB, latB_);
}

void LcdPlate::pollButtons()
{
    uint8_t a = 0;
    if (!readReg(GPIOA, a)) return;
    const uint8_t now = uint8_t(~a & kButtonMask);   // low = pressed
    const uint8_t changed = now ^ buttons_;
    buttons_ = now;
    for (int b = 0; b < 5; ++b) {
        if (!(changed & (1 << b))) continue;
        if (now & (1 << b)) emit pressed(kButtons[b]);
        else emit released(kButtons[b]);
    }
}

bool LcdPlate::writeReg(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return fd_ >= 0 && ::write(fd_, buf, 2) == 2;
}

bool LcdPlate::readReg(uint8_t reg, uint8_t &value)
{
    return fd_ >= 0 && ::write(fd_, &reg, 1) == 1 && ::read(fd_, &value, 1) == 1;
}

void LcdPlate::writeNibble(uint8_t nibble, bool rs)
{
    // Keep the blue LED bit, set the data and RS, then pulse E (data is latched on its fall).
    const uint8_t base = uint8_t((latB_ & B_BLUE) | nibbleBits(nibble) | (rs ? B_RS : 0));
    writeReg(OLATB, uint8_t(base | B_EN));
    writeReg(OLATB, base);
    latB_ = base;
}

void LcdPlate::writeByte(uint8_t value, bool rs)
{
    writeNibble(uint8_t(value >> 4), rs);
    writeNibble(uint8_t(value & 0x0F), rs);
    // Each I2C transfer takes longer than the LCD needs (~40 us), except for clear and home.
    if (!rs && value <= 0x03) QThread::msleep(2);
}
