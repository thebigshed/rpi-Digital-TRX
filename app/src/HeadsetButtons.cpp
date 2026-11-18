#include "HeadsetButtons.h"
#include <QDateTime>
#include <QDir>
#include <QSocketNotifier>
#include <QTime>
#include <cstdio>

#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

HeadsetButtons::HeadsetButtons(QObject *parent) : QObject(parent)
{
    rescan_.setInterval(30000);
    connect(&rescan_, &QTimer::timeout, this, &HeadsetButtons::rescan);
    rescan_.start();
    rescan();
}

HeadsetButtons::~HeadsetButtons()
{
#ifdef __linux__
    for (const Dev &d : devs_) ::close(d.fd);
#endif
}

QStringList HeadsetButtons::devices() const
{
    QStringList out;
    for (auto it = devs_.begin(); it != devs_.end(); ++it) out << QString("%1 (%2)").arg(it.value().name, it.key());
    return out;
}

void HeadsetButtons::rescan()
{
#ifdef __linux__
    bool changed = false;
    // Drop devices that have gone (headset unplugged)
    for (auto it = devs_.begin(); it != devs_.end();) {
        if (!QFile::exists(it.key())) {
            delete it.value().n;
            ::close(it.value().fd);
            it = devs_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    for (const QString &e : QDir("/dev/input").entryList({"event*"}, QDir::System)) {
        const QString path = "/dev/input/" + e;
        if (devs_.contains(path)) continue;
        const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;   // no permission (not root / not in the input group)
        // Only devices that have BTN_0: the headset call button, not keyboards or mice
        unsigned long keys[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1] = {};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 ||
            !(keys[BTN_0 / (8 * sizeof(unsigned long))] & (1UL << (BTN_0 % (8 * sizeof(unsigned long)))))) {
            ::close(fd);
            continue;
        }
        char name[128] = {};
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        Dev d;
        d.fd = fd;
        d.name = QString::fromLocal8Bit(name);
        d.n = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(d.n, &QSocketNotifier::activated, this, [this, fd] {
            input_event ev[16];
            const ssize_t n = ::read(fd, ev, sizeof ev);
            if (n <= 0) return;
            for (size_t i = 0; i < size_t(n) / sizeof(input_event); ++i) {
                if (ev[i].type != EV_KEY || ev[i].code != BTN_0 || ev[i].value != 1) continue;
                // One click can arrive on several of the headset's interfaces: count it once.
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - lastClickMs_ < 300) continue;
                lastClickMs_ = now;
                if (qEnvironmentVariableIsSet("DT_PTT_TIMING")) {   // when the kernel saw the click
                    const qint64 evMs = qint64(ev[i].input_event_sec) * 1000 + ev[i].input_event_usec / 1000;
                    fprintf(stderr, "%s headset click (event %lld ms ago)\n",
                            qPrintable(QTime::currentTime().toString("HH:mm:ss.zzz")), now - evMs);
                    fflush(stderr);
                }
                emit clicked();
            }
        });
        devs_.insert(path, d);
        changed = true;
    }
    if (changed) emit devicesChanged();
#endif
}
