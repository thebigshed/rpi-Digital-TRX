#pragma once
// Streaming voice encoders for PTT: 20 ms of 8 kHz audio in, network records out, for
// each digital mode. Records carry the same tag as received ones (e.g. "DSTRN" + DSRP
// packet, or a bare DMRD packet), so our own transmission is logged through the decoder.
#include <QByteArray>
#include <QList>
#include <QString>
#include <cstdint>
#include <memory>

#include "Settings.h"

struct TxTarget {
    QString mode;                 // DMR, D-Star, YSF, P25, NXDN, M17
    uint32_t dst = 9;             // DMR talkgroup / private ID, P25 and NXDN talkgroup
    bool privateCall = false;     // DMR
    QString dstarYour = "CQCQCQ"; // D-Star UR field
    QString dstarSuffix;          // MY suffix (4 chars)
    QString m17Dst;               // empty = ALL (the linked reflector)
    QString ysfDst = "ALL";
    QString text;                 // D-Star slow-data message (20 chars)
};

class VoiceEncoder {
public:
    virtual ~VoiceEncoder() = default;
    static std::unique_ptr<VoiceEncoder> create(const Settings &cfg, const TxTarget &target, QString *err = nullptr);

    virtual QList<QByteArray> begin() = 0;                       // header
    virtual QList<QByteArray> frame(const int16_t *pcm160) = 0;   // zero or more records
    virtual QList<QByteArray> end() = 0;                          // pad, flush, terminator
    virtual int recordIntervalMs() const = 0;                     // real-time pacing for a whole buffer
};
