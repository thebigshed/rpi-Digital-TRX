// The radio core: decoding, calls, audio, transmit, logs and recordings.
// Network connections are in RadioNet.cpp; the LCD and buttons in FrontPanel.cpp.
#include "Radio.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTime>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

const QStringList Radio::kModes = {"DMR", "D-Star", "YSF", "P25", "NXDN", "M17"};

static constexpr int kSeenMax = 2000;
static constexpr qint64 kHoldMs = 5000;   // speaker held on a channel after a call, for the reply

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

static QString csvQuote(QString s) { return "\"" + s.replace('"', "\"\"") + "\""; }

Radio::Radio(bool persist, QObject *parent) : QObject(parent), persist_(persist)
{
    cfg_ = Settings::load();
    saveSettings();   // writes any new defaults

    recDir_ = persist_ ? cfg_.recordingDir() : QDir::tempPath() + "/digital-transceiver-test-recordings";
    if (persist_) openLogFiles();
    for (const QString &part : cfg_.pager.myRics.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts)) {
        bool ok = false;
        const uint r = part.toUInt(&ok);
        if (ok) myRics_.insert(r);
    }
    logEvent("INFO", "Started");

    // Host lists and ID databases (downloaded once a day)
    hosts_ = std::make_unique<HostFiles>(cfg_.logDir() + "/hosts", this);
    connect(hosts_.get(), &HostFiles::updated, this, [this](const QString &file) {
        if (file == "DMRIds.dat") loadIds();
        if (file == "NXDN.csv") loadNxdnIds();
    });
    loadIds();
    loadNxdnIds();
    refreshTxModes();

    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(2000);
    connect(&saveTimer_, &QTimer::timeout, this, &Radio::saveSettings);
    connect(&tickTimer_, &QTimer::timeout, this, &Radio::tick);
    tickTimer_.start(100);
}

Radio::~Radio()
{
    if (saveTimer_.isActive()) saveSettings();
}

void Radio::shutdown()
{
    if (quitting_) return;
    quitting_ = true;
    logEvent("INFO", "Stopped");
    if (saveTimer_.isActive()) { saveTimer_.stop(); saveSettings(); }
    endAllCalls("closed");
    if (txActive_) stopTx("closing");
    mic_.reset();
    stopNetworks();   // unlink politely
    out_.reset();
}

void Radio::startReplay(const QString &path)
{
    // A capture of length-prefixed records (2-byte big-endian length, then the record).
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { setStatus("Cannot open " + path, true); return; }
    const QByteArray all = f.readAll();
    auto packets = std::make_shared<QList<QByteArray>>();
    for (int pos = 0; pos + 2 <= all.size();) {
        const int n = (uint8_t(all[pos]) << 8) | uint8_t(all[pos + 1]);
        if (pos + 2 + n > all.size()) break;
        if (n) packets->append(all.mid(pos + 2, n));
        pos += 2 + n;
    }
    setStatus(QString("Replaying %1 (%2 packets)").arg(path).arg(packets->size()));
    auto *t = new QTimer(this);
    auto idx = std::make_shared<int>(0);
    connect(t, &QTimer::timeout, this, [this, t, packets, idx] {
        if (*idx >= packets->size()) { t->stop(); setStatus("Replay finished"); return; }
        dispatchRecord(packets->at((*idx)++));
    });
    t->start(20);
}

// ---------------------------------------------------------------- settings and files
void Radio::saveSettings()
{
    if (!persist_) return;
    QString err;
    if (!cfg_.save(&err)) setStatus("Could not save settings: " + err, true);
}

void Radio::saveSettingsSoon()
{
    saveTimer_.start();
}

void Radio::setStatus(const QString &text, bool error)
{
    if (text == lastStatus_) return;
    lastStatus_ = text;
    logEvent(error ? "ERROR" : "INFO", text);
    if (qEnvironmentVariableIntValue("DT_DEBUG")) {
        fprintf(stdout, "%s %s\n", qPrintable(QTime::currentTime().toString("HH:mm:ss")), qPrintable(text));
        fflush(stdout);
    }
}

// Append to events.log in the log folder; rolls over to events.log.1 at 1 MB.
void Radio::logEvent(const QString &level, const QString &text)
{
    if (!persist_) return;
    const QString dir = cfg_.logDir();
    QDir().mkpath(dir);
    const QString path = dir + "/events.log";
    if (QFileInfo(path).size() > 1024 * 1024) {
        QFile::remove(path + ".1");
        QFile::rename(path, path + ".1");
    }
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    f.write(QString("%1 %2 %3\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"), level.leftJustified(5), text).toUtf8());
}

