#include "Lcd2004.h"

#include <QThread>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr uint8_t RS = 0x01, EN = 0x04, BL = 0x08;
constexpr uint8_t kRowAddress[4] = {0x00, 0x40, 0x14, 0x54};   // DDRAM start of each line on a 20x4
}

Lcd2004::~Lcd2004()
{
    if (fd_ < 0) return;
    if (keep_) { ::close(fd_); return; }
    command(0x01);   // clear
    QThread::msleep(2);
    backlight_ = 0;
    put(0);
    ::close(fd_);
}

bool Lcd2004::open(QString *err)
{
    for (int addr : {0x27, 0x3F}) {
        const int fd = ::open("/dev/i2c-1", O_RDWR | O_CLOEXEC);
        if (fd < 0) { if (err) *err = "cannot open /dev/i2c-1"; return false; }
        const uint8_t idle = 0;
        // An answer (ACK) to a one-byte write means a backpack is there.
        if (ioctl(fd, I2C_SLAVE, addr) == 0 && ::write(fd, &idle, 1) == 1) {
            fd_ = fd;
            address_ = addr;
            break;
        }
        ::close(fd);
    }
    if (fd_ < 0) { if (err) *err = "no 20x4 display backpack at 0x27 or 0x3F on /dev/i2c-1"; return false; }

    // HD44780 into 4-bit mode
    QThread::msleep(50);
    writeNibble(0x3, false); QThread::msleep(5);
    writeNibble(0x3, false); QThread::usleep(200);
    writeNibble(0x3, false); QThread::usleep(200);
    writeNibble(0x2, false);
    command(0x28);   // 4-bit, 2 line pairs (all four lines), 5x8
    command(0x0C);   // display on, no cursor
    command(0x06);   // move right after each character
    command(0x01);   // clear
    QThread::msleep(2);
    for (QString &s : shown_) s = QString(20, ' ');
    return true;
}

void Lcd2004::show(const QStringList &text)
{
    if (fd_ < 0) return;
    // Write only the characters that changed: each one is four I2C writes.
    for (int row = 0; row < 4; ++row) {
        const QString line = fit(text.value(row), 20);
        int cursor = -1;
        for (int col = 0; col < 20; ++col) {
            const QChar ch = line[col];
            if (ch == shown_[row][col]) continue;
            if (cursor != col) command(uint8_t(0x80 | (kRowAddress[row] + col)));
            writeByte(uint8_t(ch.toLatin1()), true);
            cursor = col + 1;
        }
        shown_[row] = line;
    }
}

void Lcd2004::setColour(int rgb)
{
    const uint8_t bl = rgb ? BL : 0;
    if (bl == backlight_) return;
    backlight_ = bl;
    put(0);
}

bool Lcd2004::put(uint8_t bits)
{
    const uint8_t b = uint8_t(bits | backlight_);
    return fd_ >= 0 && ::write(fd_, &b, 1) == 1;
}

void Lcd2004::writeNibble(uint8_t nibble, bool rs)
{
    // Set the data and RS, then pulse E (data is latched on its fall). Each I2C write takes
    // ~100 us, far longer than the pulse the LCD needs.
    const uint8_t base = uint8_t((nibble << 4) | (rs ? RS : 0));
    put(uint8_t(base | EN));
    put(base);
}

void Lcd2004::writeByte(uint8_t value, bool rs)
{
    writeNibble(uint8_t(value >> 4), rs);
    writeNibble(uint8_t(value & 0x0F), rs);
    if (!rs && value <= 0x03) QThread::msleep(2);   // clear and home take longer
}
