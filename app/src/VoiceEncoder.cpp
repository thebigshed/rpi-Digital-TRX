#include "VoiceEncoder.h"
#include "AmbeChip.h"
#include "DmrDecoder.h"
#include "../third_party/op25_vocoder/op25voc.h"   // not on the include path: OP25 ships its own mbelib.h

#include <QDateTime>
#include <array>
#include <cctype>
#include <cstring>
#include <vector>

#ifdef HAVE_CODEC2
#include <codec2/codec2.h>
#endif

namespace {
QByteArray be(uint32_t v, int bytes)
{
    QByteArray b(bytes, 0);
    for (int i = 0; i < bytes; ++i) b[i] = char(v >> (8 * (bytes - 1 - i)));
    return b;
}
QByteArray padded(const QString &s, int n)
{
    QByteArray b = s.toLatin1().left(n);
    return b + QByteArray(n - b.size(), ' ');
}
uint16_t streamId() { return uint16_t(QDateTime::currentMSecsSinceEpoch() & 0xFFFF) | 1; }
const int16_t kSilence[160] = {};

// 49-bit AMBE+2 encoder (DMR, YSF, NXDN)
class Ambe49 {
public:
    Ambe49() : h_(op25voc_ambe_new(0)) {}
    ~Ambe49() { op25voc_ambe_free(h_); }
    std::array<uint8_t, 49> encode(const int16_t *pcm)
    {
        std::array<uint8_t, 49> f{};
        AmbeChip *chip = AmbeChip::instance();   // the AMBE chip if there is one (--ambe)
        if (!chip || !chip->encode2450(pcm, f.data())) op25voc_ambe_encode(h_, pcm, f.data());
        return f;
    }
private:
    void *h_;
};

// ---------------------------------------------------------------- DMR
class DmrEncoder : public VoiceEncoder {
public:
    DmrEncoder(const Settings &c, const TxTarget &t) : c_(c), t_(t), stream_(be(0x7E570000u | streamId(), 4))
    {
        const uint32_t me = c.station.dmrId;
        const uint8_t lc[9] = {uint8_t(t.privateCall ? 0x03 : 0x00), 0, 0, uint8_t(t.dst >> 16), uint8_t(t.dst >> 8),
                               uint8_t(t.dst), uint8_t(me >> 16), uint8_t(me >> 8), uint8_t(me)};
        std::memcpy(lc_, lc, 9);
    }
    QList<QByteArray> begin() override
    {
        QList<QByteArray> out;
        for (int i = 0; i < 3; ++i) out << record(2, 1, dmrEncodeLcBurst(lc_, 1, c_.station.colourCode));
        return out;
    }
    QList<QByteArray> frame(const int16_t *pcm) override
    {
        frames_.push_back(ambe_.encode(pcm));
        if (frames_.size() < 3) return {};
        return {burst()};
    }
    QList<QByteArray> end() override
    {
        QList<QByteArray> out;
        if (!frames_.empty()) {
            while (frames_.size() < 3) frames_.push_back(ambe_.encode(kSilence));
            out << burst();
        }
        out << record(2, 2, dmrEncodeLcBurst(lc_, 2, c_.station.colourCode));
        return out;
    }
    int recordIntervalMs() const override { return 60; }

private:
    QByteArray burst()
    {
        static const uint8_t kVoiceSync[6] = {0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7};
        static const uint8_t kEmb[6] = {};
        uint8_t b[3][49];
        for (int k = 0; k < 3; ++k) std::memcpy(b[k], frames_[size_t(k)].data(), 49);
        frames_.clear();
        const int n = burst_++ % 6;                                       // A..F
        return record(n == 0 ? 1 : 0, n, dmrEncodeBurst(b, n == 0 ? kVoiceSync : kEmb));
    }
    QByteArray record(int frameType, int dataType, const QByteArray &payload)
    {
        const uint8_t flags = uint8_t((c_.station.dmrSlot == 2 ? 0x80 : 0) | (t_.privateCall ? 0x40 : 0) |
                                      (frameType << 4) | dataType);
        return QByteArray("DMRD") + char(seq_++) + be(c_.station.dmrId, 3) + be(t_.dst, 3) +
               be(c_.station.repeaterId, 4) + char(flags) + stream_ + payload + QByteArray(2, 0);
    }
    const Settings &c_;
    TxTarget t_;
    QByteArray stream_;
    uint8_t lc_[9];
    uint8_t seq_ = 0;
    int burst_ = 0;
    Ambe49 ambe_;
    std::vector<std::array<uint8_t, 49>> frames_;
};

// ---------------------------------------------------------------- D-Star
class DStarEncoder : public VoiceEncoder {
public:
    DStarEncoder(const Settings &c, const TxTarget &t) : c_(c), t_(t), id_(be(streamId(), 2)), h_(op25voc_ambe_new(1))
    {
        msg_ = padded(t.text, 20);
    }
    ~DStarEncoder() override { op25voc_ambe_free(h_); }
    QList<QByteArray> begin() override
    {
        // flags(3) RPT2 RPT1 YOUR MY MY-suffix, then the CRC (CCITT reflected, init 0xFFFF, inverted, LSB first)
        const QString call = c_.station.callsign;
        QByteArray h = QByteArray(3, 0) + padded(padded(call, 7) + "G", 8) +
                       padded(padded(call, 7) + c_.station.dstarModule, 8) + padded(t_.dstarYour, 8) +
                       padded(call, 8) + padded(t_.dstarSuffix, 4);
        uint16_t crc = 0xFFFF;
        for (char ch : h) {
            crc ^= uint8_t(ch);
            for (int b = 0; b < 8; ++b) crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0x8408) : uint16_t(crc >> 1);
        }
        crc = uint16_t(~crc);
        h += char(crc & 0xFF);
        h += char(crc >> 8);
        return {QByteArray("DSTRN") + "DSRP" + char(0x20) + id_ + char(0) + h};
    }
    QByteArray encodeFrame(const int16_t *pcm)
    {
        QByteArray ambe;
        if (AmbeChip *chip = AmbeChip::instance(); chip && chip->encodeDStar(pcm, ambe)) return ambe;
        uint8_t bits[72];
        op25voc_ambe_encode(h_, pcm, bits);
        ambe = QByteArray(9, 0);
        for (int i = 0; i < 72; ++i)
            if (bits[i]) ambe[i >> 3] = char(uint8_t(ambe[i >> 3]) | (1 << (i & 7)));   // on-air order, LSB first
        return ambe;
    }
    QList<QByteArray> frame(const int16_t *pcm) override
    {
        const QByteArray ambe = encodeFrame(pcm);
        // Hold one frame back so the last one can carry the end flag.
        QList<QByteArray> out;
        if (!held_.isEmpty()) out << voice(held_, false);
        held_ = ambe;
        return out;
    }
    QList<QByteArray> end() override
    {
        if (held_.isEmpty()) held_ = encodeFrame(kSilence);
        return {voice(held_, true)};
    }
    int recordIntervalMs() const override { return 20; }

private:
    QByteArray voice(const QByteArray &ambe, bool last)
    {
        const int seq = n_++ % 21;
        return QByteArray("DSTRN") + "DSRP" + char(0x21) + id_ + char(uint8_t(seq | (last ? 0x40 : 0))) + char(0) +
               ambe + slow(seq);
    }
    QByteArray slow(int seq) const
    {
        // Frame 0 of each superframe = sync; frames 1..8 = 4 text blocks; rest = filler.
        static const uint8_t scramble[3] = {0x70, 0x4F, 0x93};
        uint8_t b[3] = {0x66, 0x66, 0x66};
        if (seq == 0) return QByteArray("\x55\x2D\x16", 3);
        if (seq <= 8) {
            const int blk = (seq - 1) / 2;
            const uint8_t six[6] = {uint8_t(0x40 | blk), uint8_t(msg_[blk * 5]), uint8_t(msg_[blk * 5 + 1]),
                                    uint8_t(msg_[blk * 5 + 2]), uint8_t(msg_[blk * 5 + 3]), uint8_t(msg_[blk * 5 + 4])};
            std::memcpy(b, six + ((seq - 1) % 2) * 3, 3);
        }
        for (int i = 0; i < 3; ++i) b[i] ^= scramble[i];
        return QByteArray(reinterpret_cast<const char *>(b), 3);
    }
    const Settings &c_;
    TxTarget t_;
    QByteArray id_, msg_, held_;
    void *h_;
    int n_ = 0;
};

