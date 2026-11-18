#pragma once
// DMR "Homebrew" (DMRD) packet parsing and AMBE+2 voice decoding via mbelib.
#include <QByteArray>
#include <QString>
#include <cstdint>
#include <vector>
#include <deque>

extern "C" {
#include "mbelib.h"
}

struct DmrPacket {
    uint8_t  seq = 0;
    uint32_t src = 0;
    uint32_t dst = 0;
    int      slot = 1;          // 1 or 2
    bool     privateCall = false;
    int      frameType = 0;     // 0 voice, 1 voice sync, 2 data sync
    int      dataType = 0;      // for data sync: 1 = voice LC header, 2 = terminator
    QByteArray stream;          // 4-byte stream id
    QByteArray payload;         // 33-byte DMR burst

    bool isHeader() const     { return frameType == 2 && dataType == 1; }
    bool isTerminator() const { return frameType == 2 && dataType == 2; }
    bool isVoice() const      { return frameType == 0 || frameType == 1; }
};

bool parseDmrd(const QByteArray &raw, DmrPacket &out);

// 216 voice bits of a burst (sync/EMB removed), used for duplicate detection.
QByteArray voiceBits(const QByteArray &payload33);

// One 72-bit DMR AMBE+2 frame in on-air order (one bit per char) -> mbelib ambe_fr.
void dmrDeinterleave(const char *bits72, char ambe_fr[4][24]);

class AmbeDecoder {
public:
    AmbeDecoder() { reset(); }
    void reset();
    // Decode one 33-byte voice burst (3 AMBE+2 frames, 60 ms) into 480 float
    // samples at 8 kHz. Returns the number of bit errors corrected by FEC.
    int decodeBurst(const QByteArray &payload33, float *out480);

private:
    mbe_parms cur_{}, prev_{}, prevEnh_{};
};

// ---------------------------------------------------------------- D-Star
// DSRP packets between MMDVMHost and ircDDBGateway.
struct DStarPacket {
    bool fromRf = false;        // true: from the radio, false: from the network
    bool isHeader = false;
    bool isEnd = false;         // last frame of the transmission
    uint16_t id = 0;            // stream id
    uint8_t seq = 0;            // 0..20 within a superframe
    // header fields (trimmed)
    QString rpt1, rpt2, your, my, mySuffix;
    // voice frame
    QByteArray ambe;            // 9 bytes
    QByteArray slowData;        // 3 bytes (scrambled, as sent)
};

// Parses a sniffer record: 'R'|'N' + "DSRP" packet.
bool parseDstr(const QByteArray &rec, DStarPacket &out);

class DStarDecoder {
public:
    DStarDecoder() { reset(); }
    void reset();
    // One 20 ms AMBE frame -> 160 float samples at 8 kHz; returns corrected bit errors.
    int decodeFrame(const QByteArray &ambe9, float *out160);
private:
    mbe_parms cur_{}, prev_{}, prevEnh_{};
};

// Reassembles the 20-character slow-data text message.
class SlowDataText {
public:
    void reset();
    void feed(uint8_t seq, const QByteArray &slow3);
    QString text() const;          // empty until at least one block is received
private:
    uint8_t half_[3]{};
    bool haveHalf_ = false;
    char chars_[20]{};
    bool got_[4]{};
};

// ---------------------------------------------------------------- YSF (System Fusion)
struct YsfPacket {
    bool fromRf = false;
    QString gateway, src, dst;  // 10-char callsign fields from the YSFD packet
    bool end = false;
    int count = 0;
    QByteArray frame;           // 33-byte DMR data burst carrying a full LC (voice LC header: dataType 1, terminator: 2),
// with BPTC(196,96), RS(12,9) + CRC mask, slot type (colour code cc) and BS data sync.
// lc9 = FLCO, FID, options, dst(3), src(3).
QByteArray dmrEncodeLcBurst(const uint8_t lc9[9], int dataType, int cc);
// 120-byte YSF frame: sync(5) FICH(25) payload(90)
    bool fichOk = false;
    int fi = -1;                // 0 header, 1 communications, 2 terminator, 3 test
    int dt = -1;                // 0 V/D mode 1, 1 data FR, 2 V/D mode 2 (DN), 3 voice FR (VW)
    int fn = 0, ft = 0, dgid = 0;
};

// Parses a sniffer record: 'R'|'N' + 155-byte "YSFD" packet.
bool parseYsf(const QByteArray &rec, YsfPacket &out);

class YsfDecoder {
public:
    YsfDecoder() { reset(); }
    void reset();
    // V/D mode 2 (DN): 5 x 20 ms AMBE+2 frames -> 800 samples. Returns the
    // number of repetition-code triplets that needed correcting.
    int decodeVD2(const QByteArray &frame120, float *out800);
private:
    mbe_parms cur_{}, prev_{}, prevEnh_{};
};

