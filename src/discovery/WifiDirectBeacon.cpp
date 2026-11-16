#include "discovery/WifiDirectBeacon.h"

#include <QDebug>
#include <QMetaObject>
#include <QSysInfo>

#ifdef __linux__
// Qt's `signals` keyword macro collides with a struct member in GDBus headers
#pragma push_macro("signals")
#undef signals
#include <gio/gio.h>
#pragma pop_macro("signals")
#include <QObject>
#include <QSettings>
#include <QStringList>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <chrono>
#include <cstring>
#include <string>
#endif

namespace airshow {

namespace {

void appendBe16(QByteArray& out, uint16_t v) {
    out.append(static_cast<char>(v >> 8));
    out.append(static_cast<char>(v & 0xff));
}

} // namespace

// static
QByteArray WifiDirectBeacon::buildMiceVendorExtension(const QString& hostName) {
    // [MS-MICE] 2.2.8: Vendor Extension = Microsoft vendor ID + attributes, each
    // AttributeID (2 bytes) + Length (2 bytes) + value, big-endian.
    QByteArray ext = QByteArray::fromHex("000137");

    // Capability attribute (0x2001), 1 byte: bit 0 (A) MiracastOverInfrastructure
    // supported; bits 2-4 (C) Version = 1. No stream encryption / PIN support.
    appendBe16(ext, 0x2001);
    appendBe16(ext, 1);
    ext.append(static_cast<char>(0x01 | (0x1 << 2)));  // 0x05

    // Host Name attribute (0x2002): the name Windows resolves to reach TCP 7250
    const QByteArray host = hostName.toUtf8();
    appendBe16(ext, 0x2002);
    appendBe16(ext, static_cast<uint16_t>(host.size()));
    ext.append(host);
    return ext;
}

// static
QByteArray WifiDirectBeacon::buildWfdIes(uint16_t rtspPort) {
    QByteArray ies;
    // Subelement 0: WFD Device Information (6 bytes)
    ies.append('\x00');
    appendBe16(ies, 6);
    appendBe16(ies, 0x0011);    // primary sink, available for WFD session
    appendBe16(ies, rtspPort);  // session management control port
    appendBe16(ies, 300);       // max throughput, Mbps
    // Subelement 1: Associated BSSID (6 bytes, none)
    ies.append('\x01');
    appendBe16(ies, 6);
    ies.append(QByteArray(6, '\0'));
    // Subelement 6: Coupled Sink Information (7 bytes, not coupled)
    ies.append('\x06');
    appendBe16(ies, 7);
    ies.append(QByteArray(7, '\0'));
    return ies;
}

// static
uint16_t WifiDirectBeacon::parseWfdRtspPort(const QByteArray& subelements) {
    // Subelements: ID (1 byte), Length (2 bytes, big-endian), body. WFD Device
    // Information (ID 0, length 6): Device Information (2), Session Management
    // Control Port (2), WFD Device Maximum Throughput (2).
    const auto* b = reinterpret_cast<const uint8_t*>(subelements.constData());
    const qsizetype n = subelements.size();
    qsizetype pos = 0;
    while (pos + 3 <= n) {
        const uint8_t id = b[pos];
        const int len = (b[pos + 1] << 8) | b[pos + 2];
        pos += 3;
        if (pos + len > n) return 0;
        if (id == 0) {
            if (len < 6) return 0;
            return static_cast<uint16_t>((b[pos + 2] << 8) | b[pos + 3]);
        }
        pos += len;
    }
    return 0;
}

void WifiDirectBeacon::setSourceHandler(std::function<void(const WifiDirectSource&)> handler) {
    m_sourceHandler = std::move(handler);
}

void WifiDirectBeacon::deliverSource(const WifiDirectSource& source) {
    // m_refreshTimer lives on the owner's thread and dies with this object, so
    // a delivery still queued at destruction is dropped.
    QMetaObject::invokeMethod(&m_refreshTimer, [this, source] {
        if (m_sourceHandler) m_sourceHandler(source);
        else qInfo("WifiDirectBeacon: no handler for Wi-Fi Direct source '%s'",
                   qPrintable(source.name));
    }, Qt::QueuedConnection);
}

#ifdef __linux__

namespace {
constexpr const char* kService   = "fi.w1.wpa_supplicant1";
constexpr const char* kRootPath  = "/fi/w1/wpa_supplicant1";
constexpr const char* kIfaceIf   = "fi.w1.wpa_supplicant1.Interface";
constexpr const char* kP2PIf     = "fi.w1.wpa_supplicant1.Interface.P2PDevice";
constexpr const char* kWpsIf     = "fi.w1.wpa_supplicant1.Interface.WPS";
constexpr const char* kPeerIf    = "fi.w1.wpa_supplicant1.Peer";
constexpr const char* kPropsIf   = "org.freedesktop.DBus.Properties";
constexpr int         kTimeoutMs = 2000;

// NetworkManager, which must own any P2P group on the device it manages
constexpr const char* kNmService  = "org.freedesktop.NetworkManager";
constexpr const char* kNmPath     = "/org/freedesktop/NetworkManager";
constexpr const char* kNmIf       = "org.freedesktop.NetworkManager";
constexpr const char* kNmDeviceIf = "org.freedesktop.NetworkManager.Device";
constexpr int         kNmActivateTimeoutMs = 10000;

// Pairing offered to sources: push-button only (no PIN to show or enter)
constexpr const char* kConfigMethods = "virtual_push_button";
// How long a source may take from accepting its request to the group starting,
// and the group interface to get an IPv4 address after that
constexpr auto        kGroupFormationTimeout = std::chrono::seconds(60);
constexpr int         kAddressPollMs         = 250;
constexpr int         kAddressWaitMs         = 20000;
// WFD session management (RTSP) port advertised in the WFD IE. Same value as
// MiracastHandler::kRtspPort; not included here to keep Qt Network out of tests.
constexpr uint16_t    kWfdRtspPort = 7236;

// QSettings keys holding the P2P settings to restore (survive a crash/kill)
constexpr const char* kRestoreIfname = "miracast/wifiDirectRestore/ifname";
constexpr const char* kRestoreConfig = "miracast/wifiDirectRestore/config";  // a{sv}, printed
constexpr const char* kRestoreWfd    = "miracast/wifiDirectRestore/wfd";     // ay, printed
constexpr const char* kRestoreConfigMethods =
    "miracast/wifiDirectRestore/configMethods";                              // s, printed

GVariant* bytesVariant(const QByteArray& b) {
    return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, b.constData(),
                                     static_cast<gsize>(b.size()), 1);
}

GVariant* listenVariant(int periodMs, int intervalMs) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "period", g_variant_new_int32(periodMs));
    g_variant_builder_add(&b, "{sv}", "interval", g_variant_new_int32(intervalMs));
    return g_variant_new("(a{sv})", &b);
}