// ---------------------------------------------------------------- YSF (V/D mode 2)
class YsfEncoder : public VoiceEncoder {
public:
    YsfEncoder(const Settings &c, const TxTarget &t) : c_(c), t_(t) {}
    QList<QByteArray> begin() override { return {record(ysfEncodeFrame(0, 0, 6, 2, nullptr), false)}; }
    QList<QByteArray> frame(const int16_t *pcm) override
    {
        frames_.push_back(ambe_.encode(pcm));
        if (frames_.size() < 5) return {};
        return {comms()};
    }
    QList<QByteArray> end() override
    {
        QList<QByteArray> out;
        if (!frames_.empty()) {
            while (frames_.size() < 5) frames_.push_back(ambe_.encode(kSilence));
            out << comms();
        }
        out << record(ysfEncodeFrame(2, 0, 6, 2, nullptr), true);
        return out;
    }
    int recordIntervalMs() const override { return 100; }

private:
    QByteArray comms()
    {
        uint8_t five[5][49];
        for (int k = 0; k < 5; ++k) std::memcpy(five[k], frames_[size_t(k)].data(), 49);
        frames_.clear();
        const QByteArray r = record(ysfEncodeFrame(1, fn_, 6, 2, five), false);
        fn_ = (fn_ + 1) % 7;
        return r;
    }
    QByteArray record(const QByteArray &frame, bool end)
    {
        return QByteArray("YSFXN") + "YSFD" + padded(c_.station.callsign, 10) + padded(c_.station.callsign, 10) +
               padded(t_.ysfDst, 10) + char(((count_++ & 0x7F) << 1) | (end ? 1 : 0)) + frame;
    }
    const Settings &c_;
    TxTarget t_;
    Ambe49 ambe_;
    std::vector<std::array<uint8_t, 49>> frames_;
    int fn_ = 0, count_ = 0;
};

