#include "Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

QString Settings::path()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.json";
}

QString Settings::logDir() const
{
    return logging.directory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                       : logging.directory;
}

QString Settings::recordingDir() const
{
    return recording.directory.isEmpty() ? logDir() + "/recordings" : recording.directory;
}

bool Settings::stationReady(QString *why) const
{
    QString w;
    if (station.callsign.isEmpty() || station.callsign.toUpper() == "N0CALL") w = "set your callsign";
    else if (station.dmrId == 0) w = "set your DMR ID (also used for P25)";
    if (why) *why = w.isEmpty() ? QString() : w + " in Settings → Station first";
    return w.isEmpty();
}

QString Settings::testSpokenText() const
{
    if (!testCall.spokenText.isEmpty()) return testCall.spokenText;
    QStringList letters;   // spell the callsign out so it is spoken as letters and digits
    for (QChar c : station.callsign) letters << QString(c);
    return letters.join(' ') + ". Test, test, test.";
}

QString Settings::testTextMessage() const
{
    if (!testCall.textMessage.isEmpty()) return testCall.textMessage;
    const QString full = station.callsign + " Test test test";   // D-Star slow data holds 20 characters
    return full.size() <= 20 ? full : (station.callsign + " Test test").left(20);
}

// ---------------------------------------------------------------- JSON helpers
namespace {
void get(const QJsonObject &o, const char *k, QString &v) { if (o.contains(k)) v = o.value(k).toString(v); }
void get(const QJsonObject &o, const char *k, bool &v) { if (o.contains(k)) v = o.value(k).toBool(v); }
void get(const QJsonObject &o, const char *k, int &v) { if (o.contains(k)) v = o.value(k).toInt(v); }
void get(const QJsonObject &o, const char *k, double &v) { if (o.contains(k)) v = o.value(k).toDouble(v); }
void get(const QJsonObject &o, const char *k, uint32_t &v)
{
    if (o.contains(k)) v = uint32_t(o.value(k).toDouble(double(v)));
}
void get(const QJsonObject &o, const char *k, QStringList &v)
{
    if (!o.value(k).isArray()) return;
    v.clear();
    for (const QJsonValue &x : o.value(k).toArray()) v << x.toString();
}
}   // namespace

static void readInto(Settings &s, const QJsonObject &root);

Settings Settings::load()
{
    Settings s;
    QFile f(path());
    if (!f.exists()) {
        // First run: take the station, audio, pager and transmit settings from the Pi-Star monitor
        // (D-DstarMonitor) if it is installed, with what has to differ for a PC on the networks.
        QFile mon(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
                  "/pistar-dmr/pistar-dmr/settings.json");
        if (mon.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(mon.readAll()).object();
            readInto(s, root);
            s.logging.directory.clear();     // keep separate call logs and recordings
            s.recording.directory.clear();
            const QString piHost = root.value("connection").toObject().value("host").toString();
            if (!piHost.isEmpty()) s.network.piStarHost = piHost;
            // The PC needs its own BrandMeister hotspot ID (the Pi-Star keeps its own).
            if (s.station.dmrId) s.station.repeaterId = s.station.dmrId * 100 + 2;
            s.testCall.ysfParrot = "00052";  // public parrot (the Pi-Star's 00001 was local)
        }
        s.save();
        return s;
    }
    if (!f.open(QIODevice::ReadOnly)) return s;
    readInto(s, QJsonDocument::fromJson(f.readAll()).object());
    return s;
}