void Radio::openLogFiles()
{
    const QString dir = cfg_.logDir();
    QDir().mkpath(dir);
    struct F { QFile *f; const char *name; const char *header; };
    const F files[] = {
        {&dmrLog_, "calls.csv", "start,slot,src_id,callsign,name,target,duration_s,ber_pct\n"},
        {&dstarLog_, "dstar_calls.csv", "start,source,my,suffix,your,rpt1,rpt2,text,duration_s,ber_pct,reflector\n"},
        {&genLog_, "ysf_p25_calls.csv", "start,mode,source,callsign,name,target,duration_s,ber_pct,note\n"},
        {&pageLog_, "pages.csv", "time,ric,type,message\n"},
    };
    for (const F &x : files) {
        x.f->close();
        x.f->setFileName(dir + "/" + x.name);
        const bool fresh = !x.f->exists();
        if (x.f->open(QIODevice::Append | QIODevice::Text) && fresh) x.f->write(x.header);
    }
}

void Radio::appendCsv(QFile &f, const QString &line)
{
    if (!f.isOpen()) return;
    f.write(line.toUtf8() + '\n');
    f.flush();
}

void Radio::setVolume(int percent)
{
    percent = std::clamp(percent, 0, 150);
    if (percent == cfg_.audio.volume) return;
    cfg_.audio.volume = percent;
    saveSettingsSoon();
}

void Radio::setRx(const QString &mode, bool on)
{
    if (!kModes.contains(mode) || on == rxOn(mode)) return;
    QStringList &rx = cfg_.network.rxModes;
    if (on) {
        rx << mode;
    } else {
        rx.removeAll(mode);
        // stop anything being heard on it now
        if (mode == "DMR") for (int s = 1; s <= 2; ++s) endCall(s, "RX off");
        else if (mode == "D-Star") endAllDStar("RX off");
        else if (mode == "YSF") endGen(ysf_, "RX off");
        else if (mode == "P25") endGen(p25_, "RX off");
        else if (mode == "NXDN") endGen(nxdn_, "RX off");
        else if (mode == "M17") endGen(m17_, "RX off");
    }
    saveSettings();
}

void Radio::setDmrTarget(uint32_t id, bool privateCall)
{
    if (!id || (id == cfg_.transmit.dmrTalkgroup && privateCall == cfg_.transmit.dmrPrivate)) return;
    cfg_.transmit.dmrTalkgroup = id;
    cfg_.transmit.dmrPrivate = privateCall;
    saveSettings();
    setStatus(QString("DMR: transmitting to %1 %2").arg(privateCall ? "private" : "TG").arg(id));
}

void Radio::setMuted(bool on)
{
    if (on == cfg_.audio.mute) return;
    cfg_.audio.mute = on;
    saveSettingsSoon();
}

// ---------------------------------------------------------------- transmit
void Radio::refreshTxModes()
{
    txModes_.clear();
    for (const QString &m : kModes)
        if (modeAllowed(m)) txModes_ << m;
}

QString Radio::txMode() const
{
    return txModes_.contains(cfg_.transmit.mode) ? cfg_.transmit.mode : txModes_.value(0);
}

void Radio::setTxMode(const QString &mode)
{
    if (txActive_ || !txModes_.contains(mode) || mode == cfg_.transmit.mode) return;
    cfg_.transmit.mode = mode;
    saveSettings();
}

qint64 Radio::txElapsedMs() const
{
    return txActive_ ? nowMs() - txStartMs_ : 0;
}

bool Radio::txTarget(TxTarget &t, QString &err) const
{
    t.mode = txMode();
    t.dstarSuffix = cfg_.transmit.dstarSuffix;
    NetClient *c = client(t.mode);
    const QString linked = c && c->state() == NetClient::State::Linked ? c->endpoint() : QString();
    if (t.mode.isEmpty()) { err = "no modes enabled"; return false; }
    if (piStarOnline_) { err = "Pi-Star online"; return false; }
    if (t.mode == "DMR") {
        if (linked.isEmpty()) { err = "DMR not logged in"; return false; }
        t.dst = cfg_.transmit.dmrTalkgroup;
        t.privateCall = cfg_.transmit.dmrPrivate;
        return true;
    }
    if (linked.isEmpty()) { err = t.mode + " not linked"; return false; }
    if (t.mode == "P25" || t.mode == "NXDN") {
        bool ok = false;
        t.dst = linked.section(' ', 1, 1).toUInt(&ok);   // "TG 31672" / "TG 10 (Parrot)"
        if (!ok) { err = "unknown talkgroup"; return false; }
    }
    return true;   // D-Star CQCQCQ, YSF ALL and M17 ALL go to the linked reflector
}

void Radio::startTx()
{
    if (txActive_) return;
    TxTarget t;
    QString err;
    if (cfg_.stationReady(&err)) txTarget(t, err);
    else err = "set callsign/ID";
    if (err.isEmpty()) txEnc_ = VoiceEncoder::create(cfg_, t, &err);
    if (err.isEmpty()) {
        mic_ = std::make_unique<AudioIn>();
        if (!mic_->start(cfg_.transmit.inputDevice, this, [this](const std::vector<int16_t> &f) { onMicFrame(f); }, &err)) {
            txEnc_.reset();
            mic_.reset();
        }
    }
    if (!err.isEmpty()) {
        setStatus("Cannot transmit: " + err, true);
        emit txRefused(err);
        return;
    }
    txActive_ = true;
    txStartMs_ = nowMs();
    if (out_) out_->flush();
    for (const QByteArray &r : txEnc_->begin()) txSend(r);
    setStatus(QString("Transmitting %1 to %2").arg(t.mode, t.mode == "DMR" ? QString("TG %1").arg(t.dst) : endpoint(t.mode)));
}