// ---------------------------------------------------------------- P25
class P25Encoder : public VoiceEncoder {
public:
    P25Encoder(const Settings &c, const TxTarget &t) : c_(c), t_(t), h_(op25voc_imbe_new()) {}
    ~P25Encoder() override { op25voc_imbe_free(h_); }
    QList<QByteArray> begin() override { return {}; }   // LDU1 carries the header information
    QList<QByteArray> frame(const int16_t *pcm) override { return {record(pcm)}; }
    QList<QByteArray> end() override
    {
        QList<QByteArray> out;
        while (i_ % 18 != 0) out << record(kSilence);    // finish the LDU1+LDU2 pair
        out << (QByteArray("P25XN") + char(0x80) + QByteArray(16, 0));
        return out;
    }
    int recordIntervalMs() const override { return 20; }

private:
    QByteArray record(const int16_t *pcm)
    {
        // MMDVMHost P25Network record templates (REC62..REC73); IMBE goes at kOff.
        static const uint8_t kTpl[18][22] = {
            {0x62, 0x02, 0x02, 0x0C, 0x0B, 0x12, 0x64, 0x00, 0x00, 0x80},
            {0x63, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x64, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x65, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x66, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x67, 0xF0, 0x9D, 0x6A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x68, 0x19, 0xD4, 0x26, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x69, 0xE0, 0xEB, 0x7B, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x6A, 0x00, 0x00, 0x02},
            {0x6B, 0x02, 0x02, 0x0C, 0x0B, 0x12, 0x64, 0x00, 0x00, 0x80},
            {0x6C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x6D, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x6E, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x6F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x70, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},     // ALGID 0x80 = clear
            {0x71, 0xAC, 0xB8, 0xA4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x72, 0x9B, 0xDC, 0x75, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02},
            {0x73, 0x00, 0x00, 0x02}};
        static const int kLen[18] = {22, 14, 17, 17, 17, 17, 17, 17, 16, 22, 14, 17, 17, 17, 17, 17, 17, 16};
        static const int kOff[18] = {10, 1, 5, 5, 5, 5, 5, 5, 4, 10, 1, 5, 5, 5, 5, 5, 5, 4};
        static const int kBits[8] = {12, 12, 12, 12, 11, 11, 11, 7};
        int16_t u[8];
        op25voc_imbe_encode(h_, pcm, u);
        uint8_t imbe[11] = {};
        int pos = 0;
        for (int k = 0; k < 8; ++k)
            for (int b = kBits[k] - 1; b >= 0; --b, ++pos)
                if ((u[k] >> b) & 1) imbe[pos >> 3] |= uint8_t(0x80 >> (pos & 7));
        const int i = i_++ % 18;
        QByteArray r(reinterpret_cast<const char *>(kTpl[i]), kLen[i]);
        if (i == 3) r.replace(1, 3, be(t_.dst, 3));                       // 0x65: talkgroup
        if (i == 4) r.replace(1, 3, be(c_.station.dmrId, 3));             // 0x66: source
        r.replace(kOff[i], 11, QByteArray(reinterpret_cast<const char *>(imbe), 11));
        return QByteArray("P25XN") + r;
    }
    const Settings &c_;
    TxTarget t_;
    void *h_;
    int i_ = 0;
};