// P2P device names are limited to 32 octets; cut on a character boundary.
QByteArray p2pDeviceName(const QString& name) {
    QString n = name;
    while (n.toUtf8().size() > 32) n.chop(1);
    return n.toUtf8();
}

// Returns `exts` (aay, consumed) without MS-MICE vendor extensions. One found in
// the "original" settings was left by an AirShow run whose saved settings were
// lost (e.g. killed before this was persisted); restoring it would keep the
// receiver advertised after AirShow exits.
GVariant* withoutMiceExtension(GVariant* exts) {
    static const QByteArray kMicePrefix = QByteArray::fromHex("0001372001");  // MS OUI + Capability
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("aay"));
    GVariantIter it;
    g_variant_iter_init(&it, exts);
    while (GVariant* ext = g_variant_iter_next_value(&it)) {
        gsize len = 0;
        const auto* data = static_cast<const char*>(
            g_variant_get_fixed_array(ext, &len, sizeof(guchar)));
        if (QByteArray::fromRawData(data, static_cast<qsizetype>(len)).startsWith(kMicePrefix)) {
            g_message("WifiDirectBeacon: removing an MS-MICE vendor extension left by a "
                      "previous run");
        } else {
            g_variant_builder_add_value(&b, ext);
        }
        g_variant_unref(ext);
    }
    g_variant_unref(exts);
    return g_variant_ref_sink(g_variant_builder_end(&b));
}

// Human-readable reason for a failed wpa_supplicant D-Bus call.
std::string explain(GError* err) {
    if (!err) return "unknown error";
    gchar* remote = g_dbus_error_get_remote_error(err);
    std::string name = remote ? remote : "";
    g_free(remote);
    if (name == "org.freedesktop.DBus.Error.AccessDenied") {
        return "no permission to use wpa_supplicant's D-Bus API. Add your user to the "
               "'netdev' group (sudo usermod -aG netdev $USER) and log in again";
    }
    if (name == "org.freedesktop.DBus.Error.ServiceUnknown") {
        return "wpa_supplicant is not running (or not on D-Bus)";
    }
    return err->message;
}

QString macString(GVariant* ay) {
    gsize len = 0;
    const auto* b = static_cast<const uint8_t*>(g_variant_get_fixed_array(ay, &len, 1));
    QStringList parts;
    for (gsize i = 0; i < len; ++i) parts << QStringLiteral("%1").arg(b[i], 2, 16, QLatin1Char('0'));
    return parts.join(QLatin1Char(':'));
}

QByteArray byteArray(GVariant* ay) {
    gsize len = 0;
    const auto* b = static_cast<const char*>(g_variant_get_fixed_array(ay, &len, 1));
    return QByteArray(b, static_cast<qsizetype>(len));
}

// IPv4 address and netmask of interface `ifname` (host byte order), if any
bool interfaceIPv4(const std::string& ifname, uint32_t& addr, uint32_t& mask) {
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return false;
    bool found = false;
    for (ifaddrs* i = list; i && !found; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || ifname != i->ifa_name) continue;
        addr = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        mask = i->ifa_netmask
            ? ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr)
            : 0xffffff00u;
        found = true;
    }
    freeifaddrs(list);
    return found;
}