void Radio::onMicFrame(const std::vector<int16_t> &frame)
{
    if (!txActive_ || !txEnc_) return;
    // Gain, then automatic level control (fast attack, slow release, at most +18 dB) and a hard limit.
    const double gain = std::pow(10.0, cfg_.transmit.micGainDb / 20.0);
    double peak = 1;
    for (int16_t v : frame) peak = std::max(peak, std::abs(v * gain));
    if (cfg_.transmit.autoLevel) {
        const double want = std::min(8.0, 16000.0 / peak);
        micGain_ = want < micGain_ ? want : micGain_ + (want - micGain_) * 0.02;
    } else {
        micGain_ = 1.0;
    }
    int16_t pcm[160];
    const size_t n = std::min<size_t>(160, frame.size());
    for (size_t i = 0; i < n; ++i) pcm[i] = int16_t(std::clamp(frame[i] * gain * micGain_, -32000.0, 32000.0));
    std::fill(pcm + n, pcm + 160, int16_t(0));
    for (const QByteArray &r : txEnc_->frame(pcm)) txSend(r);
    if (nowMs() - txStartMs_ > cfg_.transmit.timeoutSec * 1000LL)
        stopTx(QString("time limit (%1 s) reached").arg(cfg_.transmit.timeoutSec));
}

void Radio::stopTx(const QString &why)
{
    if (!txActive_) return;
    if (!why.isEmpty()) logEvent("INFO", "PTT off: " + why);
    txActive_ = false;
    if (mic_) mic_->stop();
    if (txEnc_) for (const QByteArray &r : txEnc_->end()) txSend(r);
    txEnc_.reset();
    txMuteUntilMs_ = nowMs() + 300;
    const double secs = (nowMs() - txStartMs_) / 1000.0;
    setStatus(QString("Transmitted %1 s%2").arg(secs, 0, 'f', 1).arg(why.isEmpty() ? QString() : " - " + why), !why.isEmpty());
}

// ---------------------------------------------------------------- decoding
void Radio::dispatchRecord(const QByteArray &rec)
{
    if (rec.startsWith("PAGE")) { if (modeAllowed("POCSAG")) handlePage(rec.mid(4)); }
    else if (rec.startsWith("DSTR")) { if (hear("D-Star")) handleDStar(rec.mid(4)); }
    else if (rec.startsWith("YSFX")) { if (hear("YSF")) handleYsf(rec.mid(4)); }
    else if (rec.startsWith("P25X")) { if (hear("P25")) handleP25(rec.mid(4)); }
    else if (rec.startsWith("NXDX")) { if (hear("NXDN")) handleNxdn(rec.mid(4)); }
    else if (rec.startsWith("M17X")) { if (hear("M17")) handleM17(rec.mid(4)); }
    else if (hear("DMR")) handlePacket(rec);
}

bool Radio::modeAllowed(const QString &mode) const
{
    return mode == "POCSAG" ? cfg_.network.dapnet : cfg_.network.modes.contains(mode);
}

bool Radio::hear(const QString &mode) const
{
    // Our own transmissions are always logged.
    return modeAllowed(mode) && (ownTx_ || cfg_.network.rxModes.contains(mode));
}

void Radio::endAllCalls(const QString &why)
{
    for (int s = 1; s <= 2; ++s) endCall(s, why);
    endAllDStar(why);
    for (auto *o : {&ysf_, &p25_, &nxdn_, &m17_}) endGen(*o, why);
}

// ---- DMR
void Radio::handlePacket(const QByteArray &raw)
{
    DmrPacket p;
    if (!parseDmrd(raw, p)) return;
    const qint64 now = nowMs();

    if (p.isTerminator()) {
        if (active_[p.slot] && active_[p.slot]->stream == p.stream) endCall(p.slot, "terminator");
        return;
    }
    if (p.isHeader()) {
        if (!active_[p.slot] || active_[p.slot]->stream != p.stream) beginCall(p, false);
        active_[p.slot]->lastPacketMs = now;
        return;
    }
    if (!p.isVoice()) return;

    // Drop the same burst seen twice, but not repeated identical frames such as silence.
    const QByteArray key = p.stream + char(p.seq) + voiceBits(p.payload);
    if (seen_.contains(key)) return;
    seen_.insert(key);
    seenOrder_.push_back(key);
    if (int(seenOrder_.size()) > kSeenMax) { seen_.remove(seenOrder_.front()); seenOrder_.pop_front(); }

    if (!active_[p.slot] || active_[p.slot]->stream != p.stream) beginCall(p, true);
    Call &c = *active_[p.slot];
    c.lastPacketMs = now;

    float samples[480];
    c.errors += decoder_.decodeBurst(p.payload, samples);
    c.frames += 3;
    playDecoded(QString("DMR%1").arg(p.slot), samples, 480);
}

