#include "Networks.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <QtEndian>
#include <cstring>

// ---------------------------------------------------------------- host files
namespace {
const char *kBaseUrl = "http://www.pistar.uk/downloads/";
const QStringList kFiles = {"DPlus_Hosts.txt", "DExtra_Hosts.txt", "DCS_Hosts.txt", "XLXHosts.txt", "YSF_Hosts.txt", "FCS_Hosts.txt",
                            "P25_Hosts.txt", "NXDN_Hosts.txt", "M17_Hosts.txt", "DMRIds.dat.gz", "NXDN.csv"};
}

HostFiles::HostFiles(const QString &dir, QObject *parent) : QObject(parent), dir_(dir)
{
    QDir().mkpath(dir_);
}

void HostFiles::refresh(bool force)
{
    for (const QString &f : kFiles) {
        const QString local = dir_ + "/" + (f.endsWith(".gz") ? f.chopped(3) : f);
        const QFileInfo fi(local);
        if (force || !fi.exists() || fi.lastModified().secsTo(QDateTime::currentDateTime()) > 24 * 3600) fetch(f);
    }
}

void HostFiles::fetch(const QString &file)
{
    QNetworkRequest req(QUrl(QString(kBaseUrl) + file));
    req.setHeader(QNetworkRequest::UserAgentHeader, "Digital-Transceiver");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *r = net_.get(req);
    connect(r, &QNetworkReply::finished, this, [this, r, file] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            // Keep the cached copy, and try again in a minute (e.g. the network wasn't up yet).
            if (!retrying_.contains(file)) {
                retrying_.insert(file);
                QTimer::singleShot(60000, this, [this, file] { retrying_.remove(file); fetch(file); });
            }
            return;
        }
        QByteArray body = r->readAll();
        QString name = file;
        if (file.endsWith(".gz")) {   // gunzip with the system gzip
            QProcess gz;
            gz.start("gzip", {"-dc"});
            gz.write(body);
            gz.closeWriteChannel();
            if (!gz.waitForFinished(60000) || gz.exitCode() != 0) return;
            body = gz.readAllStandardOutput();
            name.chop(3);
        }
        QSaveFile out(dir_ + "/" + name);
        if (!out.open(QIODevice::WriteOnly)) return;
        out.write(body);
        if (!out.commit()) return;
        cache_.clear();
        emit updated(name);
    });
}

QList<HostEntry> HostFiles::list(const QString &mode) const
{
    if (cache_.contains(mode)) return cache_.value(mode);
    QList<HostEntry> out;
    auto lines = [this](const QString &f) {
        QStringList l;
        QFile file(dir_ + "/" + f);
        if (file.open(QIODevice::ReadOnly))
            for (const QByteArray &b : file.readAll().split('\n')) {
                const QString s = QString::fromUtf8(b).trimmed();
                if (!s.isEmpty() && !s.startsWith('#')) l << s;
            }
        return l;
    };
    if (mode == "YSF") {
        for (const QString &l : lines("YSF_Hosts.txt")) {   // id;name;description;host;port;count;
            const QStringList p = l.split(';');
            if (p.size() >= 5) out << HostEntry{p[0], (p[1] + "  " + p[2]).trimmed(), p[3], quint16(p[4].toUInt())};
        }
    } else if (mode == "P25" || mode == "NXDN") {
        for (const QString &l : lines(mode == "P25" ? "P25_Hosts.txt" : "NXDN_Hosts.txt")) {   // tg host port
            const QStringList p = l.split(QRegularExpression("\\s+"));
            if (p.size() >= 3) out << HostEntry{p[0], p[1], p[1], quint16(p[2].toUInt())};
        }
    } else if (mode == "M17") {
        for (const QString &l : lines("M17_Hosts.txt")) {   // M17-xxx host port
            const QStringList p = l.split(QRegularExpression("\\s+"));
            if (p.size() >= 3) out << HostEntry{p[0], p[1], p[1], quint16(p[2].toUInt())};
        }
    } else if (mode == "D-Star") {
        for (const QString &l : lines("DCS_Hosts.txt")) {
            const QStringList p = l.split(QRegularExpression("\\s+"));
            if (p.size() >= 2) out << HostEntry{p[0], p[1], p[1], 30051};
        }
        for (const QString &l : lines("DPlus_Hosts.txt")) {   // REF reflectors (DPlus, needs registration)
            const QStringList p = l.split(QRegularExpression("\\s+"));
            if (p.size() >= 2 && p[0].startsWith("REF")) out << HostEntry{p[0], p[1], p[1], 20001};
        }
        for (const QString &l : lines("DExtra_Hosts.txt")) {
            const QStringList p = l.split(QRegularExpression("\\s+"));
            if (p.size() >= 2) out << HostEntry{p[0], p[1], p[1], 30001};
        }
        for (const QString &l : lines("XLXHosts.txt")) {   // nnn;host;port   (linked with DCS)
            const QStringList p = l.split(';');
            if (p.size() >= 2) out << HostEntry{"XLX" + p[0], p[1], p[1], 30051};
        }
    }
    cache_.insert(mode, out);
    return out;
}

