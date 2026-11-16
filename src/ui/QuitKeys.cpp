#include "ui/QuitKeys.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QRegularExpression>

namespace airshow {

namespace {
// Qt reports evdev key code + 8 (the XKB keycode) as the native scan code on
// Linux platforms
constexpr quint32 kXkbOffset = 8;
} // namespace

// static
bool QuitKeys::parse(const QString& spec, QList<Key>& keys, QString& error) {
    keys.clear();
    const QStringList tokens =
        spec.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        Key key;
        key.text = token;
        bool isNumber = false;
        const int code = token.toInt(&isNumber, 10);
        if (isNumber) {
            if (code <= 0 || code > 0x2ff) {  // KEY_MAX
                error = QStringLiteral("'%1' is not a Linux key code").arg(token);
                return false;
            }
            key.linuxCode = code;
        } else {
            const QKeySequence seq = QKeySequence::fromString(token, QKeySequence::PortableText);
            if (seq.count() != 1 || seq[0].keyboardModifiers() != Qt::NoModifier ||
                seq[0].key() == Qt::Key_unknown) {
                error = QStringLiteral("'%1' is not a key name (e.g. Escape, F11, HomePage) "
                                       "or Linux key code (e.g. 172)").arg(token);
                return false;
            }
            key.qtKey = seq[0].key();
        }
        keys.append(key);
    }
    if (keys.isEmpty()) {
        error = QStringLiteral("no keys given");
        return false;
    }
    return true;
}

// static
const QuitKeys::Key* QuitKeys::match(const QList<Key>& keys, int qtKey, quint32 nativeScanCode) {
    for (const Key& key : keys) {
        if (key.qtKey != 0 && key.qtKey == qtKey) return &key;
#ifdef __linux__
        if (key.linuxCode >= 0 && nativeScanCode >= kXkbOffset &&
            nativeScanCode - kXkbOffset == quint32(key.linuxCode)) {
            return &key;
        }
#else
        Q_UNUSED(nativeScanCode);
#endif
    }
    return nullptr;
}

QuitKeys::QuitKeys(QList<Key> keys, QObject* parent)
    : QObject(parent), m_keys(std::move(keys)) {}

bool QuitKeys::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress && !m_quitting) {
        const auto* ke = static_cast<QKeyEvent*>(event);
        if (!ke->isAutoRepeat()) {
            if (const Key* key = match(m_keys, ke->key(), ke->nativeScanCode())) {
                // The same press is delivered to the window and then its items:
                // quit once
                m_quitting = true;
                qInfo("Quit key '%s' pressed — exiting", qPrintable(key->text));
                QCoreApplication::quit();
                return true;
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

} // namespace airshow
