#pragma once
#include <QByteArray>
#include <QString>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace airshow {

// A Miracast source (e.g. an Android phone) that has formed a Wi-Fi Direct
// group with this receiver and is waiting for the sink to open the WFD RTSP
// session. address is the source's IPv4 address on the group.
struct WifiDirectSource {
    QString  name;
    QString  address;        // IPv4 address on the group
    uint16_t rtspPort = 7236;
    QString  deviceAddress;  // P2P Device Address (MAC), identifies the source
};

// Makes the receiver show up in Windows "Connect" (Win+K) as a Miracast over
// Infrastructure (MS-MICE) sink, and accepts Wi-Fi Direct Miracast connections
// from sources that use a real P2P group (Android: Samsung Smart View / DeX,
// and other OEM "Screen mirroring" implementations).
//
// Windows discovers MS-MICE sinks through Wi-Fi Direct: it only lists a sink
// whose P2P probe responses carry the MS-MICE Vendor Extension (Capability +
// Host Name attributes, [MS-MICE] 2.2.8). It then resolves the host name
// (mDNS _display._tcp, see DiscoveryManager) and connects to TCP 7250 over the
// existing network; no Wi-Fi Direct group is ever formed.
//
// On Linux this asks wpa_supplicant (D-Bus API, fi.w1.wpa_supplicant1) to add
// that Vendor Extension and a Wi-Fi Display IE to its P2P device, and to stay
// in extended listen so Windows' discovery scan gets a response. The station
// connection is not touched.
//
// Privacy: probe responses are unencrypted and readable by any nearby Wi-Fi
// device, so this broadcasts the receiver name and the machine's host name.
// It is on by default; turn it off with the "miracast/wifiDirectBeacon"
// setting or the --no-beacon command-line switch (see main.cpp, README).
//
// Restoring wpa_supplicant: the P2P settings in place before start() are
// saved to QSettings ("miracast/wifiDirectRestore/*") and put back by stop().
// If AirShow is killed or crashes before stop(), the next run puts them back
// (recoverStaleState(), or start() reusing the saved values).
//
// D-Bus calls are synchronous with a 2 s timeout, so they run on a worker
// thread and never block the GUI thread. stop() waits for the restore.
//
// Android sources don't use MS-MICE: they form a Wi-Fi Direct group with the
// sink and then expect it to connect to their RTSP server (port 7236 by
// default) over that group. The beacon advertises push-button pairing (WPS
// ConfigMethods "virtual_push_button"), and when a source sends a push-button
// GO Negotiation Request it accepts it as P2P client (GO intent 0), so the
// source is the group owner and hands out the addresses (Android's GO is
// 192.168.49.1). Once the group is up and the group interface has an IPv4
// address, the source handler (setSourceHandler) is called on the thread that
// owns this object; it is expected to start the RTSP session.
//
// When NetworkManager manages the P2P device it has to own the group:
// otherwise it removes any group interface it did not ask for. The request is
// then accepted by activating a volatile (never saved) wifi-p2p connection to
// the source, which needs NetworkManager's network-control permission (an
// active local session has it). Without NetworkManager, wpa_supplicant's
// Connect is used directly, and something else has to configure the group
// interface's address (wpa_supplicant only reports it).
//
// Accepting is not approval: anyone in radio range can form the group (as with
// a TV). The connection is then subject to the same approval as any other
// Miracast connection before the session starts (MiracastHandler). Only one
// group at a time is accepted; PIN pairing is not supported.
//
// Needs a Wi-Fi adapter with P2P support and permission to use wpa_supplicant's
// D-Bus API (root, or membership of the "netdev" group on Debian/Ubuntu).
// When unavailable, it logs why and the receiver simply doesn't appear in
// Win+K; other protocols are unaffected. No-op on macOS and Windows.
class WifiDirectBeacon {
public:
    WifiDirectBeacon();
    ~WifiDirectBeacon();
    WifiDirectBeacon(const WifiDirectBeacon&) = delete;
    WifiDirectBeacon& operator=(const WifiDirectBeacon&) = delete;

    // deviceName: name shown in Win+K (the receiver name). Asynchronous: the
    // outcome is logged, and isActive() reflects it once applied.
    void start(const QString& deviceName);
    // Restores the previous P2P settings. Blocks until done (up to a few seconds
    // if wpa_supplicant is unresponsive). Safe to call when not started.
    void stop();
    void rename(const QString& deviceName);
    bool isActive() const { return m_active.load(); }

    // For when the beacon is disabled: if a previous run was killed while the
    // beacon was on, restore the P2P settings it saved. Asynchronous.
    void recoverStaleState();

    // Stops listening for Wi-Fi Direct scans while paused (e.g. while a sender
    // is on screen), and resumes it after. The listen takes the radio off the
    // station's channel 10% of the time, and a stream arriving over the
    // network loses packets meanwhile (measured with the earlier 50% listen:
    // 37% of a Windows Miracast stream on a Raspberry Pi 4, none paused). The receiver
    // can't be discovered while paused. Asynchronous.
    void setListenPaused(bool paused);

    // Called (on this object's thread) when a Wi-Fi Direct source is ready for
    // the RTSP session. May be set before or after start().
    void setSourceHandler(std::function<void(const WifiDirectSource&)> handler);

    // MS-MICE Vendor Extension value (WPS Vendor Extension attribute body):
    // Microsoft vendor ID 00:01:37, then Capability (0x2001) and Host Name (0x2002)
    // attributes. Public for unit tests.
    static QByteArray buildMiceVendorExtension(const QString& hostName);

    // Wi-Fi Display IE subelements for a primary sink available for a session.
    static QByteArray buildWfdIes(uint16_t rtspPort);

    // RTSP (session management control) port from a peer's WFD IE subelements
    // (WFD Device Information, subelement 0), or 0 if absent or malformed.
    static uint16_t parseWfdRtspPort(const QByteArray& subelements);

    // GO Negotiation Request Device Password ID for push-button pairing (WPS)
    static constexpr uint16_t kPasswordIdPushButton = 4;

private:
    void ensureWorker();
    void postApply(bool logResult);
    void deliverSource(const WifiDirectSource& source);  // any thread

    struct Impl;                 // lives on m_thread (Linux)
    std::unique_ptr<Impl> d;
    QThread           m_thread;
    QTimer            m_refreshTimer;
    QString           m_deviceName;
    std::function<void(const WifiDirectSource&)> m_sourceHandler;
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_applyPending{false};
};

} // namespace airshow
