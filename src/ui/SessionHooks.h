#pragma once
#include <QObject>
#include <QString>
#include <QTimer>

namespace airshow {

class ConnectionBridge;

// Runs user-supplied shell commands when a mirroring session starts and ends
// (--on-session-start / --on-session-end), e.g. to hide a kiosk browser that
// sits on top of AirShow while someone is casting.
//
// A session is "active" while a sender is connected or an approval dialog is
// waiting for an answer (the dialog must be visible, so it counts too).
// The end command is held back for endDelayMs so that brief disconnects —
// AirPlay reconnecting, DLNA switching tracks — don't flap the hooks.
//
// Commands run detached through /bin/sh -c (cmd.exe /c on Windows) with
// AIRSHOW_SESSION=start|end, AIRSHOW_DEVICE and AIRSHOW_PROTOCOL set.
class SessionHooks : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultEndDelayMs = 5000;

    SessionHooks(ConnectionBridge* bridge,
                 QString startCommand,
                 QString endCommand,
                 int endDelayMs = kDefaultEndDelayMs,
                 QObject* parent = nullptr);
    // Runs the end command if a session is still active, so whatever the start
    // command hid comes back when AirShow quits.
    ~SessionHooks() override;

    bool isSessionActive() const { return m_active; }

signals:
    void sessionStarted(const QString& deviceName, const QString& protocol);
    void sessionEnded(const QString& deviceName, const QString& protocol);

private:
    void update();
    void finishSession();
    void run(const QString& command, const char* phase);

    ConnectionBridge* m_bridge;
    QString m_startCommand;
    QString m_endCommand;
    QTimer  m_endTimer;
    bool    m_active = false;
    QString m_deviceName;
    QString m_protocol;
};

} // namespace airshow
