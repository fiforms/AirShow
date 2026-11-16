#include "settings/RememberedDevices.h"
#include "settings/AppSettings.h"

#include <QTextStream>

namespace airshow {

namespace {
const QString kList   = QStringLiteral("--list-remembered");
const QString kForget = QStringLiteral("--forget-remembered");

// "Daniel McFeeters (Miracast)", or empty if nothing was stored
QString describe(const AppSettings& settings, const QString& id) {
    const QString name     = settings.trustedDeviceName(id);
    const QString protocol = settings.trustedDeviceProtocol(id);
    if (name.isEmpty()) return protocol;
    return protocol.isEmpty() ? name : name + QStringLiteral(" (") + protocol + QLatin1Char(')');
}

int list(const AppSettings& settings, QTextStream& out) {
    const QStringList ids = settings.trustedDevices();
    if (ids.isEmpty()) {
        out << "No remembered devices.\n";
        return 0;
    }
    int width = 0;
    for (const QString& id : ids) width = qMax(width, int(id.size()));
    for (const QString& id : ids) {
        const QString what = describe(settings, id);
        out << id;
        if (!what.isEmpty()) out << QString(width - id.size() + 2, QLatin1Char(' ')) << what;
        out << '\n';
    }
    return 0;
}

int forget(AppSettings& settings, const QString& target, QTextStream& out, QTextStream& err) {
    const QStringList ids = settings.trustedDevices();
    if (target == QLatin1String("all")) {
        settings.clearTrustedDevices();
        out << "Forgot " << ids.size() << (ids.size() == 1 ? " device.\n" : " devices.\n");
        return 0;
    }
    // An exact identifier, else every device with that name
    QStringList matches;
    if (ids.contains(target)) {
        matches << target;
    } else {
        for (const QString& id : ids) {
            if (settings.trustedDeviceName(id).compare(target, Qt::CaseInsensitive) == 0)
                matches << id;
        }
    }
    if (matches.isEmpty()) {
        err << "No remembered device '" << target << "'. See airshow --list-remembered.\n";
        return 1;
    }
    for (const QString& id : matches) {
        const QString what = describe(settings, id);
        settings.removeTrustedDevice(id);
        out << "Forgot ";
        if (!what.isEmpty()) out << what << ": ";
        out << id << '\n';
    }
    return 0;
}
} // namespace

bool isRememberedDevicesCommand(const QStringList& args) {
    for (const QString& a : args) {
        if (a == kList || a == kForget || a.startsWith(kForget + QLatin1Char('=')))
            return true;
    }
    return false;
}

int runRememberedDevicesCommand(const QStringList& args, AppSettings& settings,
                                QTextStream& out, QTextStream& err) {
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        if (a == kList) return list(settings, out);
        if (a.startsWith(kForget + QLatin1Char('='))) {
            return forget(settings, a.mid(kForget.size() + 1), out, err);
        }
        if (a == kForget) {
            if (i + 1 >= args.size() || args[i + 1].isEmpty()) {
                err << "Usage: airshow --forget-remembered <identifier | device name | all>\n";
                return 2;
            }
            return forget(settings, args[i + 1], out, err);
        }
    }
    err << "Usage: airshow --list-remembered | --forget-remembered <identifier | "
           "device name | all>\n";
    return 2;
}

} // namespace airshow
