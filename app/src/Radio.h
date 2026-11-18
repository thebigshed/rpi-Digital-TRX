#pragma once
// The radio: network clients, decoding, calls, transmit, call logs and recordings. It has no user
// interface of its own. The FrontPanel (FrontPanel.h) shows its state on the LCD and drives it
// through the public functions below.
#include <QElapsedTimer>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "AudioIn.h"
#include "AudioOut.h"
#include "DmrDecoder.h"
#include "Networks.h"
#include "Settings.h"
#include "VoiceEncoder.h"

// The call shown on the front panel: the one that has the speaker, otherwise the newest.
struct OnAir {
    QString mode;            // DMR, D-Star, YSF, P25, NXDN, M17; empty = nothing on air
    QString call, name, target;
    qint64 elapsedMs = 0;
    bool tx = false;         // our own transmission
};

class Radio : public QObject {
    Q_OBJECT
public:
    static const QStringList kModes;   // voice modes, in display order

    // persist = false for test runs (--replay): settings, logs and recordings are left alone.
    explicit Radio(bool persist = true, QObject *parent = nullptr);
    ~Radio() override;

    void start();                          // log in to the networks (unless a Pi-Star is online)
    void startReplay(const QString &path); // test aid: feed a capture instead
    void shutdown();                       // end calls, stop transmitting, unlink

    // ---- state for the front panel
    const Settings &settings() const { return cfg_; }
    QStringList txModes() const { return txModes_; }   // modes PTT can transmit in
    QString txMode() const;
    void setTxMode(const QString &mode);               // remembered
    QString endpoint(const QString &mode) const;       // what it is linked to, or why not ("" = not linked)
    QString currentLink(const QString &mode) const;    // link target, also while still connecting
    bool linked(const QString &mode) const;
    QList<HostEntry> hostList(const QString &mode) const;
    void link(const QString &mode, const QString &target);   // "" = unlink; remembered as the start-up link
    void setDmrTarget(uint32_t id, bool privateCall);        // where DMR transmits; remembered
    bool modeEnabled(const QString &mode) const { return modeAllowed(mode); }   // connected (settings.json)
    bool rxOn(const QString &mode) const { return cfg_.network.rxModes.contains(mode); }
    void setRx(const QString &mode, bool on);                // listen to the mode or not; remembered
    bool networkFault() const;                               // a link has failed, or the Pi-Star is online
    bool txActive() const { return txActive_; }
    qint64 txElapsedMs() const;
    bool piStarOnline() const { return piStarOnline_; }
    const OnAir &onAir() const { return onAir_; }
    int volume() const { return cfg_.audio.volume; }   // percent, 0..150
    bool muted() const { return cfg_.audio.mute; }
    void setVolume(int percent);
    void setMuted(bool on);

    void startTx();
    void stopTx(const QString &why = {});
    void logEvent(const QString &level, const QString &text);   // events.log

signals:
    void ticked();                                    // every 100 ms, after call timeouts: redraw
    void txRefused(const QString &why);               // PTT pressed but we cannot transmit
    void page(uint32_t ric, const QString &text);     // a page to one of my RICs

private:
    struct Call {   // DMR, one per slot
        QByteArray stream;
        uint32_t src = 0, dst = 0;
        int slot = 2;
        bool privateCall = false, lateEntry = false, tx = false;
        QDateTime started;
        QElapsedTimer timer;
        qint64 lastPacketMs = 0;
        int frames = 0, errors = 0;
    };
    struct DStarCall {
        uint16_t id = 0;
        bool fromRf = false, lateEntry = false, tx = false;
        QString my, suffix, your, rpt1, rpt2;
        QString reflector;   // linked reflector the call went through, if any
        QDateTime started;
        QElapsedTimer timer;
        qint64 lastPacketMs = 0;
        int frames = 0, errors = 0;
        SlowDataText text;
        DStarDecoder decoder;   // per stream: two streams can be interleaved
    };
    struct GenCall {   // YSF, P25, NXDN and M17
        QString mode;
        bool fromRf = false, lateEntry = false, tx = false;
        bool audio = true;   // false for calls we can't decode
        QString callsign, target, note;
        uint32_t srcId = 0, dstId = 0;
        QDateTime started;
        QElapsedTimer timer;
        qint64 lastPacketMs = 0;
        int frames = 0, errors = 0, errorBits = 0;
        uint16_t streamId = 0;   // M17
        uint8_t l3[23]{};        // NXDN: layer 3 VCALL reassembled from SACCH (late entry)
        int l3Mask = 0;
    };

