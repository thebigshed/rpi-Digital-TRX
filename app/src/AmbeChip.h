#pragma once
#include <QByteArray>
#include <QString>
#include <cstdint>

// A DVSI AMBE-3000 vocoder chip on a serial port: the NW Digital Radio DV3000 board on a
// Raspberry Pi (/dev/ttyAMA0, 230400 baud, reset on GPIO 4) or a ThumbDV-style USB dongle.
// It encodes and decodes AMBE / AMBE+2 in hardware in place of mbelib and the OP25 encoders.
//
// Two rates are used:
//   DStar    AMBE 3600x2400 with FEC: the 9-byte D-Star voice frame exactly as on the network
//   Ambe2450 AMBE+2 2450 without FEC: the 49 voice bits of DMR, YSF DN and NXDN, after the
//            mode's own FEC (as mbelib's ambe_d / the OP25 encoder's output)
class AmbeChip {
public:
    enum Rate { DStar, Ambe2450 };

    static AmbeChip *instance() { return bypass_ ? nullptr : instance_; }   // null unless --ambe opened one
    static void setBypass(bool on) { bypass_ = on; }   // self-test: decode with mbelib for comparison

    // "/dev/ttyAMA0" or "/dev/ttyUSB0:460800". On /dev/ttyAMA* the chip is reset on GPIO 4 first.
    static AmbeChip *open(const QString &spec, QString *err);
    ~AmbeChip();

    QString productId() const { return prodId_; }
    QString version() const { return version_; }
    bool companded() const { return compand_; }

    // 20 ms of speech <-> one frame. D-Star: the 9-byte network frame. 2450: 49 bits, one per
    // byte, in mbelib / OP25 order (the chip's own order differs; this class converts).
    // Returns false on a chip error or timeout, or if the chip is busy (see below): the caller
    // then uses the software vocoder.
    //
    // The chip has one decoder, and AMBE decoding carries state from frame to frame, so it
    // follows one stream at a time: `owner` identifies the stream (e.g. its decoder object).
    // Another stream gets it after 300 ms of silence from the current one. While transmitting,
    // the chip only encodes. owner = nullptr skips this (tests).
    bool decodeDStar(const QByteArray &ambe9, int16_t out160[160], const void *owner = nullptr);
    bool encodeDStar(const int16_t in160[160], QByteArray &ambe9);
    bool decode2450(const char bits49[49], int16_t out160[160], const void *owner = nullptr);
    bool encode2450(const int16_t in160[160], uint8_t bits49[49]);

    // Self-test: set a raw rate (RATET/RATEP control payload) and round-trip 20 ms through it.
    bool rawRate(const QByteArray &ratePayload);
    bool roundTrip(const int16_t in160[160], int16_t out160[160], int &nbits);
    bool encodeRaw(const int16_t in160[160], QByteArray &packedBits, int &nbits) { return encodeBits(in160, packedBits, nbits); }

private:
    AmbeChip() = default;
    bool setRate(Rate r);
    bool claimDecoder(const void *owner, Rate r);
    bool transact(const QByteArray &packet, uint8_t wantType, QByteArray &payload, int timeoutMs = 200);
    bool readPacket(uint8_t &type, QByteArray &payload, int timeoutMs);
    bool decodeBits(const QByteArray &packedBits, int nbits, int16_t out160[160]);
    bool encodeBits(const int16_t in160[160], QByteArray &packedBits, int &nbits);

    static AmbeChip *instance_;
    static inline bool bypass_ = false;
    int fd_ = -1;
    int rate_ = -1;
    const void *decOwner_ = nullptr;
    qint64 decLastMs_ = 0, encLastMs_ = 0;
    int encRate_ = -1;
    bool decInit_ = false;
    bool compand_ = false;   // speech samples as 8-bit mu-law (half the serial traffic)   // reset the decoder before the next frame (new stream)
    QString prodId_, version_;
};