bool HostFiles::find(const QString &mode, const QString &id, HostEntry &out) const
{
    for (const HostEntry &e : list(mode))
        if (e.id.compare(id, Qt::CaseInsensitive) == 0 || e.name.section("  ", 0, 0).compare(id, Qt::CaseInsensitive) == 0) {
            out = e;
            return true;
        }
    return false;
}

// ---------------------------------------------------------------- client base
NetClient::NetClient(const QString &mode, Settings &cfg, HostFiles &hosts, QObject *parent)
    : QObject(parent), mode_(mode), cfg_(cfg), hosts_(hosts)
{
    connect(&sock_, &QUdpSocket::readyRead, this, [this] {
        while (sock_.hasPendingDatagrams()) {
            QHostAddress from;
            quint16 port = 0;
            QByteArray d(int(sock_.pendingDatagramSize()), 0);
            sock_.readDatagram(d.data(), d.size(), &from, &port);
            if (from.toIPv4Address() != addr_.toIPv4Address() && from != addr_) continue;   // only our peer
            watchdog_.start();
            received(d);
        }
    });
    watchdog_.setSingleShot(true);
    watchdog_.setInterval(60000);
    connect(&watchdog_, &QTimer::timeout, this, [this] {
        if (state_ == State::Off) return;
        setState(State::Failed, "no reply from " + target_ + ", retrying");
        const QString t = target_;
        QTimer::singleShot(5000, this, [this, t] { if (state_ == State::Failed && target_ == t) link(t); });
    });
}

void NetClient::setState(State s, const QString &detail)
{
    if (s == state_ && detail == detail_) return;
    state_ = s;
    detail_ = detail;
    emit changed();
}

void NetClient::open(const QString &host, quint16 port, std::function<void()> then)
{
    if (lookup_ >= 0) QHostInfo::abortHostLookup(lookup_);
    setState(State::Connecting, "looking up " + host);
    lookup_ = QHostInfo::lookupHost(host, this, [this, host, port, then](const QHostInfo &hi) {
        lookup_ = -1;
        QHostAddress a;
        for (const QHostAddress &x : hi.addresses())
            if (x.protocol() == QAbstractSocket::IPv4Protocol) { a = x; break; }
        if (a.isNull()) {
            setState(State::Failed, "cannot find " + host);
            emit message(mode_ + ": cannot find " + host, true);
            return;
        }
        addr_ = a;
        port_ = port;
        if (sock_.state() != QAbstractSocket::BoundState) sock_.bind(QHostAddress::AnyIPv4, 0);
        setState(State::Connecting, "connecting to " + target_);
        watchdog_.start();
        then();
    });
}

void NetClient::send(const QByteArray &data)
{
    if (!addr_.isNull()) sock_.writeDatagram(data, addr_, port_);
}

void NetClient::stopTimers()
{
    poll_.stop();
    watchdog_.stop();
    poll_.disconnect();
}

QString NetClient::callsign10() const { return cfg_.station.callsign.leftJustified(10, ' ', true); }

