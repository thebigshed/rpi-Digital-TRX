#include "AmbeChip.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QThread>
#include <cstring>

#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

AmbeChip *AmbeChip::instance_ = nullptr;

namespace {
// DVSI packet protocol: 0x61, payload length (2 bytes, big-endian), type, payload
constexpr uint8_t kStart = 0x61;
constexpr uint8_t kControl = 0x00, kChannel = 0x01, kSpeech = 0x02;
constexpr uint8_t kRateT = 0x09, kRateP = 0x0A, kInit = 0x0B, kProdId = 0x30, kVersion = 0x31,
                  kResetSoftCfg = 0x34, kReady = 0x39, kParityMode = 0x3F;

// The chip's 49-bit AMBE+2 2450 order -> mbelib / OP25 order: chip bit j is mbelib bit
// kPerm[j]. Measured on the chip (--ambe-test with DT_AMBE_PERM=1).
constexpr int kPerm[49] = {0, 18, 36, 1, 19, 37, 2, 20, 38, 3, 21, 39, 4, 22, 40, 5, 23, 41, 6, 24, 42, 7, 25,
                           43, 8, 26, 44, 9, 27, 45, 10, 28, 46, 11, 29, 47, 12, 30, 48, 13, 31, 14, 32, 15,
                           33, 16, 34, 17, 35};

// G.711 mu-law: the chip is set to 8-bit companded speech samples, halving the serial traffic
// (a 20 ms frame is 160 bytes instead of 320: ~15 ms -> ~8 ms each way at 230400 baud).
uint8_t linearToMulaw(int16_t pcm)
{
    constexpr int kBias = 0x84, kClip = 32635;
    int sign = (pcm >> 8) & 0x80;
    int v = sign ? -int(pcm) : int(pcm);
    if (v > kClip) v = kClip;
    v += kBias;
    int exponent = 7;
    for (int mask = 0x4000; (v & mask) == 0 && exponent > 0; mask >>= 1) --exponent;
    const int mantissa = (v >> (exponent + 3)) & 0x0F;
    return uint8_t(~(sign | (exponent << 4) | mantissa));
}

int16_t mulawToLinear(uint8_t u)
{
    u = uint8_t(~u);
    const int sign = u & 0x80, exponent = (u >> 4) & 0x07, mantissa = u & 0x0F;
    int v = ((mantissa << 3) + 0x84) << exponent;
    v -= 0x84;
    return int16_t(sign ? -v : v);
}

QByteArray packet(uint8_t type, const QByteArray &payload)
{
    QByteArray p;
    p += char(kStart);
    p += char((payload.size() >> 8) & 0xFF);
    p += char(payload.size() & 0xFF);
    p += char(type);
    return p + payload;
}

// Pull the Raspberry Pi GPIO line low for 20 ms (the DV3000's reset), then leave it high.
bool resetOnGpio(int gpio)
{
    int base = -1;
    for (const QString &c : QDir("/sys/class/gpio").entryList({"gpiochip*"})) {
        QFile label("/sys/class/gpio/" + c + "/label"), b("/sys/class/gpio/" + c + "/base");
        if (label.open(QIODevice::ReadOnly) && label.readAll().startsWith("pinctrl-bcm") && b.open(QIODevice::ReadOnly))
            base = b.readAll().trimmed().toInt();
    }
    if (base < 0) return false;
    const QByteArray n = QByteArray::number(base + gpio);
    const QString dir = "/sys/class/gpio/gpio" + QString::fromLatin1(n);
    if (!QDir(dir).exists()) {
        QFile e("/sys/class/gpio/export");
        if (!e.open(QIODevice::WriteOnly)) return false;
        e.write(n);
        e.close();
        QThread::msleep(100);
    }
    QFile d(dir + "/direction");
    if (!d.open(QIODevice::WriteOnly)) return false;
    d.write("out");
    d.close();
    QFile v(dir + "/value");
    if (!v.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) return false;
    v.write("0");
    QThread::msleep(20);
    v.write("1");
    v.close();
    QThread::msleep(200);
    return true;
}
}