void Radio::beginCall(const DmrPacket &p, bool lateEntry)
{
    if (active_[p.slot]) endCall(p.slot, "new call");
    Call c;
    c.stream = p.stream;
    c.src = p.src;
    c.dst = p.dst;
    c.slot = p.slot;
    c.privateCall = p.privateCall;
    c.lateEntry = lateEntry;
    c.tx = ownTx_;
    c.started = QDateTime::currentDateTime();
    c.timer.start();
    c.lastPacketMs = nowMs();
    active_[p.slot] = c;
    recBuf_[QString("DMR%1").arg(p.slot)].clear();
    decoder_.reset();
}

void Radio::endCall(int slot, const QString &why)
{
    Q_UNUSED(why)
    if (!active_[slot]) return;
    const Call c = *active_[slot];
    active_[slot].reset();
    releaseAudio(QString("DMR%1").arg(slot));
    decoder_.reset();

    const double secs = c.timer.elapsed() / 1000.0;
    const double ber = c.frames ? 100.0 * c.errors / (c.frames * 72.0) : 0.0;
    saveRecording(QString("DMR%1").arg(slot), c.started, "DMR", callsign(c.src));
    appendCsv(dmrLog_, QString("%1,%2,%3,%4,%5,%6,%7,%8")
                           .arg(c.started.toString(Qt::ISODate)).arg(c.slot).arg(c.src)
                           .arg(callsign(c.src), QString(ids_.value(c.src).second).replace(',', ' '), target(c),
                                QString::number(secs, 'f', 1), QString::number(ber, 'f', 1)));
}

// ---- D-Star
void Radio::handleDStar(const QByteArray &rec)
{
    DStarPacket p;
    if (!parseDstr(rec, p)) return;
    const qint64 now = nowMs();
    auto it = dstars_.find(p.id);

    if (p.isHeader) {
        if (it == dstars_.end()) {
            beginDStar(p, false).lastPacketMs = now;
        } else {
            DStarCall &c = it->second;
            c.lastPacketMs = now;
            // The header is repeated every superframe: fill in a late entry.
            if (c.my.isEmpty() && !p.my.isEmpty()) {
                c.my = p.my; c.suffix = p.mySuffix; c.your = p.your; c.rpt1 = p.rpt1; c.rpt2 = p.rpt2;
                c.reflector = c.your == "CQCQCQ" ? reflector_ : QString();
            }
        }
        return;
    }
    DStarCall &c = it != dstars_.end() ? it->second : beginDStar(p, true);   // missed the header
    c.lastPacketMs = now;
    c.text.feed(p.seq, p.slowData);

    float samples[160];
    c.errors += c.decoder.decodeFrame(p.ambe, samples);
    c.frames += 1;
    playDecoded(dstarKey(p.id), samples, 160);

    if (p.isEnd) endDStar(p.id, "end");
}

Radio::DStarCall &Radio::beginDStar(const DStarPacket &p, bool lateEntry)
{
    DStarCall c;
    c.id = p.id;
    c.fromRf = p.fromRf;
    c.lateEntry = lateEntry;
    c.tx = ownTx_;
    if (!lateEntry) { c.my = p.my; c.suffix = p.mySuffix; c.your = p.your; c.rpt1 = p.rpt1; c.rpt2 = p.rpt2; }
    if (lateEntry || c.your == "CQCQCQ") c.reflector = reflector_;
    c.started = QDateTime::currentDateTime();
    c.timer.start();
    c.lastPacketMs = nowMs();
    recBuf_[dstarKey(p.id)].clear();
    return dstars_[p.id] = c;
}

void Radio::endAllDStar(const QString &why)
{
    while (!dstars_.empty()) endDStar(dstars_.begin()->first, why);
}

void Radio::endDStar(uint16_t id, const QString &why)
{
    Q_UNUSED(why)
    auto it = dstars_.find(id);
    if (it == dstars_.end()) return;
    const DStarCall c = it->second;
    dstars_.erase(it);
    releaseAudio(dstarKey(id));

    const double secs = c.timer.elapsed() / 1000.0;
    const double ber = c.frames ? 100.0 * c.errors / (c.frames * 72.0) : 0.0;
    saveRecording(dstarKey(id), c.started, "DSTAR", dstarCaller(c));
    recBuf_.remove(dstarKey(id));
    appendCsv(dstarLog_, QStringList{c.started.toString(Qt::ISODate), c.fromRf ? "RF" : "Net", csvQuote(c.my),
                                     csvQuote(c.suffix), csvQuote(c.your), csvQuote(c.rpt1), csvQuote(c.rpt2),
                                     csvQuote(c.text.text()), QString::number(secs, 'f', 1),
                                     QString::number(ber, 'f', 1), csvQuote(c.reflector)}.join(','));
}

QString Radio::dstarName(const QString &call) const
{
    return nameByCall_.value(call.section(' ', 0, 0).toUpper());
}

QString Radio::dstarCaller(const DStarCall &c)
{
    if (c.my.isEmpty()) return "(unknown)";
    return c.suffix.isEmpty() ? c.my : c.my + "/" + c.suffix;
}