QString ipv4String(uint32_t hostOrder) {
    return QStringLiteral("%1.%2.%3.%4").arg(hostOrder >> 24).arg((hostOrder >> 16) & 0xff)
                                        .arg((hostOrder >> 8) & 0xff).arg(hostOrder & 0xff);
}
} // namespace

// Runs on WifiDirectBeacon::m_thread; only touched from there (plus construction
// and destruction while the thread is not running).
struct WifiDirectBeacon::Impl : QObject {
    explicit Impl(WifiDirectBeacon* o) : owner(o) {
        addressTimer.setParent(this);  // moves to the worker thread with this
        addressTimer.setInterval(kAddressPollMs);
        addressTimer.callOnTimeout(this, [this] { pollGroupAddress(); });
    }

    WifiDirectBeacon* owner;
    GDBusConnection*  bus = nullptr;
    std::string       ifacePath;
    std::string       ifname;
    GVariant*         savedConfig = nullptr;  // a{sv}: the keys we change
    GVariant*         savedWfd    = nullptr;  // ay
    GVariant*         savedConfigMethods = nullptr;  // s (WPS ConfigMethods)
    QString           appliedName;            // last applied (for re-applying)

    // Wi-Fi Direct sources (Android). One request / group at a time.
    guint             signalSub = 0;
    bool              accepting = false;       // between apply() and restore()
    bool              listenPaused = false;    // setListenPaused()
    std::string       pendingPeer;             // peer object we accepted
    std::chrono::steady_clock::time_point pendingSince;
    WifiDirectSource  pendingSource;
    bool              groupActive = false;
    std::string       groupIfname;
    QString           groupSourceAddr;         // from the GroupStarted signal
    QTimer            addressTimer;            // waits for the group's IPv4 address
    int               addressWaitedMs = 0;

    ~Impl() override {
        unsubscribe();
        clearSaved();
        if (bus) g_object_unref(bus);
    }

    void clearSaved() {
        if (savedConfig) g_variant_unref(savedConfig);
        if (savedWfd)    g_variant_unref(savedWfd);
        if (savedConfigMethods) g_variant_unref(savedConfigMethods);
        savedConfig = savedWfd = savedConfigMethods = nullptr;
    }

    // Returns the unwrapped property value (caller unrefs) or nullptr.
    GVariant* get(const std::string& path, const char* iface, const char* prop, GError** err) {
        GVariant* r = g_dbus_connection_call_sync(
            bus, kService, path.c_str(), kPropsIf, "Get",
            g_variant_new("(ss)", iface, prop), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, kTimeoutMs, nullptr, err);
        if (!r) return nullptr;
        GVariant* v = nullptr;
        g_variant_get(r, "(v)", &v);
        g_variant_unref(r);
        return v;
    }

    bool set(const std::string& path, const char* iface, const char* prop,
             GVariant* value, GError** err) {
        GVariant* r = g_dbus_connection_call_sync(
            bus, kService, path.c_str(), kPropsIf, "Set",
            g_variant_new("(ssv)", iface, prop, value), nullptr,
            G_DBUS_CALL_FLAGS_NONE, kTimeoutMs, nullptr, err);
        if (r) g_variant_unref(r);
        return r != nullptr;
    }

    bool call(const char* method, GVariant* params, GError** err) {
        GVariant* r = g_dbus_connection_call_sync(
            bus, kService, ifacePath.c_str(), kP2PIf, method, params, nullptr,
            G_DBUS_CALL_FLAGS_NONE, kTimeoutMs, nullptr, err);
        if (r) g_variant_unref(r);
        return r != nullptr;
    }