AmbeChip *AmbeChip::open(const QString &spec, QString *err)
{
#ifdef __linux__
    if (instance_) return instance_;
    const QString dev = spec.section(':', 0, 0);
    const int baud = spec.section(':', 1, 1).isEmpty() ? 230400 : spec.section(':', 1, 1).toInt();
    if (dev.startsWith("/dev/ttyAMA") && !resetOnGpio(4)) {
        if (err) *err = "cannot reset the AMBE board on GPIO 4";
        return nullptr;
    }
    const int fd = ::open(dev.toLocal8Bit().constData(), O_RDWR | O_NOCTTY);
    if (fd < 0) { if (err) *err = "cannot open " + dev; return nullptr; }
    termios tty{};
    tcgetattr(fd, &tty);
    cfmakeraw(&tty);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    const speed_t sp = baud == 460800 ? B460800 : baud == 115200 ? B115200 : B230400;
    cfsetispeed(&tty, sp);
    cfsetospeed(&tty, sp);
    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCIOFLUSH);

    auto *chip = new AmbeChip;
    chip->fd_ = fd;
    QByteArray resp;
    // Soft reset (as AMBEserver), parity off, then identify.
    const QByteArray softReset = packet(kControl, QByteArray::fromHex("34050000" "0f0000"));   // RESETSOFTCFG
    bool ok = false;
    for (int i = 0; i < 4 && !ok; ++i) {
        uint8_t type = 0;
        if (::write(fd, softReset.constData(), size_t(softReset.size())) < 0) break;
        while (chip->readPacket(type, resp, 500))
            if (type == kControl && !resp.isEmpty() && uint8_t(resp[0]) == kReady) { ok = true; break; }
    }
    if (!ok) { delete chip; if (err) *err = "the AMBE chip did not answer on " + dev; return nullptr; }
    chip->transact(packet(kControl, QByteArray::fromHex("3f00")), kControl, resp);   // parity off
    // 8-bit mu-law speech samples (COMPAND field 0x32: bit 0 on, bit 1 clear = mu-law)
    chip->compand_ = chip->transact(packet(kControl, QByteArray::fromHex("3201")), kControl, resp) &&
                     resp.size() >= 2 && uint8_t(resp[1]) == 0;
    if (chip->transact(packet(kControl, QByteArray(1, char(kProdId))), kControl, resp) && resp.size() > 1)
        chip->prodId_ = QString::fromLatin1(resp.mid(1)).section(QChar(0), 0, 0);
    if (chip->transact(packet(kControl, QByteArray(1, char(kVersion))), kControl, resp) && resp.size() > 1)
        chip->version_ = QString::fromLatin1(resp.mid(1)).section(QChar(0), 0, 0);
    instance_ = chip;
    return chip;
#else
    Q_UNUSED(spec)
    if (err) *err = "the AMBE chip needs Linux";
    return nullptr;
#endif
}

AmbeChip::~AmbeChip()
{
#ifdef __linux__
    if (fd_ >= 0) ::close(fd_);
#endif
    if (instance_ == this) instance_ = nullptr;
}

bool AmbeChip::readPacket(uint8_t &type, QByteArray &payload, int timeoutMs)
{
#ifdef __linux__
    QByteArray buf;
    auto readN = [&](int n) {
        while (buf.size() < n) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, timeoutMs) <= 0) return false;
            char tmp[512];
            const ssize_t got = ::read(fd_, tmp, size_t(std::min<qsizetype>(sizeof tmp, n - buf.size())));
            if (got <= 0) return false;
            buf.append(tmp, int(got));
        }
        return true;
    };
    // Find the start byte
    for (;;) {
        buf.clear();
        if (!readN(1)) return false;
        if (uint8_t(buf[0]) == kStart) break;
    }
    if (!readN(4)) return false;
    const int len = (uint8_t(buf[1]) << 8) | uint8_t(buf[2]);
    type = uint8_t(buf[3]);
    if (len > 1024 || !readN(4 + len)) return false;
    payload = buf.mid(4, len);
    return true;
#else
    Q_UNUSED(type) Q_UNUSED(payload) Q_UNUSED(timeoutMs)
    return false;
#endif
}

bool AmbeChip::transact(const QByteArray &pkt, uint8_t wantType, QByteArray &payload, int timeoutMs)
{
#ifdef __linux__
    if (fd_ < 0) return false;
    if (::write(fd_, pkt.constData(), size_t(pkt.size())) != pkt.size()) return false;
    uint8_t type = 0;
    while (readPacket(type, payload, timeoutMs))
        if (type == wantType) return true;
    return false;
#else
    Q_UNUSED(pkt) Q_UNUSED(wantType) Q_UNUSED(payload) Q_UNUSED(timeoutMs)
    return false;
#endif
}

bool AmbeChip::setRate(Rate r)
{
    if (rate_ == r) return true;
    QByteArray resp;
    bool ok = false;
    if (r == DStar)   // AMBE 3600x2400 with FEC, as used by D-Star (DVSI rate parameters)
        ok = transact(packet(kControl, QByteArray::fromHex("0a" "0130" "0763" "4000" "0000" "0000" "0048")), kControl, resp);
    else              // rate table index 34: AMBE+2 2450 bps, no FEC (49 bits)
        ok = transact(packet(kControl, QByteArray::fromHex("0922")), kControl, resp);
    ok = ok && resp.size() >= 2 && uint8_t(resp[1]) == 0;
    // Start the encoder and decoder afresh in the new rate
    if (ok) transact(packet(kControl, QByteArray::fromHex("0b03")), kControl, resp);
    rate_ = ok ? r : -1;
    return ok;
}

