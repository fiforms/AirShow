#include "settings/AppSettings.h"
#include <QSettings>
#include <QSysInfo>
#include <QUrl>

namespace airshow {

AppSettings::AppSettings() = default;

// ---------------------------------------------------------------------------
// Receiver identity
// ---------------------------------------------------------------------------

QString AppSettings::receiverName() const {
    QSettings s;
    return s.value(QStringLiteral("receiver/name"), defaultReceiverName()).toString();
}

void AppSettings::setReceiverName(const QString& name) {
    QSettings s;
    s.setValue(QStringLiteral("receiver/name"), name);
    s.sync();
}

bool AppSettings::isFirstLaunch() const {
    QSettings s;
    return !s.value(QStringLiteral("app/firstLaunchComplete"), false).toBool();
}

void AppSettings::setFirstLaunchComplete() {
    QSettings s;
    s.setValue(QStringLiteral("app/firstLaunchComplete"), true);
    s.sync();
}

QString AppSettings::defaultReceiverName() {
    QString host = QSysInfo::machineHostName();
    return host.isEmpty() ? QStringLiteral("AirShow") : host;
}

// ---------------------------------------------------------------------------
// Miracast
// ---------------------------------------------------------------------------

bool AppSettings::wifiDirectBeacon() const {
    QSettings s;
    return s.value(QStringLiteral("miracast/wifiDirectBeacon"), true).toBool();
}

void AppSettings::setWifiDirectBeacon(bool v) {
    QSettings s;
    s.setValue(QStringLiteral("miracast/wifiDirectBeacon"), v);
    s.sync();
}

QString AppSettings::miracastMaxResolution() const {
    QSettings s;
    return s.value(QStringLiteral("miracast/maxResolution"), QStringLiteral("auto"))
        .toString().trimmed().toLower();
}

// ---------------------------------------------------------------------------
// Security settings (Phase 7 / D-11)
// ---------------------------------------------------------------------------

bool AppSettings::requireApproval() const {
    QSettings s;
    return s.value(QStringLiteral("security/requireApproval"), true).toBool();
}

void AppSettings::setRequireApproval(bool v) {
    QSettings s;
    s.setValue(QStringLiteral("security/requireApproval"), v);
    s.sync();
}

bool AppSettings::pinEnabled() const {
    QSettings s;
    return s.value(QStringLiteral("security/pinEnabled"), false).toBool();
}

void AppSettings::setPinEnabled(bool v) {
    QSettings s;
    s.setValue(QStringLiteral("security/pinEnabled"), v);
    s.sync();
}

QString AppSettings::pin() const {
    QSettings s;
    return s.value(QStringLiteral("security/pin"), QString()).toString();
}

void AppSettings::setPin(const QString& v) {
    QSettings s;
    const bool changed = s.value(QStringLiteral("security/pin"), QString()).toString() != v;
    s.setValue(QStringLiteral("security/pin"), v);
    if (changed) {
        s.remove(QStringLiteral("security/pinPairedClients"));
    }
    s.sync();
}

QStringList AppSettings::pinPairedClients() const {
    QSettings s;
    return s.value(QStringLiteral("security/pinPairedClients"), QStringList()).toStringList();
}

void AppSettings::addPinPairedClient(const QString& pk) {
    QSettings s;
    QStringList list = s.value(QStringLiteral("security/pinPairedClients"), QStringList()).toStringList();
    if (!list.contains(pk)) {
        list.append(pk);
        s.setValue(QStringLiteral("security/pinPairedClients"), list);
        s.sync();
    }
}

void AppSettings::clearPinPairedClients() {
    QSettings s;
    s.remove(QStringLiteral("security/pinPairedClients"));
    s.sync();
}

QStringList AppSettings::trustedDevices() const {
    QSettings s;
    return s.value(QStringLiteral("security/trustedDevices"), QStringList()).toStringList();
}

namespace {
// Identifiers contain ':' and may contain '/', which QSettings treats as a
// group separator: store names under a percent-encoded key
QString trustedNameKey(const QString& deviceId) {
    return QStringLiteral("security/trustedDeviceNames/") +
           QString::fromLatin1(QUrl::toPercentEncoding(deviceId));
}
} // namespace

void AppSettings::addTrustedDevice(const QString& deviceId, const QString& deviceName,
                                   const QString& protocol) {
    QSettings s;
    QStringList list = s.value(QStringLiteral("security/trustedDevices"), QStringList()).toStringList();
    if (!list.contains(deviceId)) {
        list.append(deviceId);
        s.setValue(QStringLiteral("security/trustedDevices"), list);
    }
    if (!deviceName.isEmpty() || !protocol.isEmpty()) {
        s.setValue(trustedNameKey(deviceId), QStringList{deviceName, protocol});
    }
    s.sync();
}

QString AppSettings::trustedDeviceName(const QString& deviceId) const {
    QSettings s;
    return s.value(trustedNameKey(deviceId)).toStringList().value(0);
}

QString AppSettings::trustedDeviceProtocol(const QString& deviceId) const {
    QSettings s;
    return s.value(trustedNameKey(deviceId)).toStringList().value(1);
}

void AppSettings::removeTrustedDevice(const QString& deviceId) {
    QSettings s;
    QStringList list = s.value(QStringLiteral("security/trustedDevices"), QStringList()).toStringList();
    if (list.removeAll(deviceId) > 0) {
        s.setValue(QStringLiteral("security/trustedDevices"), list);
    }
    s.remove(trustedNameKey(deviceId));
    s.sync();
}

void AppSettings::clearTrustedDevices() {
    QSettings s;
    s.setValue(QStringLiteral("security/trustedDevices"), QStringList());
    s.remove(QStringLiteral("security/trustedDeviceNames"));
    s.sync();
}

} // namespace airshow