static void readInto(Settings &s, const QJsonObject &root)
{
    const QJsonObject st = root.value("station").toObject();
    get(st, "callsign", s.station.callsign);
    get(st, "dmrId", s.station.dmrId);
    get(st, "repeaterId", s.station.repeaterId);
    get(st, "nxdnId", s.station.nxdnId);
    get(st, "dmrSlot", s.station.dmrSlot);
    get(st, "colourCode", s.station.colourCode);
    get(st, "dstarModule", s.station.dstarModule);

    const QJsonObject p = root.value("pager").toObject();
    get(p, "myRics", s.pager.myRics);
    get(p, "onlyMine", s.pager.onlyMine);

    const QJsonObject a = root.value("audio").toObject();
    get(a, "volume", s.audio.volume);
    get(a, "mute", s.audio.mute);
    get(a, "backend", s.audio.backend);
    get(a, "device", s.audio.device);
    get(a, "noiseGate", s.audio.noiseGate);
    get(a, "gateThresholdDb", s.audio.gateThresholdDb);
    get(a, "gateReductionDb", s.audio.gateReductionDb);
    get(a, "gateHoldMs", s.audio.gateHoldMs);

    get(root.value("calls").toObject(), "timeoutMs", s.calls.timeoutMs);

    const QJsonObject r = root.value("recording").toObject();
    get(r, "keepCalls", s.recording.keepCalls);
    get(r, "minSeconds", s.recording.minSeconds);
    get(r, "maxMinutes", s.recording.maxMinutes);
    get(r, "directory", s.recording.directory);

    get(root.value("logging").toObject(), "directory", s.logging.directory);

    const QJsonObject t = root.value("testCall").toObject();
    get(t, "mode", s.testCall.mode);
    get(t, "echo", s.testCall.echo);
    get(t, "spokenText", s.testCall.spokenText);
    get(t, "textMessage", s.testCall.textMessage);
    get(t, "voice", s.testCall.voice);
    get(t, "speechRate", s.testCall.speechRate);
    get(t, "localTalkgroup", s.testCall.localTalkgroup);
    get(t, "dmrEchoId", s.testCall.dmrEchoId);
    get(t, "p25EchoTg", s.testCall.p25EchoTg);
    get(t, "nxdnEchoTg", s.testCall.nxdnEchoTg);
    get(t, "ysfParrot", s.testCall.ysfParrot);
    get(t, "m17Echo", s.testCall.m17Echo);
    get(t, "dstarEcho", s.testCall.dstarEcho);

    const QJsonObject x = root.value("transmit").toObject();
    get(x, "mode", s.transmit.mode);
    get(x, "inputDevice", s.transmit.inputDevice);
    get(x, "micGainDb", s.transmit.micGainDb);
    get(x, "autoLevel", s.transmit.autoLevel);
    get(x, "timeoutSec", s.transmit.timeoutSec);
    get(x, "latch", s.transmit.latch);
    get(x, "dmrTalkgroup", s.transmit.dmrTalkgroup);
    get(x, "dmrPrivate", s.transmit.dmrPrivate);
    get(x, "dmrTalkgroups", s.transmit.dmrTalkgroups);
    get(x, "followRx", s.transmit.followRx);
    get(x, "dstarSuffix", s.transmit.dstarSuffix);

    const QJsonObject n = root.value("network").toObject();
    get(n, "modes", s.network.modes);
    get(n, "rxModes", s.network.rxModes);
    get(n, "dstarReflector", s.network.dstarReflector);
    get(n, "ysfReflector", s.network.ysfReflector);
    get(n, "m17Reflector", s.network.m17Reflector);
    get(n, "p25Tg", s.network.p25Tg);
    get(n, "nxdnTg", s.network.nxdnTg);
    get(n, "dstarModule", s.network.dstarModule);
    get(n, "dmrMaster", s.network.dmrMaster);
    get(n, "dmrPort", s.network.dmrPort);
    get(n, "dmrPassword", s.network.dmrPassword);
    get(n, "dmrRxFreqHz", s.network.dmrRxFreqHz);
    get(n, "dmrTxFreqHz", s.network.dmrTxFreqHz);
    get(n, "dmrLocation", s.network.dmrLocation);
    get(n, "dapnet", s.network.dapnet);
    get(n, "dapnetServer", s.network.dapnetServer);
    get(n, "dapnetPort", s.network.dapnetPort);
    get(n, "dapnetCallsign", s.network.dapnetCallsign);
    get(n, "dapnetAuthKey", s.network.dapnetAuthKey);
    get(n, "piStarCheck", s.network.piStarCheck);
    get(n, "piStarHost", s.network.piStarHost);

    get(root.value("window").toObject(), "alwaysOnTop", s.window.alwaysOnTop);
}