QString Radio::dstarTarget(const DStarCall &c)
{
    QString t = c.your.isEmpty() ? QString("CQCQCQ") : c.your;
    const QString via = !c.reflector.isEmpty() ? c.reflector : c.rpt2;
    if (!via.isEmpty()) t += QString(" via %1").arg(QString(via).simplified());
    return t;
}

// ---- YSF, P25, NXDN and M17
QString Radio::genName(const GenCall &c) const
{
    if (c.mode == "P25" && c.srcId) return ids_.value(c.srcId).second;
    if (c.mode == "NXDN" && c.srcId) {
        const QString n = nxdnIds_.value(c.srcId).second;
        return n.isEmpty() ? dstarName(c.callsign) : n;
    }
    return dstarName(c.callsign);   // callsign -> name from DMRIds.dat
}

void Radio::beginGen(std::optional<GenCall> &slot, const QString &mode, bool fromRf, bool lateEntry)
{
    endGen(slot, "new call");
    GenCall c;
    c.mode = mode;
    c.fromRf = fromRf;
    c.lateEntry = lateEntry;
    c.tx = ownTx_;
    c.started = QDateTime::currentDateTime();
    c.timer.start();
    c.lastPacketMs = nowMs();
    slot = c;
    recBuf_[mode].clear();
    if (mode == "YSF") ysfDecoder_.reset();
    else if (mode == "P25") p25Decoder_.reset();
    else if (mode == "NXDN") nxdnDecoder_.reset();
    else if (mode == "M17") m17Decoder_.reset();
}

void Radio::endGen(std::optional<GenCall> &slot, const QString &why)
{
    if (!slot) return;
    const GenCall c = *slot;
    slot.reset();
    releaseAudio(c.mode);
    saveRecording(c.mode, c.started, c.mode, c.callsign);
    if (c.frames == 0 && why != "end") return;   // header-only blips

    const double secs = c.timer.elapsed() / 1000.0;
    appendCsv(genLog_, QStringList{c.started.toString(Qt::ISODate), c.mode, c.fromRf ? "RF" : "Net",
                                   csvQuote(c.callsign), csvQuote(genName(c)), csvQuote(c.target),
                                   QString::number(secs, 'f', 1),
                                   c.errorBits ? QString::number(100.0 * c.errors / c.errorBits, 'f', 1) : QString(),
                                   csvQuote(c.note)}.join(','));
}

void Radio::handleYsf(const QByteArray &rec)
{
    YsfPacket p;
    if (!parseYsf(rec, p)) return;
    const bool header = p.fichOk && p.fi == 0;
    const bool terminator = p.end || (p.fichOk && p.fi == 2);
    if (header || !ysf_ || (ysf_->callsign != p.src && !p.src.isEmpty())) beginGen(ysf_, "YSF", p.fromRf, !header);
    GenCall &c = *ysf_;
    c.lastPacketMs = nowMs();
    if (!p.src.isEmpty()) c.callsign = p.src;
    if (!p.dst.isEmpty()) c.target = p.dst;
    if (p.fichOk && p.dgid) c.target = QString("%1 DG-ID %2").arg(c.target.isEmpty() ? "ALL" : c.target).arg(p.dgid);

    if (p.fichOk && p.fi == 1) {
        if (p.dt == 2) {
            float samples[800];
            c.errors += ysfDecoder_.decodeVD2(p.frame, samples);
            c.errorBits += 5 * 27;
            c.frames += 5;
            playDecoded("YSF", samples, 800);
        } else if (p.dt == 1) {
            c.audio = false;
            c.note = "data transmission (no voice)";
        } else if (p.dt == 3 || p.dt == 0) {
            c.frames += 5;
            c.audio = false;
            c.note = p.dt == 3 ? "Voice Wide (VW) mode: no audio decoder" : "V/D mode 1: no audio decoder";
        }
    }
    if (terminator) endGen(ysf_, "end");
}

void Radio::handleP25(const QByteArray &rec)
{
    if (rec.size() < 2) return;
    const bool fromRf = rec[0] == 'R';
    const QByteArray d = rec.mid(1);
    const uint8_t type = uint8_t(d[0]);
    if (type == 0x80) { endGen(p25_, "end"); return; }

    int off = -1;
    switch (type) {
    case 0x62: case 0x6B: off = 10; break;
    case 0x63: case 0x6C: off = 1; break;
    case 0x6A: case 0x73: off = 4; break;
    default:
        if ((type >= 0x64 && type <= 0x69) || (type >= 0x6D && type <= 0x72)) off = 5;
    }
    if (off < 0 || d.size() < off + 11) return;

    auto id24 = [&d] { return (uint32_t(uint8_t(d[1])) << 16) | (uint32_t(uint8_t(d[2])) << 8) | uint8_t(d[3]); };
    // A new source ID without an end record in between means a new call.
    if (type == 0x66 && p25_ && p25_->srcId && p25_->srcId != id24()) endGen(p25_, "new call");
    if (!p25_) beginGen(p25_, "P25", fromRf, type != 0x62);
    GenCall &c = *p25_;
    c.lastPacketMs = nowMs();
    if (type == 0x65) { c.dstId = id24(); c.target = QString("TG %1").arg(c.dstId); }
    if (type == 0x66) { c.srcId = id24(); c.callsign = callsign(c.srcId); }

    float samples[160];
    p25Decoder_.decodeImbe(d.constData() + off, samples);
    c.frames += 1;
    playDecoded("P25", samples, 160);
}