namespace {
QByteArray padded(const QString &s, int n) { return s.toLatin1().leftJustified(n, ' ', true); }

// ---------------------------------------------------------------- YSF
class YsfClient : public NetClient {
public:
    using NetClient::NetClient;
    void link(const QString &target) override
    {
        HostEntry h;
        if (!hosts_.find("YSF", target, h)) { setState(State::Failed, "unknown reflector " + target); return; }
        unlink();
        target_ = h.name.section("  ", 0, 0);
        open(h.host, h.port, [this] {
            stopTimers();
            connect(&poll_, &QTimer::timeout, this, [this] { send("YSFP" + padded(cfg_.station.callsign, 10)); });
            poll_.start(5000);
            send("YSFP" + padded(cfg_.station.callsign, 10));
        });
    }
    void unlink() override
    {
        if (state_ != State::Off && !addr_.isNull())
            for (int i = 0; i < 3; ++i) send("YSFU" + padded(cfg_.station.callsign, 10));
        stopTimers();
        setState(State::Off);
        target_.clear();
    }
    void transmit(const QByteArray &rec) override
    {
        if (state_ == State::Linked && rec.startsWith("YSFXN")) send(rec.mid(5));
    }
protected:
    void received(const QByteArray &d) override
    {
        if (d.startsWith("YSFP")) setState(State::Linked);
        else if (d.startsWith("YSFD") && d.size() >= 155) emit record("YSFXN" + d.left(155));
    }
};

// ---------------------------------------------------------------- P25
class P25Client : public NetClient {
public:
    using NetClient::NetClient;
    void link(const QString &target) override
    {
        HostEntry h;
        if (!hosts_.find("P25", target, h)) { setState(State::Failed, "unknown talkgroup " + target); return; }
        unlink();
        target_ = "TG " + h.id;
        open(h.host, h.port, [this] {
            stopTimers();
            connect(&poll_, &QTimer::timeout, this, [this] { send(char(0xF0) + padded(cfg_.station.callsign, 10)); });
            poll_.start(5000);
            send(char(0xF0) + padded(cfg_.station.callsign, 10));
        });
    }
    void unlink() override
    {
        if (state_ != State::Off && !addr_.isNull())
            for (int i = 0; i < 3; ++i) send(char(0xF1) + padded(cfg_.station.callsign, 10));
        stopTimers();
        setState(State::Off);
        target_.clear();
    }
    void transmit(const QByteArray &rec) override
    {
        if (state_ == State::Linked && rec.startsWith("P25XN")) send(rec.mid(5));
    }
protected:
    void received(const QByteArray &d) override
    {
        if (d.isEmpty()) return;
        const uint8_t t = uint8_t(d[0]);
        if (t == 0xF0) setState(State::Linked);
        else if ((t >= 0x62 && t <= 0x73) || t == 0x80) emit record("P25XN" + d);
    }
};

// ---------------------------------------------------------------- NXDN
class NxdnClient : public NetClient {
public:
    using NetClient::NetClient;
    void link(const QString &target) override
    {
        HostEntry h;
        if (!hosts_.find("NXDN", target, h)) { setState(State::Failed, "unknown talkgroup " + target); return; }
        unlink();
        tg_ = h.id.toUShort();
        target_ = "TG " + h.id;
        open(h.host, h.port, [this] {
            stopTimers();
            connect(&poll_, &QTimer::timeout, this, [this] { send(packet('P')); });
            poll_.start(5000);
            send(packet('P'));
        });
    }
    void unlink() override
    {
        if (state_ != State::Off && !addr_.isNull())
            for (int i = 0; i < 3; ++i) send(packet('U'));
        stopTimers();
        setState(State::Off);
        target_.clear();
    }
    void transmit(const QByteArray &rec) override
    {
        // "NXDXN" + 102-byte Icom packet with the 33 NXDN bytes at offset 40 -> 43-byte NXDND
        if (state_ != State::Linked || !rec.startsWith("NXDXN") || rec.size() < 5 + 73) return;
        const QByteArray d = rec.mid(5 + 40, 33);
        const uint8_t lich = uint8_t(d[0]);
        if (lich == 0x81 || lich == 0x83) {   // header / trailer: learn the IDs
            src_ = uint16_t((uint8_t(d[8]) << 8) | uint8_t(d[9]));
            dst_ = uint16_t((uint8_t(d[10]) << 8) | uint8_t(d[11]));
        }
        QByteArray b("NXDND");
        b += char(src_ >> 8);
        b += char(src_);
        b += char(dst_ >> 8);
        b += char(dst_);
        uint8_t flags = 0x01;   // group call
        if (lich == 0x81 || lich == 0x83) {
            if (uint8_t(d[5]) == 0x01) flags |= 0x04;   // start
            if (uint8_t(d[5]) == 0x08) flags |= 0x08;   // end
        }
        b += char(flags);
        b += d;
        send(b);
    }
protected:
    QByteArray packet(char kind) const
    {
        QByteArray b("NXDN");
        b += kind;
        b += padded(cfg_.station.callsign, 10);
        b += char(tg_ >> 8);
        b += char(tg_);
        return b;
    }
    void received(const QByteArray &d) override
    {
        if (d.startsWith("NXDNP")) setState(State::Linked);
        else if (d.startsWith("NXDND") && d.size() >= 43) {
            setState(State::Linked);
            QByteArray icom(102, 0);
            std::memcpy(icom.data(), "ICOM\x01\x01\x08\xE0", 8);
            std::memcpy(icom.data() + 40, d.constData() + 10, 33);
            emit record("NXDXN" + icom);
        }
    }
    uint16_t tg_ = 0, src_ = 0, dst_ = 0;
};

// ---------------------------------------------------------------- M17
uint64_t m17Encode(const QString &s)
{
    static const char *alpha = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
    const QByteArray cs = s.toUpper().toLatin1();
    uint64_t e = 0;
    for (int i = int(cs.size()) - 1; i >= 0; --i) {
        const char *p = cs[i] ? std::strchr(alpha, cs[i]) : nullptr;
        e = e * 40 + uint64_t(p ? p - alpha : 0);
    }
    return e;
}

class M17Client : public NetClient {
public:
    using NetClient::NetClient;
    void link(const QString &target) override
    {
        const QString refl = target.section(' ', 0, 0).toUpper();
        const QString module = target.section(' ', 1, 1).toUpper();
        HostEntry h;
        if (!hosts_.find("M17", refl, h) || module.size() != 1) { setState(State::Failed, "unknown reflector " + target); return; }
        unlink();
        module_ = module[0].toLatin1();
        target_ = refl + " " + module;
        open(h.host, h.port, [this] {
            stopTimers();
            connect(&poll_, &QTimer::timeout, this, [this] { if (state_ != State::Linked) send(connectPacket()); });
            poll_.start(5000);   // retry the connect until acknowledged
            send(connectPacket());
        });
    }
    void unlink() override
    {
        if (state_ != State::Off && !addr_.isNull()) send("DISC" + encoded());
        stopTimers();
        setState(State::Off);
        target_.clear();
    }
    void transmit(const QByteArray &rec) override
    {
        if (state_ == State::Linked && rec.startsWith("M17XN")) send(rec.mid(5));
    }
protected:
    QByteArray encoded() const
    {
        // Our node: callsign with a "D" (desktop) module so we don't clash with a hotspot's login.
        const uint64_t e = m17Encode(cfg_.station.callsign.leftJustified(8, ' ', true) + "D");
        QByteArray b(6, 0);
        for (int i = 0; i < 6; ++i) b[i] = char(e >> (40 - 8 * i));
        return b;
    }
    QByteArray connectPacket() const { return "CONN" + encoded() + char(module_); }
    void received(const QByteArray &d) override
    {
        if (d.startsWith("ACKN")) setState(State::Linked);
        else if (d.startsWith("NACK")) setState(State::Failed, "reflector refused the connection");
        else if (d.startsWith("PING")) send("PONG" + encoded());
        else if (d.startsWith("DISC")) setState(State::Failed, "disconnected by the reflector");
        else if (d.startsWith("M17 ") && d.size() >= 54) emit record("M17XN" + d.left(54));
    }
    char module_ = 'A';
};

// ---------------------------------------------------------------- D-Star (DExtra / DCS)
class DStarClient : public NetClient {
public:
    using NetClient::NetClient;
    void link(const QString &target) override
    {
        const QString refl = target.section(' ', 0, 0).toUpper();
        const QString module = target.section(' ', 1, 1).toUpper();
        HostEntry h;
        if (!hosts_.find("D-Star", refl, h) || module.size() != 1) { setState(State::Failed, "unknown reflector " + target); return; }
        unlink();
        proto_ = refl.startsWith("REF") ? Proto::DPlus : refl.startsWith("XRF") ? Proto::DExtra : Proto::DCS;
        dcs_ = proto_ == Proto::DCS;
        reflModule_ = module[0].toLatin1();
        reflector_ = refl;
        target_ = refl + " " + module;
        const QString host = h.host;
        const quint16 port = h.port;
        auto go = [this, host, port] {
            open(host, port, [this] {
                stopTimers();
                connect(&poll_, &QTimer::timeout, this, [this] {
                    if (state_ != State::Linked) send(connectPacket(true));   // retry until ACK
                    else send(pollPacket());
                });
                poll_.start(proto_ == Proto::DPlus ? 1000 : dcs_ ? 5000 : 10000);
                send(connectPacket(true));
            });
        };
        if (proto_ == Proto::DPlus) authenticate(go);   // REF reflectors only accept registered, logged-in stations
        else go();
    }
    void unlink() override
    {
        if (state_ != State::Off && !addr_.isNull())
            for (int i = 0; i < 3; ++i) send(connectPacket(false));
        stopTimers();
        setState(State::Off);
        target_.clear();
        rxId_ = 0;
    }
    void transmit(const QByteArray &rec) override
    {
        // "DSTRN" + DSRP: 0x20 header (id, 0, flags(3) RPT2 RPT1 YOUR MY SUFFIX CRC), 0x21 voice
        if (state_ != State::Linked || !rec.startsWith("DSTRNDSRP") || rec.size() < 5 + 21) return;
        const QByteArray p = rec.mid(5);
        const uint8_t type = uint8_t(p[4]);
        if (type == 0x20 && p.size() >= 49) {
            txId_ = uint16_t(QDateTime::currentMSecsSinceEpoch() & 0xFFFF) | 1;
            // Route it through the reflector: RPT1 = our node, RPT2 = reflector + module.
            txHeader_ = QByteArray(3, 0) + padded(reflector_, 7) + char(reflModule_) + myNode() +
                        p.mid(8 + 3 + 16, 8) /*YOUR*/ + p.mid(8 + 3 + 24, 8) /*MY*/ + p.mid(8 + 3 + 32, 4) /*SUFFIX*/;
            txSeq_ = 0;
            if (!dcs_) {
                QByteArray h("DSVT");
                h += QByteArray("\x10\x00\x00\x00\x20\x00\x02\x01", 8);
                h += char(txId_ & 0xFF);
                h += char(txId_ >> 8);
                h += char(0x80);
                h += txHeader_;
                h += crc(txHeader_);
                if (proto_ == Proto::DPlus) h.prepend(QByteArray("\x3A\x80", 2));
                for (int i = 0; i < 2; ++i) send(h);
            }
        } else if (type == 0x21 && p.size() >= 21 && !txHeader_.isEmpty()) {
            const uint8_t seq = uint8_t(p[7]);
            const QByteArray frame = p.mid(9, 12);   // AMBE(9) + slow data(3)
            if (!dcs_) {
                QByteArray v("DSVT");
                v += QByteArray("\x20\x00\x00\x00\x20\x00\x02\x01", 8);
                v += char(txId_ & 0xFF);
                v += char(txId_ >> 8);
                v += char(seq);
                if (proto_ == Proto::DPlus && (seq & 0x40)) {   // end: silence + end pattern
                    v += QByteArray("\x9E\x8D\x32\x88\x26\x1A\x3F\x61\xE8", 9);
                    v += QByteArray("\x55\x55\x55\x55\xC8\x7A", 6);
                    v.prepend(QByteArray("\x20\x80", 2));
                } else {
                    v += frame;
                    if (proto_ == Proto::DPlus) v.prepend(QByteArray("\x1D\x80", 2));
                }
                send(v);
            } else {
                QByteArray v(100, 0);
                std::memcpy(v.data(), "0001", 4);
                std::memcpy(v.data() + 4, txHeader_.constData(), 39);
                v[43] = char(txId_ & 0xFF);
                v[44] = char(txId_ >> 8);
                v[45] = char(seq);
                std::memcpy(v.data() + 46, frame.constData(), 12);
                if (seq & 0x40) { v[55] = 0x55; v[56] = 0x55; v[57] = 0x55; }
                v[58] = char(txSeq_ & 0xFF);
                v[59] = char((txSeq_ >> 8) & 0xFF);
                v[60] = char((txSeq_ >> 16) & 0xFF);
                v[61] = 0x01;
                v[63] = 0x21;
                ++txSeq_;
                send(v);
            }
            if (seq & 0x40) txHeader_.clear();
        }
    }
protected:
    QByteArray myNode() const   // our "repeater" callsign: callsign padded to 7 + module letter
    {
        const QString m = cfg_.network.dstarModule.isEmpty() ? QString("D") : cfg_.network.dstarModule.left(1).toUpper();
        return padded(cfg_.station.callsign, 7) + m.toLatin1();
    }
    QByteArray connectPacket(bool link) const
    {
        const QByteArray node = myNode();
        if (proto_ == Proto::DPlus)   // step 1 (link) / unlink; step 2 follows the reflector's reply
            return QByteArray("\x05\x00\x18\x00", 4) + char(link ? 0x01 : 0x00);
        if (!dcs_) {   // DExtra: callsign(7) ' ' nodeModule reflModule 0  (11 bytes)
            QByteArray b = node.left(7) + ' ' + node.right(1);
            b += link ? char(reflModule_) : ' ';
            b += char(0);
            return b;
        }
        QByteArray b = node.left(7) + ' ' + node.right(1);   // DCS: callsign(7) ' ' nodeModule reflModule 0 reflector(8) [html]
        b += link ? char(reflModule_) : ' ';
        b += char(0);
        b += padded(reflector_, 7) + ' ';
        if (!link) return b;   // 19 bytes (reflector name at 11..18)
        QByteArray html("<table border=\"0\" width=\"95%\"><tr><td width=\"4%\"></td><td width=\"96%\">"
                        "<font size=\"2\"><b>Digital-Transceiver</b></font></td></tr></table>");
        html = html.left(500);
        b += html + QByteArray(500 - html.size(), 0);
        return b;   // 519 bytes
    }
    QByteArray pollPacket() const
    {
        if (proto_ == Proto::DPlus) return QByteArray("\x03\x60\x00", 3);
        if (!dcs_) return myNode() + char(0);              // 9 bytes
        return myNode() + char(0) + padded(reflector_, 7) + char(reflModule_);   // 17 bytes
    }
    static QByteArray crc(const QByteArray &h)
    {
        uint16_t c = 0xFFFF;
        for (char ch : h) {
            c ^= uint8_t(ch);
            for (int b = 0; b < 8; ++b) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1);
        }
        c = uint16_t(~c);
        return QByteArray(1, char(c & 0xFF)) + char(c >> 8);
    }
    void emitHeader(uint16_t id, const QByteArray &fields39)   // flags(3) RPT2 RPT1 YOUR MY SUFFIX
    {
        rxId_ = id;
        emit record(QByteArray("DSTRNDSRP") + char(0x20) + char(id >> 8) + char(id & 0xFF) + char(0) + fields39 + crc(fields39));
    }
    void emitVoice(uint16_t id, uint8_t seq, const QByteArray &frame12)
    {
        emit record(QByteArray("DSTRNDSRP") + char(0x21) + char(id >> 8) + char(id & 0xFF) + char(seq) + char(0) + frame12);
    }
    void receivedDPlus(const QByteArray &d)
    {
        if (d.size() == 5 && uint8_t(d[0]) == 0x05) {   // link step 1 answered: log in (callsign + DV019999)
            if (uint8_t(d[4]) == 0x01 && state_ != State::Linked) {
                QByteArray b("\x1C\xC0\x04\x00", 4);
                QByteArray call = cfg_.station.callsign.toLatin1().left(16);
                b += call + QByteArray(16 - call.size(), 0);
                b += "DV019999";
                send(b);
            } else if (uint8_t(d[4]) == 0x00) {
                setState(State::Failed, "unlinked by the reflector");
            }
            return;
        }
        if (d.size() == 8 && uint8_t(d[0]) == 0x08) {
            if (d.mid(4, 4) == "OKRW") setState(State::Linked);
            else setState(State::Failed, "reflector refused the login (callsign registered for D-Star?)");
            return;
        }
        if (d.size() == 3) return;   // poll
        if (d.size() >= 58 && uint8_t(d[0]) == 0x3A && d.mid(2, 4) == "DSVT" && uint8_t(d[6]) == 0x10) {
            const QByteArray f = d.mid(17, 39);   // flags(3) RPT2 RPT1 YOUR MY SUFFIX
            const QByteArray want = padded(reflector_, 7) + char(reflModule_);
            if (f.mid(3, 8) != want && f.mid(11, 8) != want) return;   // another module on this reflector
            dplusId_ = uint16_t(uint8_t(d[14]) | (uint8_t(d[15]) << 8));
            emitHeader(dplusId_, f);
            return;
        }
        if (d.size() >= 29 && (uint8_t(d[0]) == 0x1D || uint8_t(d[0]) == 0x20) && d.mid(2, 4) == "DSVT") {
            const uint16_t id = uint16_t(uint8_t(d[14]) | (uint8_t(d[15]) << 8));
            if (id != dplusId_ || id == 0) return;
            uint8_t seq = uint8_t(d[16]);
            if (uint8_t(d[0]) == 0x20) seq |= 0x40;
            emitVoice(id, seq, d.mid(17, 12));
            if (seq & 0x40) dplusId_ = 0;
        }
    }
    void authenticate(std::function<void()> then)
    {
        // Log in to the D-Star registration server so REF reflectors accept this address.
        if (authOk_ && authTime_.isValid() && authTime_.secsTo(QDateTime::currentDateTime()) < 6 * 3600) { then(); return; }
        setState(State::Connecting, "logging in to the D-Star registration server");
        auto *t = new QTcpSocket(this);
        auto done = std::make_shared<bool>(false);
        auto finish = [this, t, then, done](bool ok) {
            if (*done) return;
            *done = true;
            t->deleteLater();
            authOk_ = ok;
            if (ok) authTime_ = QDateTime::currentDateTime();
            then();   // try the link either way; the reflector will say if it refuses
        };
        connect(t, &QTcpSocket::connected, this, [this, t] {
            QByteArray b(56, ' ');
            b[0] = char(0x38); b[1] = char(0xC0); b[2] = 0x01; b[3] = 0x00;
            const QByteArray call = cfg_.station.callsign.toLatin1().left(8);
            for (int i = 0; i < call.size(); ++i) b[4 + i] = call[i];
            std::memcpy(b.data() + 12, "DV019999", 8);
            std::memcpy(b.data() + 28, "W7IB", 4);
            b[32] = 0x01;
            std::memcpy(b.data() + 40, "DHS0257", 7);
            t->write(b);
        });
        connect(t, &QTcpSocket::readyRead, this, [t] { t->readAll(); });   // the gateway list: not needed
        connect(t, &QTcpSocket::disconnected, this, [finish] { finish(true); });
        connect(t, &QTcpSocket::errorOccurred, this, [finish](QAbstractSocket::SocketError e) {
            finish(e == QAbstractSocket::RemoteHostClosedError);
        });
        QTimer::singleShot(10000, this, [finish] { finish(false); });
        t->connectToHost("auth.dstargateway.org", 20001);
    }
    void received(const QByteArray &d) override
    {
        if (proto_ == Proto::DPlus) { receivedDPlus(d); return; }
        if (d.size() == 14 && d.mid(10, 3) == "ACK") { setState(State::Linked); return; }
        if (d.size() == 14 && d.mid(10, 3) == "NAK") { setState(State::Failed, "reflector refused the link"); return; }
        if (!dcs_) {
            if (d.size() == 9) return;   // reflector poll
            if (d.startsWith("DSVT") && d.size() >= 56 && uint8_t(d[4]) == 0x10) {
                setState(State::Linked);
                emitHeader(uint16_t(uint8_t(d[12]) | (uint8_t(d[13]) << 8)), d.mid(15, 39));
            } else if (d.startsWith("DSVT") && d.size() >= 27 && uint8_t(d[4]) == 0x20) {
                setState(State::Linked);
                emitVoice(uint16_t(uint8_t(d[12]) | (uint8_t(d[13]) << 8)), uint8_t(d[14]), d.mid(15, 12));
            }
            return;
        }
        if (d.size() == 17 || d.size() == 22) {   // reflector poll: answer it
            send(pollPacket());
            return;
        }
        if (d.startsWith("0001") && d.size() >= 61) {
            setState(State::Linked);
            const uint16_t id = uint16_t(uint8_t(d[43]) | (uint8_t(d[44]) << 8));
            if (id != rxId_) emitHeader(id, d.mid(4, 39));   // every DCS frame carries the header
            emitVoice(id, uint8_t(d[45]), d.mid(46, 12));
        }
    }
    enum class Proto { DExtra, DCS, DPlus };
    Proto proto_ = Proto::DCS;
    bool dcs_ = true;
    uint16_t dplusId_ = 0;
    bool authOk_ = false;
    QDateTime authTime_;
    char reflModule_ = 'C';
    QString reflector_;
    uint16_t rxId_ = 0, txId_ = 0;
    uint32_t txSeq_ = 0;
    QByteArray txHeader_;
};

