#pragma once
#include <QList>
#include <QObject>
#include <QString>

namespace airshow {

// airshow --quit-keys: exit when one of the given keys is pressed, so a kiosk
// can leave a full-screen AirShow mode with a remote control button.
//
// Keys are given as Linux key codes (numbers, as evtest shows them, e.g. 172 for
// a remote's Home button, KEY_HOMEPAGE) or Qt key names ("Escape", "F11",
// "HomePage"), separated by commas or spaces. Linux key codes are matched on
// Linux only (Wayland, X11, EGLFS), where Qt reports the code + 8 as the native
// scan code.
//
// Keys reach AirShow only while its window has focus. A quit key quits even
// while an approval prompt is open, so don't use the prompt's keys (Enter,
// Return, Esc, Space, arrows, Tab, Y, N).
class QuitKeys : public QObject {
    Q_OBJECT
public:
    struct Key {
        int     qtKey     = 0;   // Qt::Key, or 0
        int     linuxCode = -1;  // Linux input key code, or -1
        QString text;            // as given, for logging
    };

    // Parses spec into keys. Returns false and sets error if a key is unknown
    // or spec is empty.
    static bool parse(const QString& spec, QList<Key>& keys, QString& error);

    // The key (if any) that a key press with this Qt key and native scan code
    // matches, or nullptr. Public for tests.
    static const Key* match(const QList<Key>& keys, int qtKey, quint32 nativeScanCode);

    explicit QuitKeys(QList<Key> keys, QObject* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QList<Key> m_keys;
    bool       m_quitting = false;
};

} // namespace airshow