QString Radio::nxdnCall(uint32_t id) const
{
    auto it = nxdnIds_.constFind(id);
    return it != nxdnIds_.constEnd() && !it->first.isEmpty() ? it->first : QString::number(id);
}

void Radio::handleNxdn(const QByteArray &rec)
{
    NxdnPacket p;
    if (!parseNxdn(rec, p)) return;
    const auto *d = reinterpret_cast<const uint8_t *>(p.data.constData());
    // Layer 3 VCALL: type(1) ...(1) flags(1) src(2) dst(2)
    auto applyVcall = [this](GenCall &c, const uint8_t *l3) {
        c.srcId = (l3[3] << 8) | l3[4];
        c.dstId = (l3[5] << 8) | l3[6];
        c.callsign = nxdnCall(c.srcId);
        c.target = (l3[2] & 0x80) ? QString("Private %1").arg(nxdnCall(c.dstId)) : QString("TG %1").arg(c.dstId);
    };

    if (p.usc == 0) {                       // SACCH_NS: header / trailer carried in FACCH1
        const uint8_t type = d[5] & 0x3F;
        if (type == 0x01) {                 // VCALL
            const uint32_t src = (d[8] << 8) | d[9];
            if (!nxdn_ || (nxdn_->srcId && nxdn_->srcId != src)) beginGen(nxdn_, "NXDN", p.fromRf, false);
            nxdn_->lastPacketMs = nowMs();
            applyVcall(*nxdn_, d + 5);
        } else if (type == 0x08) {          // TX_REL
            endGen(nxdn_, "end");
        }
        return;
    }
    if (!nxdn_) beginGen(nxdn_, "NXDN", p.fromRf, true);
    GenCall &c = *nxdn_;
    c.lastPacketMs = nowMs();

    if (p.usc == 1) {                       // UDCH: data call
        c.audio = false;
        c.note = "data transmission (no voice)";
        c.frames += 1;
        return;
    }

    if (!c.srcId) {
        // Late entry: the VCALL is repeated across the 4 SACCHs of each superframe (18 bits each).
        static const int partOffset[4] = {54, 36, 18, 0};   // structure 0 = 4/4 ... 3 = 1/4
        const int structure = d[1] >> 6;
        for (int i = 0; i < 18; ++i) {
            const int dst = partOffset[structure] + i;
            const bool b = (d[2 + (i >> 3)] >> (7 - (i & 7))) & 1;   // SACCH layer 3 bits start at byte 2
            if (b) c.l3[dst >> 3] |= uint8_t(0x80 >> (dst & 7));
            else   c.l3[dst >> 3] &= uint8_t(~(0x80 >> (dst & 7)));
        }
        if (structure == 3) c.l3Mask = 1; else if (c.l3Mask) c.l3Mask |= 1 << (3 - structure);
        if (c.l3Mask == 0x0F && (c.l3[0] & 0x3F) == 0x01) applyVcall(c, c.l3);
    }

    float samples[640];
    int n = 0;
    if (p.option == 3 || p.option == 2) { nxdnDecoder_.decodeHalf(d + 5, samples + n); n += 320; }
    if (p.option == 3 || p.option == 1) { nxdnDecoder_.decodeHalf(d + 19, samples + n); n += 320; }
    if (n) {
        c.frames += n / 160;
        playDecoded("NXDN", samples, n);
    }
}

void Radio::handleM17(const QByteArray &rec)
{
    M17Packet p;
    if (!parseM17(rec, p)) return;
    if (!m17_ || m17_->streamId != p.sid) {
        beginGen(m17_, "M17", p.fromRf, false);
        m17_->streamId = p.sid;
    }
    GenCall &c = *m17_;
    c.lastPacketMs = nowMs();
    c.callsign = p.src;
    c.target = p.dst;
    c.frames += 2;
    if (p.encType != 0) {
        c.audio = false;
        c.note = "encrypted";
    } else if (p.dataType == 1) {
        c.audio = false;
        c.note = "data transmission (no voice)";
    } else if (!M17Decoder::available()) {
        c.audio = false;
        c.note = "no Codec2 decoder";
    } else {
        float samples[320];
        if (const int n = m17Decoder_.decode(p, samples)) playDecoded("M17", samples, n);
    }
    if (p.end) endGen(m17_, "end");
}

