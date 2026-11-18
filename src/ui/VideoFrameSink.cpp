#include "ui/VideoFrameSink.h"
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QQuickWindow>
#include <QMetaObject>

namespace airshow {

VideoFrameSink::VideoFrameSink(QQuickItem* parent)
    : QQuickItem(parent)
{
    // Required for QQuickItem to participate in scene graph rendering.
    setFlag(QQuickItem::ItemHasContents, true);
}

void VideoFrameSink::setForceAspectRatio(bool v)
{
    if (m_forceAspectRatio == v) return;
    m_forceAspectRatio = v;
    emit forceAspectRatioChanged();
    update();
}

void VideoFrameSink::pushFrame(const QImage& frame)
{
    {
        QMutexLocker lock(&m_mutex);
        m_pendingFrame = frame;
        m_dirty = true;
    }
    // update() must be called on the main/render thread — queue it from the streaming thread.
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void VideoFrameSink::clear()
{
    {
        QMutexLocker lock(&m_mutex);
        m_pendingFrame = QImage();
        m_dirty   = false;
        m_cleared = true;
    }
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

QSGNode* VideoFrameSink::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    QImage frame;
    {
        QMutexLocker lock(&m_mutex);
        if (m_cleared && !m_dirty) {
            // Draw nothing, but keep the node and its texture alive: the render
            // thread may still draw the previous frame's batch, and deleting the
            // texture under it crashes (QSGTexture::setFiltering on freed memory).
            // The texture is replaced by the next frame's.
            m_cleared = false;
            if (oldNode) static_cast<QSGSimpleTextureNode*>(oldNode)->setRect(QRectF());
            return oldNode;
        }
        m_cleared = false;
        if (!m_dirty || m_pendingFrame.isNull())
            return oldNode;
        frame = m_pendingFrame;
        m_dirty = false;
    }

    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setFiltering(QSGTexture::Linear);
        node->setOwnsTexture(false);  // We manage texture lifetime explicitly below.
    }

    // Create the new texture first: if that fails, keep showing the old one
    // rather than leaving the node with a null/freed texture.
    QSGTexture* tex = window()->createTextureFromImage(frame,
        QQuickWindow::TextureIsOpaque);
    if (!tex) {
        if (node->texture()) return node;
        delete node;  // brand-new node with nothing to show
        return nullptr;
    }
    // Delete the old texture before replacing (safe — we're on the render thread).
    delete node->texture();
    node->setTexture(tex);

    // Apply letterbox/pillarbox if forceAspectRatio is set.
    QRectF dest = boundingRect();
    if (m_forceAspectRatio && !frame.isNull() && dest.width() > 0 && dest.height() > 0) {
        const double frameAr = double(frame.width()) / double(frame.height());
        const double itemAr  = dest.width() / dest.height();
        if (frameAr > itemAr) {
            // Wider frame than item — pillarbox (black bars on sides).
            const double h = dest.width() / frameAr;
            dest.setTop(dest.top() + (dest.height() - h) / 2.0);
            dest.setHeight(h);
        } else {
            // Taller frame than item — letterbox (black bars top/bottom).
            const double w = dest.height() * frameAr;
            dest.setLeft(dest.left() + (dest.width() - w) / 2.0);
            dest.setWidth(w);
        }
    }
    node->setRect(dest);

    return node;
}

} // namespace airshow