// ---------------------------------------------------------------- P25
class P25Decoder {
public:
    P25Decoder() { reset(); }
    void reset();
    // One 11-byte network IMBE value (88 bits, u0..u7) -> 160 samples.
    void decodeImbe(const char *imbe11, float *out160);
private:
    mbe_parms cur_{}, prev_{}, prevEnh_{};
};

// ---------------------------------------------------------------- NXDN
// MMDVMHost <-> NXDNGateway "ICOM" packets (Icom protocol): 33 bytes of NXDN
// data at offset 40: LICH(1) SACCH(4) then two 14-byte halves, each holding
// either FACCH1 or two 49-bit AMBE+2 voice frames (FEC already removed).
struct NxdnPacket {
    bool fromRf = false;
    uint8_t usc = 0;        // 0 SACCH_NS (header/trailer), 1 UDCH (data), 2 SACCH_SS (voice)
    uint8_t option = 0;     // 3 = both halves voice, 1 = FACCH in 1st half, 2 = FACCH in 2nd half
    QByteArray data;        // 33 bytes
};
bool parseNxdn(const QByteArray &rec, NxdnPacket &out);

class NxdnDecoder {
public:
    NxdnDecoder() { reset(); }
    void reset();
    // One 14-byte half (2 x 20 ms AMBE+2) -> 320 samples.
    void decodeHalf(const uint8_t *half14, float *out320);
private:
    mbe_parms cur_{}, prev_{}, prevEnh_{};
};

// ---------------------------------------------------------------- M17
// MMDVMHost <-> M17Gateway 54-byte "M17 " stream frames:
// sid(2) LSF[dst(6) src(6) type(2) meta(14)] FN(2) payload(16) CRC(2)
struct M17Packet {
    bool fromRf = false;
    uint16_t sid = 0;
    QString src, dst;
    int dataType = 0;       // 1 data, 2 voice (Codec2 3200), 3 voice+data (Codec2 1600)
    int encType = 0;        // 0 none
    bool end = false;
    QByteArray payload;     // 16 bytes
};
bool parseM17(const QByteArray &rec, M17Packet &out);
QString m17Callsign(const uint8_t *enc6);

class M17Decoder {
public:
    M17Decoder();
    ~M17Decoder();
    M17Decoder(const M17Decoder &) = delete;
    M17Decoder &operator=(const M17Decoder &) = delete;
    static bool available();   // built with Codec2?
    void reset();
    // 40 ms of audio -> 320 samples; returns the number of samples (0 if no decoder).
    int decode(const M17Packet &p, float *out320);
private:
    void *c3200_ = nullptr, *c1600_ = nullptr;
};

// ---------------------------------------------------------------- encoders (test calls)
// 49-bit AMBE+2 frame (one bit per byte) -> 72-bit FEC'd frame laid out as mbelib's ambe_fr.
void ambe49ToFrame(const uint8_t ambe49[49], char fr[4][24]);
// Three 49-bit frames -> 33-byte DMR voice burst; mid48 = sync or EMB (48 bits, 6 bytes).
QByteArray dmrEncodeBurst(const uint8_t ambe49[3][49], const uint8_t mid48[6]);
// 33-byte DMR data burst carrying a full LC (voice LC header: dataType 1, terminator: 2),
// with BPTC(196,96), RS(12,9) + CRC mask, slot type (colour code cc) and BS data sync.
// lc9 = FLCO, FID, options, dst(3), src(3).
QByteArray dmrEncodeLcBurst(const uint8_t lc9[9], int dataType, int cc);
// 120-byte YSF frame (sync + FICH + payload). With ambe49 != nullptr the payload is
// V/D mode 2 voice (5 frames); otherwise it is left empty.
QByteArray ysfEncodeFrame(int fi, int fn, int ft, int dt, const uint8_t (*ambe49)[49]);

// Adaptive noise gate for decoded speech: tracks the background level between words
// and turns it down when nobody is speaking. Works on 8 kHz float audio in place.
class NoiseGate {
public:
    struct Params {
        double thresholdDb = 10;   // open when this far above the background
        double reductionDb = 25;   // how much to turn the background down
        int holdMs = 250;          // stay open this long after speech
    };
    void reset();
    void process(float *pcm, int n, const Params &p);
    bool isOpen() const { return gain_ > 0.5; }
private:
    std::deque<double> recent_;   // block RMS over the last 2 s; background = the quietest
    int holdLeft_ = 0;            // samples
    double gain_ = 1.0;   // current gain (smoothed)
};

// Peak AGC (fast attack, slow release) followed by a user volume, -> int16.
class Agc {
public:
    void process(const float *in, int n, double volume, std::vector<int16_t> &out, double &rmsOut);
private:
    double gain_ = 20.0;
};
