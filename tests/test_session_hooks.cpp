// SessionHooks: --on-session-start / --on-session-end command hooks.

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "ui/ConnectionBridge.h"
#include "ui/SessionHooks.h"

using airshow::ConnectionBridge;
using airshow::SessionHooks;

namespace {

// Spin the event loop until pred() holds or timeoutMs passes.
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 3000) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return pred();
}

void spin(int ms) {
    waitFor([] { return false; }, ms);
}

} // namespace

TEST(SessionHooks, StartsOnConnectAndEndsAfterDelay) {
    ConnectionBridge bridge;
    SessionHooks hooks(&bridge, {}, {}, 100);
    QSignalSpy started(&hooks, &SessionHooks::sessionStarted);
    QSignalSpy ended(&hooks, &SessionHooks::sessionEnded);

    bridge.setConnected(true, QStringLiteral("iPhone"), QStringLiteral("AirPlay"));
    ASSERT_EQ(started.count(), 1);
    EXPECT_EQ(started.at(0).at(0).toString(), QStringLiteral("iPhone"));
    EXPECT_EQ(started.at(0).at(1).toString(), QStringLiteral("AirPlay"));
    EXPECT_TRUE(hooks.isSessionActive());

    bridge.setConnected(false);
    EXPECT_EQ(ended.count(), 0) << "end must wait for the grace period";
    ASSERT_TRUE(waitFor([&] { return ended.count() == 1; }));
    EXPECT_EQ(ended.at(0).at(0).toString(), QStringLiteral("iPhone"));
    EXPECT_FALSE(hooks.isSessionActive());
}

TEST(SessionHooks, BriefDisconnectDoesNotEndSession) {
    ConnectionBridge bridge;
    SessionHooks hooks(&bridge, {}, {}, 300);
    QSignalSpy started(&hooks, &SessionHooks::sessionStarted);
    QSignalSpy ended(&hooks, &SessionHooks::sessionEnded);

    bridge.setConnected(true, QStringLiteral("TV remote"), QStringLiteral("DLNA"));
    bridge.setConnected(false);
    spin(50);
    bridge.setConnected(true, QStringLiteral("TV remote"), QStringLiteral("DLNA"));
    spin(500);
    EXPECT_EQ(started.count(), 1);
    EXPECT_EQ(ended.count(), 0);
}

TEST(SessionHooks, ApprovalPromptCountsAsActive) {
    ConnectionBridge bridge;
    SessionHooks hooks(&bridge, {}, {}, 100);
    QSignalSpy started(&hooks, &SessionHooks::sessionStarted);
    QSignalSpy ended(&hooks, &SessionHooks::sessionEnded);

    bridge.showApprovalRequest(QStringLiteral("req1"), QStringLiteral("Pixel"),
                               QStringLiteral("Cast"));
    ASSERT_EQ(started.count(), 1);
    EXPECT_EQ(started.at(0).at(0).toString(), QStringLiteral("Pixel"));

    // Approved: the connection follows the prompt — still one session.
    bridge.clearApprovalRequest();
    bridge.setConnected(true, QStringLiteral("Pixel"), QStringLiteral("Cast"));
    spin(200);
    EXPECT_EQ(started.count(), 1);
    EXPECT_EQ(ended.count(), 0);

    bridge.setConnected(false);
    EXPECT_TRUE(waitFor([&] { return ended.count() == 1; }));
}

#ifndef _WIN32
TEST(SessionHooks, RunsCommandsWithSessionEnvironment) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString log = dir.filePath(QStringLiteral("hooks.log"));
    const QString cmd =
        QStringLiteral("echo \"$AIRSHOW_SESSION|$AIRSHOW_DEVICE|$AIRSHOW_PROTOCOL\" >> '%1'").arg(log);

    auto readLog = [&] {
        QFile f(log);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };

    ConnectionBridge bridge;
    {
        SessionHooks hooks(&bridge, cmd, cmd, 50);
        bridge.setConnected(true, QStringLiteral("My Phone"), QStringLiteral("AirPlay"));
        ASSERT_TRUE(waitFor([&] { return readLog().contains(QStringLiteral("start|")); }));
        EXPECT_EQ(readLog(), QStringLiteral("start|My Phone|AirPlay\n"));
        bridge.setConnected(false);
        ASSERT_TRUE(waitFor([&] { return readLog().contains(QStringLiteral("end|")); }));
        EXPECT_EQ(readLog(), QStringLiteral("start|My Phone|AirPlay\nend|My Phone|AirPlay\n"));

        // Quitting mid-session runs the end hook too.
        bridge.setConnected(true, QStringLiteral("Laptop"), QStringLiteral("Miracast"));
        ASSERT_TRUE(waitFor([&] { return readLog().contains(QStringLiteral("start|Laptop")); }));
    }
    EXPECT_TRUE(waitFor([&] { return readLog().contains(QStringLiteral("end|Laptop|Miracast")); }));
}
#endif

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