// ---------------------------------------------------------------- DMR (BrandMeister / Homebrew)
class DmrClient : public NetClient {
public:
    using NetClient::NetClient;
    bool canLink() const override { return false; }   // talkgroups are chosen per transmission
    void link(const QString &) override
    {
        unlink();
        if (cfg_.network.dmrPassword.isEmpty()) { setState(State::Failed, "set the BrandMeister password in Settings → Networks"); return; }
        if (cfg_.station.repeaterId == 0) { setState(State::Failed, "set the hotspot ID in Settings → Station"); return; }
        const QString host = cfg_.network.dmrMaster;
        target_ = host.contains("brandmeister", Qt::CaseInsensitive) ? "BM " + host.section('.', 0, 0) : host;
        open(host, quint16(cfg_.network.dmrPort), [this] {
            stopTimers();
            stage_ = Stage::Login;
            connect(&poll_, &QTimer::timeout, this, [this] {
                if (stage_ == Stage::Running) send("RPTPING" + id());
                else { stage_ = Stage::Login; send("RPTL" + id()); }   // retry the login
            });
            poll_.start(10000);
            send("RPTL" + id());
        });
    }
    void unlink() override
    {
        if (state_ != State::Off && !addr_.isNull()) send("RPTCL" + id());
        stopTimers();
        stage_ = Stage::Login;
        setState(State::Off);
        target_.clear();
    }
    void transmit(const QByteArray &rec) override
    {
        if (state_ == State::Linked && rec.startsWith("DMRD")) send(rec.left(55));
    }
protected:
    enum class Stage { Login, Auth, Config, Running };
    QByteArray id() const
    {
        QByteArray b(4, 0);
        qToBigEndian(quint32(cfg_.station.repeaterId), b.data());
        return b;
    }
    QByteArray config() const
    {
        const Settings::Network &n = cfg_.network;
        QByteArray c = padded(cfg_.station.callsign, 8);
        c += QByteArray::number(n.dmrRxFreqHz).rightJustified(9, '0');
        c += QByteArray::number(n.dmrTxFreqHz).rightJustified(9, '0');
        c += "01";                                                     // power
        c += QByteArray::number(cfg_.station.colourCode).rightJustified(2, '0');
        c += "0.000000";                                               // latitude  (%08f, as MMDVM sends it)
        c += "00.000000";                                              // longitude (%09f: BrandMeister rejects other forms)
        c += "000";                                                    // height
        c += padded(n.dmrLocation, 20);
        c += padded("Digital-Transceiver", 19);
        c += "4";                                                      // slots: simplex
        c += padded("", 124);                                          // URL
        c += padded("20261114", 40);                                   // software
        c += padded("MMDVM", 40);                                      // package: BrandMeister only accepts MMDVM here
        return c;
    }
    void received(const QByteArray &d) override
    {
        if (d.startsWith("DMRD")) {
            if (d.size() >= 53) emit record(d);
            return;
        }
        if (d.startsWith("MSTNAK")) {
            const char *why = stage_ == Stage::Auth ? "wrong BrandMeister password" :
                              stage_ == Stage::Login ? "hotspot ID refused" : "configuration refused";
            setState(State::Failed, why);
            emit message(QString("DMR: %1").arg(why), true);
            stage_ = Stage::Login;
            return;
        }
        if (d.startsWith("MSTCL")) { setState(State::Failed, "closed by the master"); stage_ = Stage::Login; return; }
        if (d.startsWith("RPTACK")) {
            switch (stage_) {
            case Stage::Login: {
                const QByteArray salt = d.mid(6, 4);
                const QByteArray hash = QCryptographicHash::hash(salt + cfg_.network.dmrPassword.toUtf8(),
                                                                 QCryptographicHash::Sha256);
                send("RPTK" + id() + hash);
                stage_ = Stage::Auth;
                break;
            }
            case Stage::Auth:
                send("RPTC" + id() + config());
                stage_ = Stage::Config;
                break;
            case Stage::Config:
                stage_ = Stage::Running;
                setState(State::Linked);
                break;
            default:
                break;
            }
        }
    }
    Stage stage_ = Stage::Login;
};
}   // namespace

