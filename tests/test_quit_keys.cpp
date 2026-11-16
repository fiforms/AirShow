#include <gtest/gtest.h>
#include "ui/QuitKeys.h"

using airshow::QuitKeys;

TEST(QuitKeysTest, ParsesLinuxCodesAndNames) {
    QList<QuitKeys::Key> keys;
    QString error;
    ASSERT_TRUE(QuitKeys::parse(QStringLiteral("172, 158 Escape,F11"), keys, error)) << error.toStdString();
    ASSERT_EQ(keys.size(), 4);
    EXPECT_EQ(keys[0].linuxCode, 172);
    EXPECT_EQ(keys[1].linuxCode, 158);
    EXPECT_EQ(keys[2].qtKey, int(Qt::Key_Escape));
    EXPECT_EQ(keys[3].qtKey, int(Qt::Key_F11));
}

TEST(QuitKeysTest, RejectsBadKeys) {
    QList<QuitKeys::Key> keys;
    QString error;
    EXPECT_FALSE(QuitKeys::parse(QString(), keys, error));
    EXPECT_FALSE(QuitKeys::parse(QStringLiteral("NotAKey"), keys, error));
    EXPECT_FALSE(QuitKeys::parse(QStringLiteral("Ctrl+Q"), keys, error));  // single keys only
    EXPECT_FALSE(QuitKeys::parse(QStringLiteral("0"), keys, error));
    EXPECT_FALSE(QuitKeys::parse(QStringLiteral("99999"), keys, error));
}

// On Linux, Qt reports the Linux key code + 8 as the native scan code
TEST(QuitKeysTest, Matches) {
    QList<QuitKeys::Key> keys;
    QString error;
    ASSERT_TRUE(QuitKeys::parse(QStringLiteral("172,Escape"), keys, error));
    EXPECT_NE(QuitKeys::match(keys, Qt::Key_Escape, 0), nullptr);
    EXPECT_EQ(QuitKeys::match(keys, Qt::Key_Return, 36), nullptr);
#ifdef __linux__
    const QuitKeys::Key* home = QuitKeys::match(keys, Qt::Key_HomePage, 172 + 8);
    ASSERT_NE(home, nullptr);
    EXPECT_EQ(home->text, QStringLiteral("172"));
    EXPECT_EQ(QuitKeys::match(keys, Qt::Key_Back, 158 + 8), nullptr);
#endif
}