bool AmbeChip::decodeBits(const QByteArray &packed, int nbits, int16_t out160[160])
{
    QByteArray payload;
    payload += char(0x01);            // CHAND field
    payload += char(nbits);
    payload += packed;
    QByteArray resp;
    if (!transact(packet(kChannel, payload), kSpeech, resp)) return false;
    // SPEECHD field: 0x00, sample count, samples (mu-law bytes, or 16-bit big-endian)
    const int bytesPer = compand_ ? 1 : 2;
    if (resp.size() < 2 + 160 * bytesPer || uint8_t(resp[0]) != 0x00) return false;
    const int n = std::min(160, int(uint8_t(resp[1])));
    for (int i = 0; i < n; ++i)
        out160[i] = compand_ ? mulawToLinear(uint8_t(resp[2 + i]))
                             : int16_t((uint8_t(resp[2 + 2 * i]) << 8) | uint8_t(resp[3 + 2 * i]));
    for (int i = n; i < 160; ++i) out160[i] = 0;
    return true;
}

bool AmbeChip::encodeBits(const int16_t in160[160], QByteArray &packed, int &nbits)
{
    QByteArray payload;
    payload += char(0x00);            // SPEECHD field
    payload += char(160);
    for (int i = 0; i < 160; ++i) {
        if (compand_) {
            payload += char(linearToMulaw(in160[i]));
        } else {
            payload += char((uint16_t(in160[i]) >> 8) & 0xFF);
            payload += char(uint16_t(in160[i]) & 0xFF);
        }
    }
    QByteArray resp;
    if (!transact(packet(kSpeech, payload), kChannel, resp)) return false;
    // CHAND field: 0x01, bit count, bits (MSB first)
    if (resp.size() < 2 || uint8_t(resp[0]) != 0x01) return false;
    nbits = uint8_t(resp[1]);
    packed = resp.mid(2, (nbits + 7) / 8);
    return packed.size() == (nbits + 7) / 8;
}

bool AmbeChip::claimDecoder(const void *owner, Rate r)
{
    if (!owner) return true;   // tests
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // While transmitting, the chip only encodes: at 230400 baud a 20 ms frame takes ~15 ms on the
    // serial line each way, so it can't also decode in real time. (Our own transmission is
    // decoded in software for the log.)
    if (now - encLastMs_ < 300) return false;
    if (decOwner_ && decOwner_ != owner && now - decLastMs_ < 300) return false;   // another stream has it
    if (decOwner_ != owner) { decOwner_ = owner; decInit_ = true; }
    decLastMs_ = now;
    return true;
}

bool AmbeChip::decodeDStar(const QByteArray &ambe9, int16_t out160[160], const void *owner)
{
    if (ambe9.size() != 9 || !claimDecoder(owner, DStar) || !setRate(DStar)) return false;
    QByteArray resp;
    if (decInit_) { transact(packet(kControl, QByteArray::fromHex("0b02")), kControl, resp); decInit_ = false; }
    return decodeBits(ambe9, 72, out160);
}

bool AmbeChip::encodeDStar(const int16_t in160[160], QByteArray &ambe9)
{
    int n = 0;
    encLastMs_ = QDateTime::currentMSecsSinceEpoch();
    encRate_ = DStar;
    return setRate(DStar) && encodeBits(in160, ambe9, n) && n == 72;
}

bool AmbeChip::decode2450(const char bits49[49], int16_t out160[160], const void *owner)
{
    if (!claimDecoder(owner, Ambe2450) || !setRate(Ambe2450)) return false;
    QByteArray resp;
    if (decInit_) { transact(packet(kControl, QByteArray::fromHex("0b02")), kControl, resp); decInit_ = false; }
    QByteArray packed(7, 0);
    for (int j = 0; j < 49; ++j)
        if (bits49[kPerm[j]]) packed[j >> 3] = char(uint8_t(packed[j >> 3]) | (0x80 >> (j & 7)));
    return decodeBits(packed, 49, out160);
}

bool AmbeChip::encode2450(const int16_t in160[160], uint8_t bits49[49])
{
    QByteArray packed;
    int n = 0;
    encLastMs_ = QDateTime::currentMSecsSinceEpoch();
    encRate_ = Ambe2450;
    if (!setRate(Ambe2450) || !encodeBits(in160, packed, n) || n != 49) return false;
    for (int j = 0; j < 49; ++j) bits49[kPerm[j]] = (uint8_t(packed[j >> 3]) >> (7 - (j & 7))) & 1;
    return true;
}

bool AmbeChip::rawRate(const QByteArray &ratePayload)
{
    QByteArray resp;
    rate_ = -1;
    const bool ok = transact(packet(kControl, ratePayload), kControl, resp) && resp.size() >= 2 && uint8_t(resp[1]) == 0;
    if (ok) transact(packet(kControl, QByteArray::fromHex("0b03")), kControl, resp);
    return ok;
}

bool AmbeChip::roundTrip(const int16_t in160[160], int16_t out160[160], int &nbits)
{
    QByteArray bits;
    return encodeBits(in160, bits, nbits) && decodeBits(bits, nbits, out160);
}