// ---- pager
void Radio::handlePage(const QByteArray &logLine)
{
    // DAPNET line, e.g.
    // M: 2026-11-13 20:00:38.443 Sending message in slot 0 to 1234567, type 6, func Alphanumeric: "text"
    static const QRegularExpression re(
        R"re(^\w: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+ Sending message in slot \d+ to (\d+), type \d+, func (\w+): "(.*)"\s*$)re");
    if (seenPages_.contains(logLine)) return;   // history is re-sent on every reconnect
    seenPages_.insert(logLine);

    const auto m = re.match(QString::fromUtf8(logLine));
    if (!m.hasMatch()) return;
    QDateTime when = QDateTime::fromString(m.captured(1), "yyyy-MM-dd HH:mm:ss");
    when.setTimeZone(QTimeZone::utc());   // DAPNET times are UTC
    when = when.toLocalTime();
    const uint32_t ric = m.captured(2).toUInt();
    const QString func = m.captured(3), text = m.captured(4);
    appendCsv(pageLog_, QString("%1,%2,%3,%4").arg(when.toString(Qt::ISODate)).arg(ric).arg(func, csvQuote(text)));
    if (myRics_.contains(ric)) emit page(ric, text);
}

// ---------------------------------------------------------------- audio
static QString audioChannel(const QString &key)   // D-Star streams share one channel
{
    return key.startsWith("DSTAR") ? QString("DSTAR") : key;
}

bool Radio::claimAudio(const QString &key)
{
    if (audioOwner_.isEmpty()) {
        // After a call, hold the speaker on its channel for a few seconds so a reply isn't cut
        // off by traffic elsewhere (which is still logged and recorded).
        if (nowMs() < holdUntilMs_ && audioChannel(key) != holdChannel_) return false;
        audioOwner_ = key;
        gate_.reset();   // learn the new station's background afresh
    }
    return audioOwner_ == key;
}

void Radio::releaseAudio(const QString &key)
{
    if (audioOwner_ != key) return;
    audioOwner_.clear();
    holdChannel_ = audioChannel(key);
    holdUntilMs_ = nowMs() + kHoldMs;
}

void Radio::playDecoded(const QString &key, const float *samples, int n)
{
    // One call is played at a time; overlapping calls are still logged and recorded.
    if (cfg_.recording.keepCalls > 0) {
        std::vector<float> &rec = recBuf_[key];
        if (rec.size() < size_t(cfg_.recording.maxMinutes) * 60 * 8000) rec.insert(rec.end(), samples, samples + n);
    }
    if (!claimAudio(key)) return;
    if (txActive_ || nowMs() < txMuteUntilMs_) return;   // our own transmission
    const double vol = cfg_.audio.mute ? 0.0 : cfg_.audio.volume / 100.0;
    std::vector<int16_t> pcm;
    double rms = 0;
    agc_.process(samples, n, vol, pcm, rms);
    if (cfg_.audio.noiseGate) {
        // After the level control: gating before it would let the AGC turn the pauses back up.
        std::vector<float> f(pcm.begin(), pcm.end());
        gate_.process(f.data(), n, {cfg_.audio.gateThresholdDb, cfg_.audio.gateReductionDb, cfg_.audio.gateHoldMs});
        for (int i = 0; i < n; ++i) pcm[size_t(i)] = int16_t(f[size_t(i)]);
    }
    if (!out_ || !out_->isOpen()) {
        const qint64 now = nowMs();
        if (now < audioRetryMs_) return;
        if (!out_) out_ = std::make_unique<AudioOut>("Live");
        QString err;
        if (!out_->open(cfg_.audio.backend, cfg_.audio.device, &err)) {
            audioRetryMs_ = now + 5000;   // don't hammer a missing device on every packet
            setStatus("No audio output: " + err, true);
            return;
        }
    }
    out_->write(pcm.data(), pcm.size());
}

// Recordings of the last few calls, as 8 kHz WAV (Settings → recording.keepCalls; 0 = none).
QString Radio::saveRecording(const QString &key, const QDateTime &started, const QString &mode, const QString &who)
{
    std::vector<float> buf;
    buf.swap(recBuf_[key]);
    if (buf.size() < size_t(cfg_.recording.minSeconds * 8000) || recDir_.isEmpty() || cfg_.recording.keepCalls <= 0) return {};

    // Same AGC as live listening, at 100% volume.
    Agc agc;
    std::vector<int16_t> pcm, chunk;
    pcm.reserve(buf.size());
    double rms = 0;
    for (size_t i = 0; i < buf.size(); i += 160) {
        agc.process(buf.data() + i, int(std::min<size_t>(160, buf.size() - i)), 1.0, chunk, rms);
        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
    }

    QDir().mkpath(recDir_);
    QString safe = who.isEmpty() ? QString("unknown") : who;
    safe.replace(QRegularExpression("[^A-Za-z0-9-]+"), "_");
    const QString path = QString("%1/%2_%3_%4.wav").arg(recDir_, started.toString("yyyyMMdd-HHmmss-zzz"), mode, safe);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    auto le32 = [](quint32 v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };   // ARM is little-endian
    auto le16 = [](quint16 v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };
    const quint32 dataBytes = quint32(pcm.size() * 2);
    QByteArray hdr;
    hdr += "RIFF" + le32(36 + dataBytes) + "WAVE";
    hdr += "fmt " + le32(16) + le16(1) + le16(1) + le32(8000) + le32(16000) + le16(2) + le16(16);
    hdr += "data" + le32(dataBytes);
    f.write(hdr);
    f.write(reinterpret_cast<const char *>(pcm.data()), dataBytes);
    f.close();
    pruneRecordings();
    return path;
}

