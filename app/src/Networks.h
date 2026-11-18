#pragma once
#include <QSet>
// Direct network clients: the PC itself connects to BrandMeister and to D-Star,
// YSF, P25, NXDN and M17 reflectors, and to DAPNET. Each client turns what it
// receives into the app's internal records (the same formats the decoders use:
// a DMRD packet, "DSTRN"+DSRP, "YSFXN"+YSFD, "P25XN"+record, "NXDXN"+Icom packet,
// "M17XN"+frame, "PAGE"+log line) and turns outgoing records into its protocol.
#include <QDateTime>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <functional>
#include <memory>

#include "Settings.h"

struct HostEntry {
    QString id;      // e.g. "00007", "31672", "M17-672", "XRF001", "DCS011", "XLX001"
    QString name;    // description shown in the picker
    QString host;
    quint16 port = 0;
};

// Host lists and ID databases from pistar.uk, cached in the data folder and
// refreshed once a day.
class HostFiles : public QObject {
    Q_OBJECT
public:
    explicit HostFiles(const QString &dir, QObject *parent = nullptr);
    void refresh(bool force = false);
    QList<HostEntry> list(const QString &mode) const;
    bool find(const QString &mode, const QString &id, HostEntry &out) const;
    QString dmrIdsPath() const { return dir_ + "/DMRIds.dat"; }
    QString nxdnIdsPath() const { return dir_ + "/NXDN.csv"; }
signals:
    void updated(const QString &file);   // a file was (re)downloaded
private:
    QSet<QString> retrying_;   // files whose download failed, retried in a minute
    void fetch(const QString &file);
    QString dir_;
    QNetworkAccessManager net_;
    mutable QHash<QString, QList<HostEntry>> cache_;
};

class NetClient : public QObject {
    Q_OBJECT
public:
    enum class State { Off, Connecting, Linked, Failed };
    NetClient(const QString &mode, Settings &cfg, HostFiles &hosts, QObject *parent = nullptr);
    QString mode() const { return mode_; }
    State state() const { return state_; }
    QString endpoint() const { return state_ == State::Linked ? target_ : QString(); }   // "" = not linked
    QString target() const { return target_; }
    QString detail() const { return detail_; }

    virtual void link(const QString &target) = 0;   // reflector / talkgroup, as shown in the picker
    virtual void unlink() = 0;
    virtual void transmit(const QByteArray &record) = 0;   // an internal record (as the encoders make)
    virtual bool canLink() const { return true; }

signals:
    void record(const QByteArray &rec);
    void changed();
    void message(const QString &text, bool error);

protected:
    void setState(State s, const QString &detail = {});
    // Resolve host and point the socket at it; calls then() on success.
    void open(const QString &host, quint16 port, std::function<void()> then);
    void send(const QByteArray &data);
    void stopTimers();
    QString callsign10() const;                      // callsign padded to 10
    virtual void received(const QByteArray &data) = 0;

    QString mode_;
    Settings &cfg_;
    HostFiles &hosts_;
    QUdpSocket sock_;
    QHostAddress addr_;
    quint16 port_ = 0;
    QTimer poll_, watchdog_;
    State state_ = State::Off;
    QString target_, detail_;
    int lookup_ = -1;
};

std::unique_ptr<NetClient> makeClient(const QString &mode, Settings &cfg, HostFiles &hosts);

// DAPNET: logs in as a (registered) transmitter and turns messages into PAGE records.
class DapnetClient : public QObject {
    Q_OBJECT
public:
    DapnetClient(Settings &cfg, QObject *parent = nullptr);
    void start();
    void stop();
    bool loggedIn() const { return loggedIn_; }
    QString lastError() const { return lastError_; }
signals:
    void record(const QByteArray &rec);
    void changed();
    void message(const QString &text, bool error);
private:
    void line(const QByteArray &l);
    Settings &cfg_;
    QTcpSocket sock_;
    QByteArray buf_;
    bool loggedIn_ = false;
    QTimer retry_;
    int failures_ = 0;      // consecutive failed attempts, for back-off
    int refusals_ = 0;      // logins refused in a row (stop at 2)
    QString lastError_;
    int scheduleRetry();   // minutes until the next attempt (0 = none scheduled)
};
