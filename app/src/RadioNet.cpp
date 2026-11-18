// Radio: network connections (DMR, D-Star, YSF, P25, NXDN, M17 and DAPNET), the Pi-Star guard,
// links and the downloaded ID lists.
#include "Radio.h"

#include <QFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTime>

NetClient *Radio::client(const QString &mode) const
{
    auto it = nets_.find(mode);
    return it == nets_.end() ? nullptr : it->second.get();
}

void Radio::start()
{
    hosts_->refresh();
    connect(&piTimer_, &QTimer::timeout, this, &Radio::checkPiStar);
    checkPiStar();   // logs in once it knows no Pi-Star is online
    piTimer_.start(30000);

    // Links still waiting (no network yet, lookup failed, list not downloaded) are retried when a
    // host list arrives and every 30 s.
    connect(hosts_.get(), &HostFiles::updated, this, [this] { if (networksUp_) startNetworks(); });
    connect(&relinkTimer_, &QTimer::timeout, this, [this] { if (networksUp_) startNetworks(); });
    relinkTimer_.start(30000);
}

// ---------------------------------------------------------------- Pi-Star guard
// Don't log in while a Pi-Star with our identity is online: both would use the same IDs.
void Radio::checkPiStar()
{
    if (!cfg_.network.piStarCheck || cfg_.network.piStarHost.trimmed().isEmpty()) {
        piStarOnline_ = false;
        piChecked_ = true;
        if (!networksUp_) startNetworks();
        return;
    }
    QNetworkRequest req(QUrl("http://" + cfg_.network.piStarHost.trimmed() + "/"));
    req.setTransferTimeout(4000);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Digital-Transceiver");
    QNetworkReply *r = piNet_.get(req);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        const bool online = r->error() == QNetworkReply::NoError && r->readAll().contains("Pi-Star");
        const bool first = !piChecked_;
        piChecked_ = true;
        if (online) {
            if (!piStarOnline_ || first) {
                piStarOnline_ = true;
                stopNetworks();
                logEvent("WARN", "Pi-Star detected online at " + cfg_.network.piStarHost + "; networks stopped");
            }
        } else if (piStarOnline_ || first) {
            if (piStarOnline_) logEvent("INFO", "Pi-Star went offline; starting networks");
            piStarOnline_ = false;
            if (!networksUp_) startNetworks();
        }
    });
}

// ---------------------------------------------------------------- networks
// Creates the clients on the first call; links every mode that isn't linked to its start-up
// link (the last one used) on every call.
void Radio::startNetworks()
{
    if (piStarOnline_ || quitting_) return;
    if (!networksUp_) {
        networksUp_ = true;
        setStatus("Connecting to the networks");
        for (const QString &m : kModes) {
            if (!cfg_.network.modes.contains(m)) continue;
            auto c = makeClient(m, cfg_, *hosts_);
            NetClient *raw = c.get();
            connect(raw, &NetClient::record, this, &Radio::dispatchRecord);
            connect(raw, &NetClient::changed, this, &Radio::onNetChanged);
            connect(raw, &NetClient::changed, this, [this, raw] {   // event log: real state changes only
                static const char *names[] = {"off", "connecting", "linked", "failed"};
                QString st = QString("%1 %2").arg(names[int(raw->state())], raw->target());
                if (!raw->detail().isEmpty()) st += " (" + raw->detail() + ")";
                if (lastNetState_.value(raw->mode()) == st) return;
                lastNetState_[raw->mode()] = st;
                logEvent(raw->state() == NetClient::State::Failed ? "ERROR" : "INFO", raw->mode() + ": " + st.trimmed());
                if (qEnvironmentVariableIntValue("DT_DEBUG")) {
                    fprintf(stdout, "%s %s: %s\n", qPrintable(QTime::currentTime().toString("HH:mm:ss")),
                            qPrintable(raw->mode()), qPrintable(st));
                    fflush(stdout);
                }
            });
            connect(raw, &NetClient::message, this, [this](const QString &t, bool error) { setStatus(t, error); });
            nets_[m] = std::move(c);
        }
        if (cfg_.network.dapnet) {
            dapnet_ = std::make_unique<DapnetClient>(cfg_, this);
            connect(dapnet_.get(), &DapnetClient::record, this, &Radio::dispatchRecord);
            connect(dapnet_.get(), &DapnetClient::changed, this, [this] {
                const QString st = dapnet_->loggedIn() ? "logged in to " + cfg_.network.dapnetServer : "not logged in";
                if (lastNetState_.value("POCSAG") == st) return;
                lastNetState_["POCSAG"] = st;
                logEvent("INFO", "DAPNET: " + st);
            });
            connect(dapnet_.get(), &DapnetClient::message, this, [this](const QString &t, bool e) { setStatus(t, e); });
            dapnet_->start();
        }
    }
    const auto &n = cfg_.network;
    const QHash<QString, QString> startup = {
        {"DMR", "BM"},
        {"D-Star", n.dstarReflector},
        {"YSF", n.ysfReflector},
        {"P25", n.p25Tg ? QString::number(n.p25Tg) : QString()},
        {"NXDN", n.nxdnTg ? QString::number(n.nxdnTg) : QString()},
        {"M17", n.m17Reflector}};
    for (auto &[m, c] : nets_)
        if (c->state() != NetClient::State::Linked && c->state() != NetClient::State::Connecting &&
            !startup.value(m).isEmpty())
            c->link(startup.value(m));
}