void Radio::pruneRecordings()
{
    // File names start with the call's start time, so name order is age order.
    QDir d(recDir_);
    const QStringList files = d.entryList({"*.wav"}, QDir::Files, QDir::Name);
    for (int i = 0; i + cfg_.recording.keepCalls < files.size(); ++i) d.remove(files[i]);
}

// ---------------------------------------------------------------- names
QString Radio::callsign(uint32_t id) const
{
    if (id == cfg_.testCall.dmrEchoId) return "Parrot";
    auto it = ids_.constFind(id);
    return it != ids_.constEnd() && !it->first.isEmpty() ? it->first : QString::number(id);
}

QString Radio::target(const Call &c) const
{
    if (!c.privateCall) return QString("TG %1").arg(c.dst);
    if (c.dst == cfg_.testCall.dmrEchoId) return QString("Private %1 (Parrot)").arg(c.dst);
    return QString("Private %1").arg(callsign(c.dst));
}

// ---------------------------------------------------------------- every 100 ms
void Radio::tick()
{
    const qint64 now = nowMs();

    // End calls whose packets stopped without a terminator.
    const int timeout = cfg_.calls.timeoutMs;
    for (int s = 1; s <= 2; ++s)
        if (active_[s] && now - active_[s]->lastPacketMs > timeout) endCall(s, "timeout");
    for (auto it = dstars_.begin(); it != dstars_.end();) {
        const uint16_t id = it->first;
        const bool stale = now - it->second.lastPacketMs > timeout;
        ++it;
        if (stale) endDStar(id, "timeout");
    }
    for (auto *o : {&ysf_, &p25_, &nxdn_, &m17_})
        if (*o && now - (*o)->lastPacketMs > timeout) endGen(*o, "timeout");

    // The call on show: the one that has the speaker; otherwise the newest.
    const Call *c = nullptr;
    const GenCall *g = nullptr;
    const DStarCall *ds = nullptr;
    if (audioOwner_ == "DMR1" || audioOwner_ == "DMR2") {
        const int s = audioOwner_.right(1).toInt();
        if (active_[s]) c = &*active_[s];
    } else if (audioOwner_.startsWith("DSTAR")) {
        auto it = dstars_.find(uint16_t(audioOwner_.mid(5).toUInt()));
        if (it != dstars_.end()) ds = &it->second;
    } else if (!audioOwner_.isEmpty()) {
        for (const auto *o : {&ysf_, &p25_, &nxdn_, &m17_})
            if (*o && (*o)->mode == audioOwner_) g = &**o;
    } else if (now >= holdUntilMs_) {   // (while holding for a reply, show nothing else)
        QDateTime newest;
        for (int s = 1; s <= 2; ++s)
            if (active_[s] && (!c || active_[s]->started > newest)) { c = &*active_[s]; newest = c->started; }
        for (const auto &[id, d] : dstars_)
            if (newest.isNull() || d.started > newest) { c = nullptr; ds = &d; newest = d.started; }
        for (const auto *o : {&ysf_, &p25_, &nxdn_, &m17_})
            if (*o && (newest.isNull() || (*o)->started > newest)) { c = nullptr; ds = nullptr; g = &**o; newest = g->started; }
    }

    if (g) onAir_ = {g->mode, g->callsign.isEmpty() ? "?" : g->callsign, genName(*g), g->target, g->timer.elapsed(), g->tx};
    else if (ds) onAir_ = {"D-Star", ds->my, dstarName(ds->my), dstarTarget(*ds), ds->timer.elapsed(), ds->tx};
    else if (c) onAir_ = {"DMR", callsign(c->src), ids_.value(c->src).second, target(*c), c->timer.elapsed(), c->tx};
    else onAir_ = {};

    followRx(c);
    emit ticked();
}

// TX follows RX: PTT replies in the mode (and for DMR, on the talkgroup) just heard.
void Radio::followRx(const Call *c)
{
    const QString heard = onAir_.tx ? QString() : onAir_.mode;
    if (heard == onAirMode_) return;
    onAirMode_ = heard;
    if (heard.isEmpty() || txActive_ || !cfg_.transmit.followRx) return;
    if (txModes_.contains(heard) && cfg_.transmit.mode != heard) {
        cfg_.transmit.mode = heard;
        saveSettings();
    }
    if (c && heard == "DMR") {
        const bool priv = c->privateCall;
        const uint32_t tg = priv ? c->src : c->dst;   // a private call to us: reply to the caller
        if (tg && (tg != cfg_.transmit.dmrTalkgroup || priv != cfg_.transmit.dmrPrivate)) {
            cfg_.transmit.dmrTalkgroup = tg;
            cfg_.transmit.dmrPrivate = priv;
            saveSettings();
        }
    }
}