bool Settings::save(QString *err) const
{
    QJsonObject root;
    root["station"] = QJsonObject{{"callsign", station.callsign},
                                  {"dmrId", double(station.dmrId)},
                                  {"repeaterId", double(station.repeaterId)},
                                  {"nxdnId", double(station.nxdnId)},
                                  {"dmrSlot", station.dmrSlot},
                                  {"colourCode", station.colourCode},
                                  {"dstarModule", station.dstarModule}};
    root["pager"] = QJsonObject{{"myRics", pager.myRics}, {"onlyMine", pager.onlyMine}};
    root["audio"] = QJsonObject{{"volume", audio.volume},
                                {"mute", audio.mute},
                                {"backend", audio.backend},
                                {"device", audio.device},
                                {"noiseGate", audio.noiseGate},
                                {"gateThresholdDb", audio.gateThresholdDb},
                                {"gateReductionDb", audio.gateReductionDb},
                                {"gateHoldMs", audio.gateHoldMs}};
    root["calls"] = QJsonObject{{"timeoutMs", calls.timeoutMs}};
    root["recording"] = QJsonObject{{"keepCalls", recording.keepCalls},
                                    {"minSeconds", recording.minSeconds},
                                    {"maxMinutes", recording.maxMinutes},
                                    {"directory", recording.directory}};
    root["logging"] = QJsonObject{{"directory", logging.directory}};
    root["testCall"] = QJsonObject{{"mode", testCall.mode},
                                   {"echo", testCall.echo},
                                   {"spokenText", testCall.spokenText},
                                   {"textMessage", testCall.textMessage},
                                   {"voice", testCall.voice},
                                   {"speechRate", testCall.speechRate},
                                   {"localTalkgroup", double(testCall.localTalkgroup)},
                                   {"dmrEchoId", double(testCall.dmrEchoId)},
                                   {"p25EchoTg", double(testCall.p25EchoTg)},
                                   {"nxdnEchoTg", double(testCall.nxdnEchoTg)},
                                   {"ysfParrot", testCall.ysfParrot},
                                   {"m17Echo", testCall.m17Echo},
                                   {"dstarEcho", testCall.dstarEcho}};
    root["transmit"] = QJsonObject{{"mode", transmit.mode},
                                   {"inputDevice", transmit.inputDevice},
                                   {"micGainDb", transmit.micGainDb},
                                   {"autoLevel", transmit.autoLevel},
                                   {"timeoutSec", transmit.timeoutSec},
                                   {"latch", transmit.latch},
                                   {"dmrTalkgroup", double(transmit.dmrTalkgroup)},
                                   {"dmrPrivate", transmit.dmrPrivate},
                                   {"dmrTalkgroups", QJsonArray::fromStringList(transmit.dmrTalkgroups)},
                                   {"followRx", transmit.followRx},
                                   {"dstarSuffix", transmit.dstarSuffix}};
    root["network"] = QJsonObject{{"modes", QJsonArray::fromStringList(network.modes)},
                                  {"rxModes", QJsonArray::fromStringList(network.rxModes)},
                                  {"dstarReflector", network.dstarReflector},
                                  {"ysfReflector", network.ysfReflector},
                                  {"m17Reflector", network.m17Reflector},
                                  {"p25Tg", double(network.p25Tg)},
                                  {"nxdnTg", double(network.nxdnTg)},
                                  {"dstarModule", network.dstarModule},
                                  {"dmrMaster", network.dmrMaster},
                                  {"dmrPort", network.dmrPort},
                                  {"dmrPassword", network.dmrPassword},
                                  {"dmrRxFreqHz", double(network.dmrRxFreqHz)},
                                  {"dmrTxFreqHz", double(network.dmrTxFreqHz)},
                                  {"dmrLocation", network.dmrLocation},
                                  {"dapnet", network.dapnet},
                                  {"dapnetServer", network.dapnetServer},
                                  {"dapnetPort", network.dapnetPort},
                                  {"dapnetCallsign", network.dapnetCallsign},
                                  {"dapnetAuthKey", network.dapnetAuthKey},
                                  {"piStarCheck", network.piStarCheck},
                                  {"piStarHost", network.piStarHost}};
    root["window"] = QJsonObject{{"alwaysOnTop", window.alwaysOnTop}};

    QDir().mkpath(QFileInfo(path()).absolutePath());
    QSaveFile f(path());   // atomic: never leaves a half-written file
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}