    // decoding (Radio.cpp)
    void dispatchRecord(const QByteArray &rec);
    bool modeAllowed(const QString &mode) const;
    bool hear(const QString &mode) const;
    void handlePacket(const QByteArray &raw);
    void beginCall(const DmrPacket &p, bool lateEntry);
    void endCall(int slot, const QString &why);
    void handleDStar(const QByteArray &rec);
    DStarCall &beginDStar(const DStarPacket &p, bool lateEntry);
    void endDStar(uint16_t id, const QString &why);
    void endAllDStar(const QString &why);
    void handleYsf(const QByteArray &rec);
    void handleP25(const QByteArray &rec);
    void handleNxdn(const QByteArray &rec);
    void handleM17(const QByteArray &rec);
    void beginGen(std::optional<GenCall> &slot, const QString &mode, bool fromRf, bool lateEntry);
    void endGen(std::optional<GenCall> &slot, const QString &why);
    void handlePage(const QByteArray &logLine);
    void endAllCalls(const QString &why);
    void tick();
    void followRx(const Call *c);

    // audio
    bool claimAudio(const QString &key);
    void releaseAudio(const QString &key);
    void playDecoded(const QString &key, const float *samples, int n);
    QString saveRecording(const QString &key, const QDateTime &started, const QString &mode, const QString &who);
    void pruneRecordings();

    // names
    QString callsign(uint32_t id) const;
    QString target(const Call &c) const;
    QString dstarName(const QString &call) const;
    static QString dstarCaller(const DStarCall &c);
    static QString dstarTarget(const DStarCall &c);
    static QString dstarKey(uint16_t id) { return QString("DSTAR%1").arg(id); }
    QString genName(const GenCall &c) const;
    QString nxdnCall(uint32_t id) const;

    // transmit
    void onMicFrame(const std::vector<int16_t> &frame);
    bool txTarget(TxTarget &t, QString &err) const;
    void refreshTxModes();
    void txSend(const QByteArray &record);

    // networks (RadioNet.cpp)
    NetClient *client(const QString &mode) const;
    void checkPiStar();
    void startNetworks();
    void stopNetworks();
    void onNetChanged();
    void loadIds();
    void loadNxdnIds();

    // files
    void openLogFiles();
    void saveSettings();
    void saveSettingsSoon();   // for knob turns: one write after the knob stops
    void setStatus(const QString &text, bool error = false);
    static void appendCsv(QFile &f, const QString &line);

    Settings cfg_;
    bool persist_ = true;
    bool quitting_ = false;
    QTimer tickTimer_, saveTimer_;
    QString lastStatus_;

    // decoding
    AmbeDecoder decoder_;
    QSet<QByteArray> seen_;
    std::deque<QByteArray> seenOrder_;
    std::optional<Call> active_[3];   // by slot 1/2
    std::map<uint16_t, DStarCall> dstars_;
    YsfDecoder ysfDecoder_;
    P25Decoder p25Decoder_;
    NxdnDecoder nxdnDecoder_;
    M17Decoder m17Decoder_;
    std::optional<GenCall> ysf_, p25_, nxdn_, m17_;
    QString reflector_;   // linked D-Star reflector, e.g. "REF001 C"
    OnAir onAir_;
    QString onAirMode_;   // last mode followed by TX

    // names
    QHash<uint32_t, QPair<QString, QString>> ids_;       // DMR id -> (callsign, name)
    QHash<QString, QString> nameByCall_;                 // callsign -> name
    QHash<uint32_t, QPair<QString, QString>> nxdnIds_;   // NXDN id -> (callsign, name)

    // audio
    std::unique_ptr<AudioOut> out_;
    qint64 audioRetryMs_ = 0;
    Agc agc_;
    NoiseGate gate_;
    QString audioOwner_;    // key of the call that has the speaker: DMR1/DMR2/DSTAR<id>/YSF/P25/NXDN/M17
    QString holdChannel_;   // channel held for a reply after a call ends
    qint64 holdUntilMs_ = 0;
    QHash<QString, std::vector<float>> recBuf_;   // decoded audio of each call in progress
    QString recDir_;

    // logs
    QFile dmrLog_, dstarLog_, genLog_, pageLog_;
    QSet<QByteArray> seenPages_;
    QSet<uint32_t> myRics_;

    // transmit
    QStringList txModes_;
    bool txActive_ = false;
    bool ownTx_ = false;   // records being dispatched are our own transmission
    qint64 txStartMs_ = 0, txMuteUntilMs_ = 0;
    double micGain_ = 1.0;
    std::unique_ptr<VoiceEncoder> txEnc_;
    std::unique_ptr<AudioIn> mic_;

    // networks
    std::unique_ptr<HostFiles> hosts_;
    std::map<QString, std::unique_ptr<NetClient>> nets_;
    std::unique_ptr<DapnetClient> dapnet_;
    QNetworkAccessManager piNet_;
    QTimer piTimer_, relinkTimer_;
    bool piStarOnline_ = false, piChecked_ = false, networksUp_ = false;
    QHash<QString, QString> lastNetState_;   // per mode, to log only real changes
};