    bool connectBus(GError** err) {
        if (!bus) bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, err);
        return bus != nullptr;
    }

    // Pick the station interface that owns a P2P device (e.g. wlp2s0, not the
    // p2p-dev-* / p2p-*-N interfaces wpa_supplicant creates for P2P itself).
    bool findInterface(GError** err) {
        GVariant* list = get(kRootPath, kService, "Interfaces", err);
        if (!list) return false;
        ifacePath.clear();
        GVariantIter it;
        const gchar* path = nullptr;
        g_variant_iter_init(&it, list);
        while (g_variant_iter_next(&it, "&o", &path)) {
            GVariant* nameV = get(path, kIfaceIf, "Ifname", nullptr);
            if (!nameV) continue;
            const std::string name = g_variant_get_string(nameV, nullptr);
            g_variant_unref(nameV);
            if (name.rfind("p2p-", 0) == 0) continue;
            GVariant* cfg = get(path, kP2PIf, "P2PDeviceConfig", nullptr);
            if (!cfg) continue;  // no P2P support on this interface
            g_variant_unref(cfg);
            ifacePath = path;
            ifname = name;
            break;
        }
        g_variant_unref(list);
        if (ifacePath.empty()) {
            g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "no Wi-Fi interface with Wi-Fi Direct (P2P) support");
            return false;
        }
        return true;
    }

    // --- Persistence of the settings to restore ---------------------------

    static bool hasPersisted() {
        return QSettings().contains(QLatin1String(kRestoreIfname));
    }

    static void clearPersisted() {
        QSettings s;
        s.remove(QStringLiteral("miracast/wifiDirectRestore"));
        s.sync();
    }

    void persist() const {
        gchar* cfg = g_variant_print(savedConfig, TRUE);
        gchar* wfd = g_variant_print(savedWfd, TRUE);
        QSettings s;
        s.setValue(QLatin1String(kRestoreIfname), QString::fromStdString(ifname));
        s.setValue(QLatin1String(kRestoreConfig), QString::fromUtf8(cfg));
        s.setValue(QLatin1String(kRestoreWfd),    QString::fromUtf8(wfd));
        if (savedConfigMethods) {
            gchar* cm = g_variant_print(savedConfigMethods, TRUE);
            s.setValue(QLatin1String(kRestoreConfigMethods), QString::fromUtf8(cm));
            g_free(cm);
        }
        s.sync();
        g_free(cfg);
        g_free(wfd);
    }

    // Loads settings saved by a run that never reached stop(), if they belong
    // to the interface in use.
    bool loadPersisted() {
        QSettings s;
        if (s.value(QLatin1String(kRestoreIfname)).toString().toStdString() != ifname) {
            return false;
        }
        const QByteArray cfgText = s.value(QLatin1String(kRestoreConfig)).toString().toUtf8();
        const QByteArray wfdText = s.value(QLatin1String(kRestoreWfd)).toString().toUtf8();
        GVariant* cfg = g_variant_parse(G_VARIANT_TYPE("a{sv}"), cfgText.constData(),
                                        nullptr, nullptr, nullptr);
        GVariant* wfd = g_variant_parse(G_VARIANT_TYPE("ay"), wfdText.constData(),
                                        nullptr, nullptr, nullptr);
        if (!cfg || !wfd) {
            if (cfg) g_variant_unref(cfg);
            if (wfd) g_variant_unref(wfd);
            g_warning("WifiDirectBeacon: ignoring unreadable saved Wi-Fi Direct settings");
            clearPersisted();
            return false;
        }
        clearSaved();
        savedConfig = g_variant_ref_sink(cfg);
        savedWfd    = g_variant_ref_sink(wfd);
        // Absent when saved by a version that did not change ConfigMethods
        const QByteArray cmText =
            s.value(QLatin1String(kRestoreConfigMethods)).toString().toUtf8();
        if (!cmText.isEmpty()) {
            if (GVariant* cm = g_variant_parse(G_VARIANT_TYPE_STRING, cmText.constData(),
                                               nullptr, nullptr, nullptr)) {
                savedConfigMethods = g_variant_ref_sink(cm);
            }
        }
        return true;
    }

    // Saves the P2P settings apply() changes, before it changes them. Returns
    // false if they can't be read: then nothing is changed, since it could not
    // be undone.
    bool saveOriginals(GError** err) {
        if (savedConfig) return true;
        if (loadPersisted()) {
            g_message("WifiDirectBeacon: a previous run did not exit cleanly; will restore "
                      "the Wi-Fi Direct settings it saved");
            if (!savedConfigMethods) {
                // Saved by a version that never changed it: the current value is
                // still the original
                savedConfigMethods = get(ifacePath, kWpsIf, "ConfigMethods", nullptr);
                persist();
            }
            return true;
        }
        GVariant* cfg = get(ifacePath, kP2PIf, "P2PDeviceConfig", err);
        if (!cfg) return false;
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        for (const char* key : {"DeviceName", "PrimaryDeviceType", "VendorExtension"}) {
            if (GVariant* v = g_variant_lookup_value(cfg, key, nullptr)) {
                if (std::string(key) == "VendorExtension") {
                    v = withoutMiceExtension(v);
                }
                g_variant_builder_add(&b, "{sv}", key, v);
                g_variant_unref(v);
            } else if (std::string(key) == "VendorExtension") {
                // Absent means none; restoring must still remove ours
                g_variant_builder_add(&b, "{sv}", key,
                                      g_variant_new_array(G_VARIANT_TYPE("ay"), nullptr, 0));
            }
        }
        savedConfig = g_variant_ref_sink(g_variant_builder_end(&b));
        g_variant_unref(cfg);

        // Unreadable / unset WFD IEs: restore to empty so ours are removed
        GVariant* wfd = get(kRootPath, kService, "WFDIEs", nullptr);
        savedWfd = wfd ? wfd : g_variant_ref_sink(bytesVariant({}));

        // Not fatal if unreadable: then it is left alone (see apply())
        savedConfigMethods = get(ifacePath, kWpsIf, "ConfigMethods", nullptr);

        persist();
        return true;
    }

    // --- Operations (worker thread) ---------------------------------------

    bool apply(const QString& deviceName, std::string& why) {
        GError* err = nullptr;
        auto fail = [&](const char* step) {
            why = std::string(step) + ": " + explain(err);
            g_clear_error(&err);
            return false;
        };

        if (!connectBus(&err))       return fail("system D-Bus");
        if (!findInterface(&err))    return fail("wpa_supplicant");
        if (!saveOriginals(&err))    return fail("reading the current P2P settings");

        const QString host = QSysInfo::machineHostName();

        GVariantBuilder ext;
        g_variant_builder_init(&ext, G_VARIANT_TYPE("aay"));
        g_variant_builder_add_value(&ext, bytesVariant(buildMiceVendorExtension(host)));

        GVariantBuilder cfg;
        g_variant_builder_init(&cfg, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&cfg, "{sv}", "DeviceName",
                              g_variant_new_string(p2pDeviceName(deviceName).constData()));
        // WPS primary device type: category 7 (Displays), WPS OUI, subcategory 1
        g_variant_builder_add(&cfg, "{sv}", "PrimaryDeviceType",
                              bytesVariant(QByteArray::fromHex("00070050f2040001")));
        g_variant_builder_add(&cfg, "{sv}", "VendorExtension", g_variant_builder_end(&ext));

        if (!set(ifacePath, kP2PIf, "P2PDeviceConfig", g_variant_builder_end(&cfg), &err))
            return fail("setting P2P device config");
        if (!set(kRootPath, kService, "WFDIEs",
                 bytesVariant(buildWfdIes(kWfdRtspPort)), &err))
            return fail("setting Wi-Fi Display IEs");

        // Offer push-button pairing, so Android sources don't ask for a PIN.
        // Only changed when the original value could be saved for restoring.
        std::string pairing = "push-button pairing";
        if (savedConfigMethods &&
            !set(ifacePath, kWpsIf, "ConfigMethods", g_variant_new_string(kConfigMethods),
                 &err)) {
            pairing = "pairing methods unchanged (" + explain(err) + ")";
            g_clear_error(&err);
        }

        subscribe();
        appliedName = deviceName;
        accepting   = true;

        // Listen 500 ms of every 1000 ms so Windows' P2P scan gets probe
        // responses. Not while in a group or paused (a session is on screen):
        // the radio has to stay on its channel.
        if (groupActive || listenPaused) {
            call("ExtendedListen", listenVariant(0, 0), nullptr);
        } else if (!call("ExtendedListen", listenVariant(500, 1000), &err)) {
            return fail("starting P2P listen");
        }

        why = "via " + ifname + " (host " + host.toStdString() + ", " + pairing + ")";
        return true;
    }

    // --- Wi-Fi Direct sources (Android) -----------------------------------

    void subscribe() {
        if (signalSub) return;
        // Delivered on the thread-default main context of this (worker) thread;
        // handled through the event queue either way.
        signalSub = g_dbus_connection_signal_subscribe(
            bus, kService, kP2PIf, nullptr, nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            &Impl::onSignal, this, nullptr);
    }

    void unsubscribe() {
        if (signalSub && bus) g_dbus_connection_signal_unsubscribe(bus, signalSub);
        signalSub = 0;
    }

    static void onSignal(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                         const gchar* signal, GVariant* params, gpointer user) {
        auto* self = static_cast<Impl*>(user);
        const std::string name = signal;
        GVariant* p = g_variant_ref(params);
        QMetaObject::invokeMethod(self, [self, name, p] {
            self->handleSignal(name, p);
            g_variant_unref(p);
        }, Qt::QueuedConnection);
    }

    void handleSignal(const std::string& name, GVariant* params) {
        if (name == "GONegotiationRequest") {
            const gchar* peer = nullptr;
            guint16 passwordId = 0;
            guchar intent = 0;
            g_variant_get(params, "(&oqy)", &peer, &passwordId, &intent);
            onGoNegotiationRequest(peer, passwordId);
        } else if (name == "GroupStarted") {
            onGroupStarted(params);
        } else if (name == "GroupFinished") {
            onGroupFinished();
        } else if (name == "GONegotiationFailure" || name == "GroupFormationFailure") {
            if (!pendingPeer.empty() && !groupActive) {
                g_message("WifiDirectBeacon: Wi-Fi Direct connection with '%s' failed (%s)",
                          qPrintable(pendingSource.name), name.c_str());
                pendingPeer.clear();
            }
        }
    }

    // Returns the peer's property, or nullptr (caller unrefs)
    GVariant* peerProperty(const char* peer, const char* prop) {
        return get(peer, kPeerIf, prop, nullptr);
    }

    void onGoNegotiationRequest(const char* peer, guint16 passwordId) {
        if (!accepting) return;
        QString name = QStringLiteral("Wi-Fi Direct device");
        if (GVariant* v = peerProperty(peer, "DeviceName")) {
            name = QString::fromUtf8(g_variant_get_string(v, nullptr));
            g_variant_unref(v);
        }
        const bool busy = groupActive ||
            (!pendingPeer.empty() && pendingPeer != peer &&
             std::chrono::steady_clock::now() - pendingSince < kGroupFormationTimeout);
        if (busy) {
            g_message("WifiDirectBeacon: ignoring Wi-Fi Direct request from '%s': busy with "
                      "'%s'", qPrintable(name), qPrintable(pendingSource.name));
            return;
        }
        if (passwordId != kPasswordIdPushButton) {
            g_message("WifiDirectBeacon: ignoring Wi-Fi Direct request from '%s': it asked "
                      "for PIN pairing (password ID %u), only push-button is supported",
                      qPrintable(name), passwordId);
            return;
        }

        QString mac;
        if (GVariant* v = peerProperty(peer, "DeviceAddress")) {
            mac = macString(v);
            g_variant_unref(v);
        }
        uint16_t port = 0;
        if (GVariant* v = peerProperty(peer, "IEs")) {
            port = parseWfdRtspPort(byteArray(v));
            g_variant_unref(v);
        }
        pendingPeer   = peer;
        pendingSince  = std::chrono::steady_clock::now();
        pendingSource = WifiDirectSource{name, QString(), port ? port : kWfdRtspPort, mac};

        GError* err = nullptr;
        std::string via;
        const std::string nmDevice = nmP2PDevice();
        bool ok;
        if (!nmDevice.empty()) {
            via = "NetworkManager";
            ok = nmActivate(nmDevice, name, mac, &err);
        } else {
            via = "wpa_supplicant";
            ok = wpaConnect(peer, &err);
        }
        if (ok) {
            g_message("WifiDirectBeacon: accepting Wi-Fi Direct connection from '%s' (%s) "
                      "via %s", qPrintable(name), qPrintable(mac), via.c_str());
        } else {
            g_warning("WifiDirectBeacon: could not accept Wi-Fi Direct connection from '%s' "
                      "via %s: %s", qPrintable(name), via.c_str(), explain(err).c_str());
            g_clear_error(&err);
            pendingPeer.clear();
        }
    }

    // NetworkManager's object for this interface's P2P device, if NetworkManager
    // manages it; empty otherwise (not running, or device unmanaged).
    std::string nmP2PDevice() {
        GVariant* r = g_dbus_connection_call_sync(
            bus, kNmService, kNmPath, kNmIf, "GetDevices", nullptr, G_VARIANT_TYPE("(ao)"),
            G_DBUS_CALL_FLAGS_NONE, kTimeoutMs, nullptr, nullptr);
        if (!r) return {};
        const std::string want = "p2p-dev-" + ifname;
        std::string found;
        GVariantIter* it = nullptr;
        g_variant_get(r, "(ao)", &it);
        const gchar* path = nullptr;
        while (found.empty() && g_variant_iter_next(it, "&o", &path)) {
            GVariant* iface = nmProperty(path, "Interface");
            GVariant* managed = nmProperty(path, "Managed");
            if (iface && managed && want == g_variant_get_string(iface, nullptr) &&
                g_variant_get_boolean(managed)) {
                found = path;
            }
            if (iface) g_variant_unref(iface);
            if (managed) g_variant_unref(managed);
        }
        g_variant_iter_free(it);
        g_variant_unref(r);
        return found;
    }

    GVariant* nmProperty(const char* path, const char* prop) {
        GVariant* r = g_dbus_connection_call_sync(
            bus, kNmService, path, kPropsIf, "Get",
            g_variant_new("(ss)", kNmDeviceIf, prop), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, kTimeoutMs, nullptr, nullptr);
        if (!r) return nullptr;
        GVariant* v = nullptr;
        g_variant_get(r, "(v)", &v);
        g_variant_unref(r);
        return v;
    }

    // Activates a volatile (deleted on deactivation, never saved) wifi-p2p
    // connection to the source: NetworkManager accepts the pending GO
    // Negotiation Request and then owns, addresses and cleans up the group.
    bool nmActivate(const std::string& device, const QString& name, const QString& mac,
                    GError** err) {
        GVariantBuilder conn;
        g_variant_builder_init(&conn, G_VARIANT_TYPE("a{sv}"));
        const QByteArray id = ("AirShow Miracast " + name).toUtf8();
        g_variant_builder_add(&conn, "{sv}", "id", g_variant_new_string(id.constData()));
        g_variant_builder_add(&conn, "{sv}", "type", g_variant_new_string("wifi-p2p"));
        g_variant_builder_add(&conn, "{sv}", "autoconnect", g_variant_new_boolean(FALSE));

        GVariantBuilder p2p;
        g_variant_builder_init(&p2p, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&p2p, "{sv}", "peer",
                              g_variant_new_string(mac.toUtf8().constData()));
        g_variant_builder_add(&p2p, "{sv}", "wfd-ies",
                              bytesVariant(buildWfdIes(kWfdRtspPort)));

        GVariantBuilder settings;
        g_variant_builder_init(&settings, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&settings, "{sa{sv}}", "connection", &conn);
        g_variant_builder_add(&settings, "{sa{sv}}", "wifi-p2p", &p2p);

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&options, "{sv}", "persist", g_variant_new_string("volatile"));
        g_variant_builder_add(&options, "{sv}", "bind-activation",
                              g_variant_new_string("none"));

        GVariant* r = g_dbus_connection_call_sync(
            bus, kNmService, kNmPath, kNmIf, "AddAndActivateConnection2",
            g_variant_new("(a{sa{sv}}ooa{sv})", &settings, device.c_str(), "/", &options),
            nullptr, G_DBUS_CALL_FLAGS_NONE, kNmActivateTimeoutMs, nullptr, err);
        if (r) g_variant_unref(r);
        return r != nullptr;
    }

    // Without NetworkManager: accept the pending request as P2P client
    bool wpaConnect(const char* peer, GError** err) {
        GVariantBuilder args;
        g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&args, "{sv}", "peer", g_variant_new_object_path(peer));
        g_variant_builder_add(&args, "{sv}", "wps_method", g_variant_new_string("pbc"));
        g_variant_builder_add(&args, "{sv}", "go_intent", g_variant_new_int32(0));
        return call("Connect", g_variant_new("(a{sv})", &args), err);
    }

    void onGroupStarted(GVariant* params) {
        if (pendingPeer.empty()) return;  // a group someone else set up
        GVariant* info = g_variant_get_child_value(params, 0);
        const gchar* role = nullptr;
        const gchar* ifaceObj = nullptr;
        g_variant_lookup(info, "role", "&s", &role);
        g_variant_lookup(info, "interface_object", "&o", &ifaceObj);
        groupSourceAddr.clear();
        if (GVariant* go = g_variant_lookup_value(info, "IpAddrGo", G_VARIANT_TYPE("ay"))) {
            const QByteArray b = byteArray(go);
            if (b.size() == 4) {
                groupSourceAddr = ipv4String((uint32_t(uint8_t(b[0])) << 24) |
                                             (uint32_t(uint8_t(b[1])) << 16) |
                                             (uint32_t(uint8_t(b[2])) << 8) | uint8_t(b[3]));
            }
            g_variant_unref(go);
        }
        groupIfname.clear();
        if (ifaceObj) {
            if (GVariant* n = get(ifaceObj, kIfaceIf, "Ifname", nullptr)) {
                groupIfname = g_variant_get_string(n, nullptr);
                g_variant_unref(n);
            }
        }
        const std::string roleStr = role ? role : "";
        g_variant_unref(info);

        groupActive = true;
        if (roleStr != "client" || groupIfname.empty()) {
            // As group owner the sink would have to hand out the source's address
            g_warning("WifiDirectBeacon: Wi-Fi Direct group with '%s' started with this "
                      "device as %s; only the client role is supported",
                      qPrintable(pendingSource.name), roleStr.c_str());
            return;
        }
        g_message("WifiDirectBeacon: Wi-Fi Direct group with '%s' up on %s; waiting for "
                  "its IPv4 address", qPrintable(pendingSource.name), groupIfname.c_str());
        addressWaitedMs = 0;
        addressTimer.start();
        pollGroupAddress();
    }

    void pollGroupAddress() {
        uint32_t addr = 0, mask = 0;
        if (!groupActive || groupIfname.empty()) {
            addressTimer.stop();
            return;
        }
        if (!interfaceIPv4(groupIfname, addr, mask)) {
            addressWaitedMs += kAddressPollMs;
            if (addressWaitedMs >= kAddressWaitMs) {
                addressTimer.stop();
                g_warning("WifiDirectBeacon: %s got no IPv4 address; cannot reach '%s'",
                          groupIfname.c_str(), qPrintable(pendingSource.name));
            }
            return;
        }
        addressTimer.stop();
        WifiDirectSource source = pendingSource;
        // The group owner's address as reported by wpa_supplicant (EAPOL IP
        // allocation), else the first host of the subnet (the GO's DHCP server;
        // Android uses 192.168.49.1)
        source.address = !groupSourceAddr.isEmpty()
            ? groupSourceAddr : ipv4String((addr & mask) | 1u);
        g_message("WifiDirectBeacon: %s is %s; Miracast source '%s' at %s:%u",
                  groupIfname.c_str(), qPrintable(ipv4String(addr)), qPrintable(source.name),
                  qPrintable(source.address), source.rtspPort);
        owner->deliverSource(source);
    }

    void onGroupFinished() {
        if (!groupActive && pendingPeer.empty()) return;
        g_message("WifiDirectBeacon: Wi-Fi Direct group with '%s' ended",
                  qPrintable(pendingSource.name));
        addressTimer.stop();
        groupActive = false;
        groupIfname.clear();
        pendingPeer.clear();
        if (!accepting) return;
        // NetworkManager clears the WFD IEs when the connection goes away, and
        // listening stops while in a group: advertise again.
        std::string why;
        if (!apply(appliedName, why)) {
            g_warning("WifiDirectBeacon: could not re-advertise after the group ended: %s",
                      why.c_str());
        }
    }

    // Puts back the saved settings and forgets them. Keeps them (in memory and
    // QSettings) if wpa_supplicant can't be reached, so a later run can retry.
    void restore() {
        accepting = false;
        unsubscribe();
        addressTimer.stop();
        if (!savedConfig || !bus || ifacePath.empty()) return;

        GError* err = nullptr;
        call("ExtendedListen", listenVariant(0, 0), nullptr);
        bool ok = set(ifacePath, kP2PIf, "P2PDeviceConfig", savedConfig, &err);
        if (ok) ok = set(kRootPath, kService, "WFDIEs", savedWfd, &err);
        if (ok && savedConfigMethods) {
            ok = set(ifacePath, kWpsIf, "ConfigMethods", savedConfigMethods, &err);
        }
        if (!ok) {
            g_warning("WifiDirectBeacon: could not restore the Wi-Fi Direct settings (%s); "
                      "will retry on the next start", explain(err).c_str());
            g_clear_error(&err);
            return;
        }
        clearSaved();
        clearPersisted();
    }

    void recoverStale() {
        if (!hasPersisted()) return;
        GError* err = nullptr;
        if (!connectBus(&err) || !findInterface(&err) || !loadPersisted()) {
            g_clear_error(&err);
            return;  // try again on a later run
        }
        g_message("WifiDirectBeacon: restoring Wi-Fi Direct settings left by a previous run");
        restore();
    }
};