std::unique_ptr<NetClient> makeClient(const QString &mode, Settings &cfg, HostFiles &hosts)
{
    if (mode == "DMR") return std::make_unique<DmrClient>(mode, cfg, hosts);
    if (mode == "D-Star") return std::make_unique<DStarClient>(mode, cfg, hosts);
    if (mode == "YSF") return std::make_unique<YsfClient>(mode, cfg, hosts);
    if (mode == "P25") return std::make_unique<P25Client>(mode, cfg, hosts);
    if (mode == "NXDN") return std::make_unique<NxdnClient>(mode, cfg, hosts);
    if (mode == "M17") return std::make_unique<M17Client>(mode, cfg, hosts);
    return nullptr;
}

// ---------------------------------------------------------------- DAPNET
DapnetClient::DapnetClient(Settings &cfg, QObject *parent) : QObject(parent), cfg_(cfg)
{
    connect(&sock_, &QTcpSocket::connected, this, [this] {
        // Identify exactly as DAPNETGateway does: the server may not accept other software names.
        sock_.write(QString("[DAPNETGateway v20260214 %1 %2]\r\n")
                        .arg(cfg_.network.dapnetCallsign, cfg_.network.dapnetAuthKey).toUtf8());
    });
    connect(&sock_, &QTcpSocket::readyRead, this, [this] {
        buf_ += sock_.readAll();
        int nl;
        while ((nl = buf_.indexOf('\n')) >= 0) {
            line(buf_.left(nl).trimmed());
            buf_.remove(0, nl + 1);
        }
    });
    connect(&sock_, &QTcpSocket::disconnected, this, [this] {
        loggedIn_ = false;
        emit changed();
        if (refusals_ < 2) scheduleRetry();
    });
    connect(&sock_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError e) {
        if (!buf_.trimmed().isEmpty()) { line(buf_.trimmed()); buf_.clear(); }   // a last line without a newline
        if (e == QAbstractSocket::RemoteHostClosedError && !loggedIn_) {
            // Closed straight after the login: the transmitter name or auth key was refused. The server
            // blocks addresses that keep failing, so stop after the second refusal until settings change.
            if (++refusals_ >= 2) {
                retry_.stop();
                lastError_ = "login refused, check the transmitter name and auth key in Settings";
                emit message("DAPNET: " + lastError_ + " (not retrying)", true);
                emit changed();
                return;
            }
        }
        lastError_ = sock_.errorString();
        const int mins = scheduleRetry();
        emit message(QString("DAPNET: %1%2").arg(lastError_, mins ? QString(" — retrying in %1 min").arg(mins) : QString()), true);
        emit changed();
    });
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &DapnetClient::start);
}

