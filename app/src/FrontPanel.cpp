#include "FrontPanel.h"
#include "Radio.h"

#include <QDateTime>
#include <QNetworkInterface>
#include <QTime>
#include <algorithm>
#include <limits>

namespace {
constexpr qint64 kBrowseMs = 6000;    // browsing ends after this long without a control
constexpr qint64 kModeHoldMs = 700;   // mode button held this long: TX mode
constexpr qint64 kShutdownHoldMs = 3000;   // Menu held this long: shut down
constexpr qint64 kShutdownHintMs = 800;    // ... and from this long, say so

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QString shortMode(const QString &m) { return m == "D-Star" ? QString("DSTR") : m; }

QString clock(qint64 ms)   // "12s", "3:05"
{
    const qint64 s = ms / 1000;
    return s < 60 ? QString("%1s").arg(s) : QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

bool hasModule(const QString &mode) { return mode == "D-Star" || mode == "M17"; }

int modeIndex(FrontPanel::Key k)   // mode buttons -> Radio::kModes index, else -1
{
    const int i = int(k) - int(FrontPanel::Key::DMR);
    return i >= 0 && i < int(Radio::kModes.size()) ? i : -1;
}
}

FrontPanel::FrontPanel(Radio &radio, Display &display, QObject *parent)
    : QObject(parent), radio_(radio), display_(display)
{
    connect(&radio_, &Radio::ticked, this, &FrontPanel::refresh);
    connect(&radio_, &Radio::txRefused, this, [this](const QString &why) { message("Cannot transmit", why, 3000); });
    connect(&radio_, &Radio::page, this, [this](uint32_t ric, const QString &text) {
        pageUnread_ = true;
        message(QString("Page %1").arg(ric), text, 15000);
    });
    display_.setColour(Display::White);
    display_.show({"Digital TRX", "Starting..."});
}

// "left" and "right" on one line: the right part is kept whole if it fits beside at least the
// first few characters of the left one, and cut otherwise
QString FrontPanel::twoParts(const QString &left, const QString &right_) const
{
    const int width = display_.columns();
    const int keepLeft = std::min(int(left.size()), 5);
    const QString right = right_.left(std::max(0, width - keepLeft - 1));
    const int room = std::max(0, width - int(right.size()) - (right.isEmpty() ? 0 : 1));
    return left.left(room).leftJustified(room, ' ') + (right.isEmpty() ? "" : " " + right);
}

int FrontPanel::modeColour(const QString &m) const
{
    if (!modeColours_) return Display::White;
    if (m == "DMR") return Display::Green;
    if (m == "D-Star") return Display::Blue;
    if (m == "YSF") return Display::Violet;
    if (m == "P25") return Display::Red;
    if (m == "NXDN") return Display::Cyan;
    if (m == "M17") return Display::Yellow;
    return Display::White;
}

void FrontPanel::message(const QString &line1, const QString &line2, int ms)
{
    msg_[0] = line1;
    msg_[1] = line2;
    msgUntil_ = nowMs() + ms;
    refresh();
}

QString FrontPanel::destination(const QString &mode) const
{
    if (mode == "DMR" && radio_.linked("DMR")) {
        const auto &t = radio_.settings().transmit;
        return QString("%1 %2").arg(t.dmrPrivate ? "Pvt" : "TG").arg(t.dmrTalkgroup);
    }
    const QString e = radio_.endpoint(mode);
    return e.isEmpty() && !mode.isEmpty() ? QString("not linked") : e;
}

// ---------------------------------------------------------------- controls
void FrontPanel::togglePtt()
{
    if (shuttingDown_) return;
    pageUnread_ = false;
    const bool on = !radio_.txActive();
    radio_.logEvent("INFO", QString("PTT %1: headset call button").arg(on ? "on" : "off"));
    if (on) radio_.startTx(); else radio_.stopTx();
}

void FrontPanel::press(Key k)
{
    if (shuttingDown_) return;
    const qint64 now = nowMs();
    pageUnread_ = false;   // any control acknowledges a page
    if (const int m = modeIndex(k); m >= 0) {
        modeHeld_ = m;
        modeHeldSince_ = now;
        modeHoldDone_ = false;
        return;
    }
    switch (k) {
    case Key::Ptt:
        radio_.logEvent("INFO", "PTT on: PTT button");
        radio_.startTx();
        return;
    case Key::Mute:
        radio_.setMuted(!radio_.muted());
        message("Volume", radio_.muted() ? QString("muted") : QString("%1%").arg(radio_.volume()), 1500);
        return;
    case Key::Link:
        linkHeld_ = true;
        linkTurned_ = false;
        return;
    case Key::Menu:   // acts on release (tap = back), or when held long enough (shut down)
        menuHeld_ = true;
        menuHeldSince_ = now;
        return;
    case Key::Select:
        if (browsing()) {
            choose();
            selectChose_ = true;
            return;
        }
        radio_.logEvent("INFO", "PTT on: Select pressed");
        radio_.startTx();
        return;
    default:
        break;
    }
    // Plate Up / Down / Left / Right
    held_ = int(k);
    heldSince_ = now;
    repeatAt_ = now + 500;
    if (radio_.txActive()) return;
    if (k == Key::Up || k == Key::Down) {
        const int dir = k == Key::Down ? 1 : -1;
        if (browsing() && hasModule(browseMode_)) stepModule(-dir);   // Up = next letter
        else stepTxMode(dir);
    } else {
        browse(k == Key::Right ? 1 : -1);
    }
    refresh();
}

void FrontPanel::release(Key k)
{
    if (shuttingDown_) return;
    if (k == Key::Menu) {
        if (menuHeld_ && nowMs() - menuHeldSince_ < kShutdownHintMs) browse_.clear();   // tap: back
        menuHeld_ = false;
        msgUntil_ = 0;   // also drops the shutdown countdown
        refresh();
        return;
    }
    if (const int m = modeIndex(k); m >= 0) {
        if (m == modeHeld_ && !modeHoldDone_) modeTap(Radio::kModes[m]);
        if (m == modeHeld_) modeHeld_ = -1;
        return;
    }
    if (int(k) == held_) held_ = -1;
    switch (k) {
    case Key::Ptt:
        if (radio_.txActive()) radio_.logEvent("INFO", "PTT off: PTT button released");
        radio_.stopTx();
        break;
    case Key::Link:   // the channel knob acts on release, so that push-and-turn can change the module
        if (linkHeld_ && !linkTurned_) {
            if (browsing()) choose();
            else if (!radio_.txActive()) message("TX " + shortMode(radio_.txMode()), destination(radio_.txMode()), 2000);
        }
        linkHeld_ = false;
        break;
    case Key::Select:
        if (selectChose_) { selectChose_ = false; break; }
        if (radio_.txActive()) radio_.logEvent("INFO", "PTT off: Select released");
        radio_.stopTx();
        break;
    default:
        break;
    }
}

void FrontPanel::turn(Knob k, int steps)
{
    if (steps == 0 || shuttingDown_) return;
    pageUnread_ = false;
    if (k == Knob::Volume) {
        radio_.setVolume(radio_.volume() + 5 * steps);
        message("Volume", radio_.muted() ? QString("muted") : QString("%1%").arg(radio_.volume()), 1500);
        return;
    }
    if (radio_.txActive()) return;
    if (linkHeld_) {   // pushed and turned: module letter
        linkTurned_ = true;
        if (!browsing()) browse(0);
        if (browsing() && hasModule(browseMode_)) stepModule(steps);
        refresh();
        return;
    }
    // Spinning the knob quickly moves faster through long lists (D-Star has thousands).
    const qint64 gap = lastTurn_.isValid() ? lastTurn_.restart() : 1000;
    if (!lastTurn_.isValid()) lastTurn_.start();
    if (browse_.size() > 50) steps *= gap < 25 ? 10 : gap < 60 ? 3 : 1;
    browse(steps);
    refresh();
}

void FrontPanel::modeTap(const QString &mode)
{
    if (!radio_.modeEnabled(mode)) { message(shortMode(mode), "not enabled", 2000); return; }
    const bool on = !radio_.rxOn(mode);
    radio_.setRx(mode, on);
    message("RX " + shortMode(mode), on ? "on" : "off", 1500);
}

void FrontPanel::modeHold(const QString &mode)
{
    if (!radio_.modeEnabled(mode)) { message(shortMode(mode), "not enabled", 2000); return; }
    if (radio_.txActive()) return;
    browse_.clear();
    radio_.setTxMode(mode);
    message("TX " + shortMode(mode), destination(mode), 2000);
}

void FrontPanel::stepTxMode(int dir)
{
    browse_.clear();
    const QStringList modes = radio_.txModes();
    const int n = int(modes.size());
    if (n == 0) return;
    const int i = std::max(0, int(modes.indexOf(radio_.txMode())));
    radio_.setTxMode(modes[((i + dir) % n + n) % n]);
}

void FrontPanel::stepModule(int dir)
{
    auto &[target, label] = browse_[browseIdx_];
    const QString cur = target.section(' ', 1, 1);
    const int m = cur.isEmpty() ? 0 : cur.at(0).toLatin1() - 'A';
    const QString module(QChar(char('A' + ((m + dir) % 26 + 26) % 26)));
    for (auto &[t, l] : browse_) {   // every entry follows the new module
        t = t.section(' ', 0, 0) + " " + module;
        l = t;
    }
    browseUntil_ = nowMs() + kBrowseMs;
}

// The TX mode's destinations, with browseIdx_ at the current one.
void FrontPanel::buildBrowseList(const QString &mode)
{
    browse_.clear();
    browseIdx_ = 0;
    browseMode_ = mode;
    if (mode == "DMR") {   // talkgroups from settings.json; target "<id>" or "P<id>" (private)
        const Settings &s = radio_.settings();
        const uint32_t parrot = s.testCall.dmrEchoId;
        for (const QString &e : s.transmit.dmrTalkgroups) {
            bool ok = false;
            const uint32_t id = e.section(' ', 0, 0).toUInt(&ok);
            if (!ok || !id) continue;
            const QString name = e.section(' ', 1).trimmed();
            if (id == parrot) browse_.append({QString("P%1").arg(id), name.isEmpty() ? QString("Parrot") : name});
            else browse_.append({QString::number(id), QString("TG %1 %2").arg(id).arg(name).trimmed()});
        }
        const QString cur = (s.transmit.dmrPrivate ? "P" : "") + QString::number(s.transmit.dmrTalkgroup);
        auto it = std::find_if(browse_.begin(), browse_.end(), [&cur](const auto &e) { return e.first == cur; });
        if (it != browse_.end()) browseIdx_ = int(it - browse_.begin());
        else browse_.prepend({cur, QString("%1 %2").arg(s.transmit.dmrPrivate ? "Private" : "TG").arg(s.transmit.dmrTalkgroup)});
        return;
    }
    const QString current = radio_.currentLink(mode);
    QString module;
    if (hasModule(mode)) {
        module = current.section(' ', 1, 1);
        if (module.isEmpty()) module = radio_.settings().network.dstarModule.left(1);
    }
    for (const HostEntry &e : radio_.hostList(mode)) {
        const QString target = module.isEmpty() ? e.id : e.id + " " + module;
        const QString label = mode == "YSF" ? e.name.section("  ", 0, 0)
                              : mode == "P25" || mode == "NXDN" ? QString("TG %1 %2").arg(e.id, e.name) : target;
        if (current.startsWith(module.isEmpty() ? e.id : target, Qt::CaseInsensitive) ||
            (mode == "YSF" && current.compare(label, Qt::CaseInsensitive) == 0) || current == "TG " + e.id)
            browseIdx_ = int(browse_.size());
        browse_.append({target, label});
    }
}

// Step through the TX mode's destinations, starting at the current one (the first step only
// shows it).
void FrontPanel::browse(int dir)
{
    const QString mode = radio_.txMode();
    if (mode.isEmpty()) return;
    if (!browsing() || browseMode_ != mode) {
        buildBrowseList(mode);
        if (!browsing()) { message(shortMode(mode), "No list yet", 2000); return; }
    } else {
        const int n = int(browse_.size());
        browseIdx_ = ((browseIdx_ + dir) % n + n) % n;
    }
    browseUntil_ = nowMs() + kBrowseMs;
}

void FrontPanel::choose()
{
    const auto [target, label] = browse_[browseIdx_];
    const QString mode = browseMode_;
    browse_.clear();
    if (mode == "DMR") {
        const bool priv = target.startsWith('P');
        radio_.setDmrTarget(target.mid(priv ? 1 : 0).toUInt(), priv);
        message("DMR TX to", label, 2000);
    } else {
        radio_.link(mode, target);
        message("Linking", label, 2500);
    }
}

// ---------------------------------------------------------------- display and LEDs
void FrontPanel::refresh()
{
    const qint64 now = nowMs();
    // Plate Left / Right held: keep stepping, faster after a while
    if ((held_ == int(Key::Left) || held_ == int(Key::Right)) && now >= repeatAt_ && !radio_.txActive()) {
        browse(held_ == int(Key::Right) ? 1 : -1);
        repeatAt_ = now + (now - heldSince_ > 2500 ? 40 : 150);
    }
    // Menu held: count down to shutting down
    if (menuHeld_ && !shuttingDown_) {
        const qint64 held = now - menuHeldSince_;
        if (held >= kShutdownHoldMs) {
            shuttingDown_ = true;
            menuHeld_ = false;
            browse_.clear();
            msg_[0] = "Shutting down";
            msg_[1] = "Please wait...";
            msgUntil_ = std::numeric_limits<qint64>::max();
            radio_.logEvent("INFO", "Shutdown: Menu held");
            emit shutdownRequested();
        } else if (held >= kShutdownHintMs) {
            msg_[0] = "Hold: shut down";
            msg_[1] = QString("%1...").arg((kShutdownHoldMs - held + 999) / 1000);
            msgUntil_ = now + 500;
        }
    }
    // Mode button held long enough: TX mode (while still held, so it's felt at once)
    if (modeHeld_ >= 0 && !modeHoldDone_ && now - modeHeldSince_ >= kModeHoldMs) {
        modeHoldDone_ = true;
        modeHold(Radio::kModes[modeHeld_]);
    }
    if (browsing() && now > browseUntil_) browse_.clear();

    // Remember the last call heard, for the idle screen
    const OnAir &call = radio_.onAir();
    if (!call.mode.isEmpty() && !call.tx) {
        lastHeard_ = call.call;
        lastHeardName_ = call.name;
        lastHeardTime_ = QTime::currentTime().toString("HH:mm");
    }

    const QString txm = radio_.txMode();
    display_.setColour(radio_.txActive() ? Display::White
                       : now < msgUntil_ || (!browsing() && call.mode.isEmpty()) ? modeColour(txm)
                       : browsing() ? modeColour(browseMode_)
                                    : modeColour(call.mode));
    display_.show(display_.rows() >= 4 ? screen4(now) : screen2(now));
    updateLeds();
}

// 16x2: one thing at a time
QStringList FrontPanel::screen2(qint64 now) const
{
    const QString txm = radio_.txMode();
    const OnAir &call = radio_.onAir();
    if (radio_.txActive())
        return {twoParts("TX " + shortMode(txm), destination(txm)), twoParts("ON AIR", clock(radio_.txElapsedMs()))};
    if (now < msgUntil_) return {msg_[0], msg_[1]};
    if (browsing())
        return {twoParts(shortMode(browseMode_), QString("%1/%2").arg(browseIdx_ + 1).arg(browse_.size())),
                ">" + browse_[browseIdx_].second};
    if (!call.mode.isEmpty())
        return {twoParts(call.call, shortMode(call.mode)),
                twoParts(call.name.isEmpty() ? ">" + call.target : call.name, clock(call.elapsedMs))};
    // Idle: the TX mode and where it transmits, then the last call heard
    return {twoParts(shortMode(txm), destination(txm)),
            radio_.piStarOnline() ? QString("Pi-Star online")
            : lastHeard_.isEmpty() ? QString() : twoParts(lastHeard_, lastHeardTime_)};
}

// 20x4: the top line always shows the TX mode, where it transmits and the time; the bottom line
// the IP address and volume, or a problem.
//
//   DSTR REF001 E  12:41     DSTR REF001 E  12:41     DSTR REF001 E  12:41
//   M6OZE            DSTR    Last M6OZE      12:39    TRANSMITTING    0:07
//   Ozzy                     Ozzy                     >REF001 E
//   >CQCQCQ          0:07    192.168.1.75  Vol 80%    192.168.1.75  Vol 80%
QStringList FrontPanel::screen4(qint64 now)
{
    const QString txm = radio_.txMode();
    const OnAir &call = radio_.onAir();
    const QString top = twoParts(txm.isEmpty() ? QString("No modes") : shortMode(txm) + " " + destination(txm),
                                 QTime::currentTime().toString("HH:mm"));
    const QString status = statusLine();
    if (radio_.txActive())
        return {top, twoParts("TRANSMITTING", clock(radio_.txElapsedMs())), ">" + destination(txm), status};
    if (now < msgUntil_) return {top, msg_[0], msg_[1], status};
    if (browsing()) {
        const int n = int(browse_.size());
        const QString next = n > 1 ? " " + browse_[(browseIdx_ + 1) % n].second : QString();
        return {twoParts(shortMode(browseMode_) + " destination", QString("%1/%2").arg(browseIdx_ + 1).arg(n)),
                ">" + browse_[browseIdx_].second, next, "Push: go  Menu: back"};
    }
    if (!call.mode.isEmpty())
        return {top, twoParts(call.call, shortMode(call.mode)), call.name,
                twoParts(">" + call.target, clock(call.elapsedMs))};
    if (lastHeard_.isEmpty()) return {top, QString(), QString(), status};
    return {top, twoParts("Last " + lastHeard_, lastHeardTime_), lastHeardName_, status};
}

QString FrontPanel::statusLine()
{
    if (radio_.piStarOnline()) return "Pi-Star online";
    for (const QString &m : Radio::kModes) {   // the first failed link, e.g. "DMR ! login failed"
        const QString e = radio_.modeEnabled(m) ? radio_.endpoint(m) : QString();
        if (e.startsWith('!')) return shortMode(m) + " " + e;
    }
    const qint64 now = nowMs();
    if (now - ipCheckedMs_ > 5000) {   // the Wi-Fi address can change
        ipCheckedMs_ = now;
        ip_.clear();
        for (const QHostAddress &a : QNetworkInterface::allAddresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback()) { ip_ = a.toString(); break; }
    }
    return twoParts(ip_.isEmpty() ? QString("No network") : ip_,
                    radio_.muted() ? QString("Muted") : QString("Vol %1%").arg(radio_.volume()));
}

void FrontPanel::updateLeds()
{
    if (!leds_) return;
    const OnAir &call = radio_.onAir();
    const QString heard = call.tx ? QString() : call.mode;
    const QString txm = radio_.txMode();
    for (int i = 0; i < int(Radio::kModes.size()); ++i) {
        const QString &m = Radio::kModes[i];
        int c = Display::Off;
        if (!radio_.modeEnabled(m)) c = Display::Off;
        else if (m == heard) c = Display::Blue;
        else c = (m == txm ? Display::Red : 0) | (radio_.rxOn(m) ? Display::Green : 0);   // both = yellow
        leds_->setLed(i, c);
    }
    leds_->setLed(Indicators::Status, radio_.txActive() ? Display::Red
                                      : !heard.isEmpty() ? Display::Green
                                      : radio_.networkFault() || pageUnread_ ? Display::Blue
                                                                             : Display::Off);
    leds_->commit();
}