WifiDirectBeacon::WifiDirectBeacon() : d(std::make_unique<Impl>(this)) {
    m_thread.setObjectName(QStringLiteral("WifiDirectBeacon"));
    // wpa_supplicant may restart, or another client may reset the P2P device;
    // re-apply periodically (idempotent).
    m_refreshTimer.setInterval(60 * 1000);
    m_refreshTimer.callOnTimeout([this] { postApply(/*logResult=*/false); });
}

WifiDirectBeacon::~WifiDirectBeacon() {
    stop();
    if (m_thread.isRunning()) {
        // Hand d back to this thread so it is destroyed where it lives
        QThread* here = QThread::currentThread();
        QMetaObject::invokeMethod(d.get(), [this, here] { d->moveToThread(here); },
                                  Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
}

void WifiDirectBeacon::ensureWorker() {
    if (m_thread.isRunning()) return;
    d->moveToThread(&m_thread);
    m_thread.start();
}

void WifiDirectBeacon::postApply(bool logResult) {
    // Skip timer refreshes while one is still queued (wpa_supplicant stuck)
    if (!logResult && m_applyPending.exchange(true)) return;
    if (logResult) m_applyPending = true;
    const QString name = m_deviceName;
    QMetaObject::invokeMethod(d.get(), [this, name, logResult] {
        m_applyPending = false;
        std::string why;
        const bool ok  = d->apply(name, why);
        const bool was = m_active.exchange(ok);
        if (logResult) {
            if (ok) g_message("WifiDirectBeacon: discoverable in Windows Connect (Win+K) as "
                              "'%s' %s", qPrintable(name), why.c_str());
            else    g_message("WifiDirectBeacon: Windows Miracast discovery (Win+K) "
                              "unavailable — %s", why.c_str());
        } else if (ok != was) {
            if (ok) g_message("WifiDirectBeacon: discoverable again %s", why.c_str());
            else    g_warning("WifiDirectBeacon: lost Wi-Fi Direct discovery (%s); retrying",
                              why.c_str());
        }
    }, Qt::QueuedConnection);
}

void WifiDirectBeacon::start(const QString& deviceName) {
    m_deviceName = deviceName;
    ensureWorker();
    postApply(/*logResult=*/true);
    m_refreshTimer.start();
}

void WifiDirectBeacon::rename(const QString& deviceName) {
    m_deviceName = deviceName;
    if (m_refreshTimer.isActive()) {
        postApply(/*logResult=*/true);
    }
}

void WifiDirectBeacon::setListenPaused(bool paused) {
    if (!m_thread.isRunning()) return;  // never started: nothing listens
    QMetaObject::invokeMethod(d.get(), [this, paused] {
        if (d->listenPaused == paused) return;
        d->listenPaused = paused;
        if (!d->accepting) return;  // stopped: restore() already ended the listen
        std::string why;
        if (d->apply(d->appliedName, why)) {
            g_message("WifiDirectBeacon: %s listening for Wi-Fi Direct scans",
                      paused ? "paused" : "resumed");
        } else {
            g_warning("WifiDirectBeacon: could not %s listening: %s",
                      paused ? "pause" : "resume", why.c_str());
        }
    }, Qt::QueuedConnection);
}

void WifiDirectBeacon::recoverStaleState() {
    if (!Impl::hasPersisted()) return;
    ensureWorker();
    QMetaObject::invokeMethod(d.get(), [this] { d->recoverStale(); }, Qt::QueuedConnection);
}

void WifiDirectBeacon::stop() {
    m_refreshTimer.stop();
    if (!m_thread.isRunning()) return;
    // Blocking: runs after any queued apply, and before the process may exit
    QMetaObject::invokeMethod(d.get(), [this] {
        const bool was = m_active.exchange(false);
        d->restore();
        if (was) g_message("WifiDirectBeacon: stopped");
    }, Qt::BlockingQueuedConnection);
}

#else  // !__linux__

struct WifiDirectBeacon::Impl {};

WifiDirectBeacon::WifiDirectBeacon() : d(std::make_unique<Impl>()) {}
WifiDirectBeacon::~WifiDirectBeacon() = default;
void WifiDirectBeacon::start(const QString& deviceName) { m_deviceName = deviceName; }
void WifiDirectBeacon::stop() {}
void WifiDirectBeacon::rename(const QString& deviceName) { m_deviceName = deviceName; }
void WifiDirectBeacon::recoverStaleState() {}
void WifiDirectBeacon::setListenPaused(bool) {}
void WifiDirectBeacon::ensureWorker() {}
void WifiDirectBeacon::postApply(bool) {}

#endif

} // namespace airshow