// The DAPNET server firewalls any address that opens more than 5 connections a minute
// (for 10 minutes), so back off: 1, 2, 4, 8 then 15 minutes between attempts.
int DapnetClient::scheduleRetry()
{
    if (!cfg_.network.dapnet || retry_.isActive()) return 0;
    const int mins = std::min(15, 1 << std::min(failures_, 4));
    ++failures_;
    retry_.start(mins * 60000);
    return mins;
}

void DapnetClient::start()
{
    if (!cfg_.network.dapnet || cfg_.network.dapnetCallsign.isEmpty() || cfg_.network.dapnetAuthKey.isEmpty()) return;
    sock_.abort();
    buf_.clear();
    sock_.connectToHost(cfg_.network.dapnetServer, quint16(cfg_.network.dapnetPort));
}

void DapnetClient::stop()
{
    retry_.stop();
    sock_.abort();
    loggedIn_ = false;
    emit changed();
}

void DapnetClient::line(const QByteArray &l)
{
    if (l.isEmpty()) return;
    switch (l[0]) {
    case '2':   // time sync (also confirms the login)
        if (!loggedIn_) {
            loggedIn_ = true;
            failures_ = 0;
            refusals_ = 0;
            lastError_.clear();
            emit changed();
        }
        sock_.write(l + ":0000\r\n");
        sock_.write("+\r\n");
        break;
    case '3':
    case '4':
        sock_.write("+\r\n");
        break;
    case '7':
        lastError_ = "login refused";
        emit message("DAPNET login failed: " + QString::fromUtf8(l.mid(2)), true);
        emit changed();
        sock_.write("+\r\n");
        break;
    case '#': {   // "#ID type:speed:RIC(hex):func:text"
        bool ok = false;
        const unsigned id = l.mid(1, 2).toUInt(&ok, 16);
        const QList<QByteArray> p = l.mid(4).split(':');
        const QByteArray reply = QString("#%1 %2\r\n").arg((id + 1) % 256, 2, 16, QChar('0')).toUpper().toLatin1();
        if (p.size() < 5) {
            sock_.write(reply.left(4) + "-\r\n");
            break;
        }
        const unsigned ric = p[2].toUInt(nullptr, 16);
        const unsigned func = p[3].toUInt();
        const QByteArray text = QString::fromLatin1(l.mid(4)).section(':', 4).toLatin1();
        const char *f = func == 3 ? "Alphanumeric" : func == 0 ? "Numeric" : "Tone";
        emit record("PAGE" + QString("M: %1 Sending message in slot 0 to %2, type %3, func %4: \"%5\"")
                                 .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss.zzz"))
                                 .arg(ric, 7, 10, QChar('0')).arg(QString::fromLatin1(p[0])).arg(f)
                                 .arg(QString::fromLatin1(text)).toUtf8());
        sock_.write(QString("#%1 +\r\n").arg((id + 1) % 256, 2, 16, QChar('0')).toUpper().toLatin1());
        break;
    }
    default:
        sock_.write("-\r\n");
    }
}