void Radio::stopNetworks()
{
    networksUp_ = false;
    if (txActive_) stopTx("networks stopped");
    for (auto &[m, c] : nets_) c->unlink();
    nets_.clear();
    if (dapnet_) dapnet_->stop();
    dapnet_.reset();
}

void Radio::onNetChanged()
{
    if (NetClient *d = client("D-Star")) reflector_ = d->endpoint();
    for (auto &[m, c] : nets_)
        if (c->state() == NetClient::State::Linked) { setStatus("Connected to the networks"); break; }
}

QString Radio::endpoint(const QString &mode) const
{
    if (piStarOnline_) return "Pi-Star online";
    NetClient *c = client(mode);
    if (!c) return {};
    switch (c->state()) {
    case NetClient::State::Linked: return c->endpoint();
    case NetClient::State::Connecting: return c->target().isEmpty() ? QString("connecting") : c->target() + "...";
    case NetClient::State::Failed: return "! " + (c->detail().isEmpty() ? c->target() : c->detail());
    case NetClient::State::Off: break;
    }
    return {};
}

QString Radio::currentLink(const QString &mode) const
{
    NetClient *c = client(mode);
    if (!c) return {};
    return c->endpoint().isEmpty() ? c->target() : c->endpoint();
}

bool Radio::linked(const QString &mode) const
{
    NetClient *c = client(mode);
    return !piStarOnline_ && c && c->state() == NetClient::State::Linked;
}

bool Radio::networkFault() const
{
    if (piStarOnline_) return true;
    for (const auto &[m, c] : nets_)
        if (c->state() == NetClient::State::Failed) return true;
    return dapnet_ && !dapnet_->loggedIn() && !dapnet_->lastError().isEmpty();
}

QList<HostEntry> Radio::hostList(const QString &mode) const
{
    return hosts_->list(mode);
}

void Radio::link(const QString &mode, const QString &target)
{
    NetClient *c = client(mode);
    if (!c || !c->canLink()) return;
    if (target.isEmpty()) c->unlink();
    else c->link(target);
    // Remember it as the start-up link.
    auto &n = cfg_.network;
    if (mode == "D-Star") n.dstarReflector = target;
    else if (mode == "YSF") n.ysfReflector = target;
    else if (mode == "M17") n.m17Reflector = target;
    else if (mode == "P25") n.p25Tg = target.toUInt();
    else if (mode == "NXDN") n.nxdnTg = target.toUInt();
    saveSettings();
    setStatus(target.isEmpty() ? mode + ": unlinked" : QString("%1: linking to %2").arg(mode, target));
}

void Radio::txSend(const QByteArray &record)
{
    if (NetClient *c = client(record.startsWith("DMRD") ? QString("DMR") : record.startsWith("DSTR") ? QString("D-Star")
                                : record.startsWith("YSFX") ? QString("YSF") : record.startsWith("P25X") ? QString("P25")
                                : record.startsWith("NXDX") ? QString("NXDN") : QString("M17")))
        c->transmit(record);
    // Reflectors don't send our own audio back, so log it here (muted while transmitting).
    ownTx_ = true;
    dispatchRecord(record);
    ownTx_ = false;
}

// ---------------------------------------------------------------- ID lists (downloaded)
void Radio::loadIds()
{
    QFile f(hosts_->dmrIdsPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    ids_.clear();
    for (const QByteArray &line : f.readAll().split('\n')) {   // id <tab> callsign <tab> name
        const QList<QByteArray> p = line.split('\t');
        bool ok = false;
        const uint32_t id = p.value(0).toUInt(&ok);
        if (!ok || p.size() < 2) continue;
        const QString call = QString::fromUtf8(p[1]).trimmed();
        const QString name = QString::fromUtf8(p.value(2)).trimmed();
        ids_.insert(id, {call, name});
        if (!name.isEmpty() && !nameByCall_.contains(call.toUpper())) nameByCall_.insert(call.toUpper(), name);
    }
}

void Radio::loadNxdnIds()
{
    if (cfg_.station.nxdnId) nxdnIds_.insert(cfg_.station.nxdnId, {cfg_.station.callsign, QString()});
    QFile f(hosts_->nxdnIdsPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QByteArray &line : f.readAll().split('\n')) {   // RADIO_ID,CALLSIGN,FIRST_NAME,...
        const QList<QByteArray> p = line.split(',');
        bool ok = false;
        const uint32_t id = p.value(0).toUInt(&ok);
        if (ok && p.size() >= 2) nxdnIds_.insert(id, {QString::fromUtf8(p[1]).trimmed(), QString::fromUtf8(p.value(2)).trimmed()});
    }
    if (cfg_.station.nxdnId && !nxdnIds_.contains(cfg_.station.nxdnId))
        nxdnIds_.insert(cfg_.station.nxdnId, {cfg_.station.callsign, QString()});
}
