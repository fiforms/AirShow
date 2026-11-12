#include "ui/SessionHooks.h"
#include "ui/ConnectionBridge.h"

#include <QProcess>
#include <QProcessEnvironment>

namespace airshow {

SessionHooks::SessionHooks(ConnectionBridge* bridge,
                           QString startCommand,
                           QString endCommand,
                           int endDelayMs,
                           QObject* parent)
    : QObject(parent)
    , m_bridge(bridge)
    , m_startCommand(std::move(startCommand))
    , m_endCommand(std::move(endCommand))
{
    m_endTimer.setSingleShot(true);
    m_endTimer.setInterval(endDelayMs);
    connect(&m_endTimer, &QTimer::timeout, this, &SessionHooks::finishSession);

    connect(m_bridge, &ConnectionBridge::connectedChanged, this, &SessionHooks::update);
    connect(m_bridge, &ConnectionBridge::approvalPendingChanged, this, &SessionHooks::update);
    update();
}

SessionHooks::~SessionHooks() {
    if (m_active) {
        m_active = false;
        run(m_endCommand, "end");
    }
}

void SessionHooks::update() {
    const bool active = m_bridge->isConnected() || m_bridge->isApprovalPending();
    if (active) {
        m_endTimer.stop();  // a reconnect within the grace period continues the session
        if (m_bridge->isConnected()) {
            m_deviceName = m_bridge->deviceName();
            m_protocol   = m_bridge->protocol();
        } else if (!m_active) {
            m_deviceName = m_bridge->pendingDeviceName();
            m_protocol   = m_bridge->pendingProtocol();
        }
        if (!m_active) {
            m_active = true;
            emit sessionStarted(m_deviceName, m_protocol);
            run(m_startCommand, "start");
        }
    } else if (m_active && !m_endTimer.isActive()) {
        m_endTimer.start();
    }
}

void SessionHooks::finishSession() {
    if (!m_active || m_bridge->isConnected() || m_bridge->isApprovalPending()) return;
    m_active = false;
    emit sessionEnded(m_deviceName, m_protocol);
    run(m_endCommand, "end");
}

void SessionHooks::run(const QString& command, const char* phase) {
    if (command.isEmpty()) return;

    QProcess proc;
#ifdef _WIN32
    proc.setProgram(QStringLiteral("cmd.exe"));
    proc.setArguments({QStringLiteral("/c"), command});
#else
    proc.setProgram(QStringLiteral("/bin/sh"));
    proc.setArguments({QStringLiteral("-c"), command});
#endif
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("AIRSHOW_SESSION"), QString::fromLatin1(phase));
    env.insert(QStringLiteral("AIRSHOW_DEVICE"), m_deviceName);
    env.insert(QStringLiteral("AIRSHOW_PROTOCOL"), m_protocol);
    proc.setProcessEnvironment(env);

    qInfo("Session %s hook: %s", phase, qPrintable(command));
    if (!proc.startDetached())
        qWarning("Session %s hook failed to start: %s", phase, qPrintable(proc.errorString()));
}

} // namespace airshow
