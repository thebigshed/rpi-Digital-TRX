#include "SimPanel.h"

#include <QSocketNotifier>
#include <QStringList>
#include <algorithm>
#include <cstdio>
#include <unistd.h>

SimPanel::SimPanel(int columns, int rows, QObject *parent) : QObject(parent), cols_(columns), rows_(rows)
{
    stdin_ = new QSocketNotifier(0, QSocketNotifier::Read, this);
    connect(stdin_, &QSocketNotifier::activated, this, [this] {
        // Read straight from the descriptor (stdio would buffer lines the notifier never reports).
        char buf[256];
        const ssize_t n = ::read(0, buf, sizeof buf);
        if (n <= 0) { stdin_->setEnabled(false); return; }
        input_ += QByteArray(buf, int(n));
        int nl;
        while ((nl = input_.indexOf('\n')) >= 0) {
            line(QString::fromLatin1(input_.left(nl)).trimmed().toLower());
            input_.remove(0, nl + 1);
        }
    });
}

void SimPanel::line(const QString &l)
{
    if (!panel_ || l.isEmpty()) return;
    using Key = FrontPanel::Key;
    const QChar c = l[0];
    const bool down = l.size() < 2 || l[1] == '+';
    if (c == 'h') { panel_->togglePtt(); return; }
    if (c == 'm') { panel_->press(Key::Mute); panel_->release(Key::Mute); return; }
    if (c == 't' || c == 'v') { panel_->turn(c == 't' ? FrontPanel::Knob::Tune : FrontPanel::Knob::Volume, down ? 1 : -1); return; }
    static const QString keys = "sudlrpkn123456";
    static const Key map[] = {Key::Select, Key::Up, Key::Down, Key::Left, Key::Right, Key::Ptt, Key::Link, Key::Menu,
                              Key::DMR, Key::DStar, Key::YSF, Key::P25, Key::NXDN, Key::M17};
    const int i = int(keys.indexOf(c));
    if (i < 0) return;
    if (down) panel_->press(map[i]); else panel_->release(map[i]);
}

static const char *colourName(int c)
{
    static const char *names[] = {"off", "blue", "green", "cyan", "red", "violet", "yellow", "white"};
    return c >= 0 ? names[c & 7] : "?";
}

void SimPanel::commit()
{
    if (std::equal(leds_, leds_ + Count, shownLeds_)) return;
    std::copy(leds_, leds_ + Count, shownLeds_);
    static const char *names[] = {"DMR", "D-Star", "YSF", "P25", "NXDN", "M17", "status"};
    QStringList parts;
    for (int i = 0; i < Count; ++i) parts << QString("%1=%2").arg(names[i], colourName(leds_[i]));
    fprintf(stdout, "LED %s\n", qPrintable(parts.join(' ')));
    fflush(stdout);
}

void SimPanel::show(const QStringList &text)
{
    QStringList lines;
    for (int r = 0; r < rows_; ++r) lines << fit(text.value(r), cols_);
    if (lines == shown_) return;
    shown_ = lines;
    fprintf(stdout, "LCD [%s] |%s|\n", colourName(colour_), qPrintable(lines.join("| |")));
    fflush(stdout);
}

void SimPanel::setColour(int rgb)
{
    if (rgb == colour_) return;
    colour_ = rgb;
    shown_.clear();   // reprinted with the colour by the next show()
}