// ---------------------------------------------------------------- NXDN
class NxdnEncoder : public VoiceEncoder {
public:
    NxdnEncoder(const Settings &c, const TxTarget &t)
    {
        const uint32_t me = c.station.nxdnId;
        const uint8_t l3[9] = {0x01, 0x00, 0x20, uint8_t(me >> 8), uint8_t(me), uint8_t(t.dst >> 8), uint8_t(t.dst), 0, 0};
        std::memcpy(l3_, l3, 9);
    }
    QList<QByteArray> begin() override { return {facch(0x01)}; }      // VCALL
    QList<QByteArray> frame(const int16_t *pcm) override
    {
        frames_.push_back(ambe_.encode(pcm));
        if (frames_.size() < 4) return {};
        return {voice()};
    }
    QList<QByteArray> end() override
    {
        QList<QByteArray> out;
        if (!frames_.empty()) {
            while (frames_.size() < 4) frames_.push_back(ambe_.encode(kSilence));
            out << voice();
        }
        out << facch(0x08);                                               // TX_REL
        return out;
    }
    int recordIntervalMs() const override { return 80; }

private:
    static void setbit(uint8_t *p, int i, int v)
    {
        if (v) p[i >> 3] |= uint8_t(0x80 >> (i & 7)); else p[i >> 3] &= uint8_t(~(0x80 >> (i & 7)));
    }
    static QByteArray icom(const uint8_t d[33])
    {
        QByteArray b(102, 0);
        std::memcpy(b.data(), "ICOM\x01\x01\x08\xE0", 8);
        std::memcpy(b.data() + 40, d, 33);
        return QByteArray("NXDXN") + b;
    }
    QByteArray facch(uint8_t type) const
    {
        uint8_t d[33] = {};
        d[0] = 0x81;                          // RFCT RDCH, USC SACCH_NS, FACCH in both halves, inbound, parity
        std::memcpy(d + 5, l3_, 9);
        d[5] = type;
        std::memcpy(d + 19, d + 5, 14);
        return icom(d);
    }
    QByteArray voice()
    {
        uint8_t d[33] = {};
        d[0] = uint8_t((2 << 6) | (2 << 4) | (3 << 2));   // RDCH, SACCH_SS, both halves voice
        const int structure = 3 - (sf_++ % 4);           // 1/4 .. 4/4
        d[1] = uint8_t(structure << 6);
        const int part = 3 - structure;
        for (int i = 0; i < 18; ++i)
            setbit(d + 2, i, (l3_[(part * 18 + i) >> 3] >> (7 - ((part * 18 + i) & 7))) & 1);
        for (int k = 0; k < 4; ++k) {
            uint8_t *half = d + 5 + (k / 2) * 14;
            for (int b = 0; b < 49; ++b) setbit(half, (k % 2) * 49 + b, frames_[size_t(k)][size_t(b)]);
        }
        frames_.clear();
        return icom(d);
    }
    uint8_t l3_[9];
    Ambe49 ambe_;
    std::vector<std::array<uint8_t, 49>> frames_;
    int sf_ = 0;
};

// ---------------------------------------------------------------- M17
uint64_t m17Encode(const QString &s)
{
    static const char *alpha = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
    const QByteArray cs = s.toLatin1();
    uint64_t e = 0;
    for (int i = int(cs.size()) - 1; i >= 0; --i) {
        const char *p = cs[i] ? std::strchr(alpha, std::toupper(static_cast<unsigned char>(cs[i]))) : nullptr;
        e = e * 40 + uint64_t(p ? p - alpha : 0);   // characters outside the alphabet encode as a space
    }
    return e;
}

#ifdef HAVE_CODEC2
class M17Encoder : public VoiceEncoder {
public:
    M17Encoder(const Settings &c, const TxTarget &t) : c2_(codec2_create(CODEC2_MODE_3200)), sid_(streamId())
    {
        src_ = m17Encode(c.station.callsign);
        dst_ = t.m17Dst.isEmpty() ? 0xFFFFFFFFFFFFULL : m17Encode(t.m17Dst);
    }
    ~M17Encoder() override { codec2_destroy(c2_); }
    QList<QByteArray> begin() override { return {}; }
    QList<QByteArray> frame(const int16_t *pcm) override
    {
        uint8_t half[8];
        std::vector<short> in(pcm, pcm + 160);
        codec2_encode(c2_, half, in.data());
        pending_.append(reinterpret_cast<const char *>(half), 8);
        if (pending_.size() < 16) return {};
        QList<QByteArray> out;
        if (!held_.isEmpty()) out << packet(held_, false);   // hold one back for the end flag
        held_ = pending_;
        pending_.clear();
        return out;
    }
    QList<QByteArray> end() override
    {
        if (!pending_.isEmpty()) {                             // finish a half-full payload with silence
            uint8_t half[8];
            std::vector<short> in(160, 0);
            codec2_encode(c2_, half, in.data());
            pending_.append(reinterpret_cast<const char *>(half), 8);
            QList<QByteArray> out;
            if (!held_.isEmpty()) out << packet(held_, false);
            held_ = pending_;
            pending_.clear();
            out << packet(held_, true);
            return out;
        }
        if (held_.isEmpty()) return {};
        return {packet(held_, true)};
    }
    int recordIntervalMs() const override { return 40; }

private:
    QByteArray packet(const QByteArray &payload, bool last)
    {
        uint8_t m[54] = {'M', '1', '7', ' ', uint8_t(sid_ >> 8), uint8_t(sid_)};
        for (int i = 0; i < 6; ++i) { m[6 + i] = uint8_t(dst_ >> (40 - 8 * i)); m[12 + i] = uint8_t(src_ >> (40 - 8 * i)); }
        m[19] = 0x05;                                                    // stream, Codec2 3200 voice
        const uint16_t w = uint16_t((fn_++ & 0x7FFF) | (last ? 0x8000 : 0));
        m[34] = uint8_t(w >> 8);
        m[35] = uint8_t(w);
        std::memcpy(m + 36, payload.constData(), 16);
        uint16_t crc = 0xFFFF;                                           // M17 CRC-16, poly 0x5935
        for (int i = 0; i < 52; ++i) {
            crc ^= uint16_t(m[i] << 8);
            for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x5935) : uint16_t(crc << 1);
        }
        m[52] = uint8_t(crc >> 8);
        m[53] = uint8_t(crc);
        return QByteArray("M17XN") + QByteArray(reinterpret_cast<const char *>(m), 54);
    }
    CODEC2 *c2_;
    uint16_t sid_;
    uint64_t src_ = 0, dst_ = 0;
    QByteArray pending_, held_;
    uint16_t fn_ = 0;
};
#endif
}   // namespace

std::unique_ptr<VoiceEncoder> VoiceEncoder::create(const Settings &cfg, const TxTarget &t, QString *err)
{
    if (t.mode == "DMR") return std::make_unique<DmrEncoder>(cfg, t);
    if (t.mode == "D-Star") return std::make_unique<DStarEncoder>(cfg, t);
    if (t.mode == "YSF") return std::make_unique<YsfEncoder>(cfg, t);
    if (t.mode == "P25") return std::make_unique<P25Encoder>(cfg, t);
    if (t.mode == "NXDN") return std::make_unique<NxdnEncoder>(cfg, t);
#ifdef HAVE_CODEC2
    if (t.mode == "M17") return std::make_unique<M17Encoder>(cfg, t);
#endif
    if (err) *err = t.mode == "M17" ? QString("built without Codec2") : "unknown mode " + t.mode;
    return nullptr;
}
