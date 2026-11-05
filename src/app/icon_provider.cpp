// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/icon_provider.hpp"

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQmlEngine>
#include <QQuickAsyncImageProvider>
#include <QRunnable>
#include <QThreadPool>

#include <algorithm>
#include <QUrl>

#include <optional>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

namespace {

// Icons are a few KB each and a process list rarely names more than a few
// hundred distinct images, so the cost is counted in entries, not bytes.
class IconCache {
public:
    std::optional<QImage> find(const QString& path)
    {
        QMutexLocker lock(&mu_);
        if (auto* img = cache_.object(path)) return *img;
        return std::nullopt;
    }
    void put(const QString& path, const QImage& img)
    {
        QMutexLocker lock(&mu_);
        cache_.insert(path, new QImage(img));
    }

private:
    QMutex mu_;
    QCache<QString, QImage> cache_{512};
};

QImage hicon_to_image(HICON h)
{
    QImage img = QImage::fromHICON(h);
    DestroyIcon(h);
    return img;
}

// Returns the process's own icon, or a null image if it has none we can read.
// Never the generic fallback: that is decided by the caller and never cached.
QImage load_icon(const QString& path, int px)
{
    if (path.isEmpty()) return {};
    const auto w = path.toStdWString();
    // Straight from the exe's resources at the exact size: crisp at any DPI,
    // no shell, no COM, and safe to call from several threads.
    HICON h = nullptr;
    UINT id = 0;
    if (PrivateExtractIconsW(w.c_str(), 0, px, px, &h, &id, 1, LR_DEFAULTCOLOR) == 1 && h)
        return hicon_to_image(h);
    // Some images carry no icon group of their own but the shell maps one
    // (store apps, redirected binaries).
    SHFILEINFOW info{};
    if (SHGetFileInfoW(w.c_str(), 0, &info, sizeof(info), SHGFI_ICON | (px > 16 ? SHGFI_LARGEICON : SHGFI_SMALLICON)) && info.hIcon)
        return hicon_to_image(info.hIcon);
    return {};
}

QImage generic_icon(int px)
{
    SHFILEINFOW info{};
    if (SHGetFileInfoW(L"x.exe", FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_USEFILEATTRIBUTES | (px > 16 ? SHGFI_LARGEICON : SHGFI_SMALLICON)) && info.hIcon)
        return hicon_to_image(info.hIcon);
    return {};
}

class IconResponse : public QQuickImageResponse, public QRunnable {
public:
    IconResponse(QString path, QSize requested, IconCache& cache)
        : path_(std::move(path)), requested_(requested), cache_(cache)
    {
        setAutoDelete(false);
    }

    QQuickTextureFactory* textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(image_);
    }

    void run() override
    {
        const int px = requested_.isValid() ? std::max(16, requested_.width()) : 32;
        const QString key = path_.toLower() + u'|' + QString::number(px);
        if (auto hit = cache_.find(key)) {
            image_ = *hit;
        } else {
            // The shell fallback resolves icon handlers through COM, and pool
            // threads start with no apartment.
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            image_ = load_icon(path_, px);
            // Only real icons are cached; a failure may be transient and must not
            // pin the generic icon to this path for the rest of the session.
            if (!image_.isNull()) cache_.put(key, image_);
            else image_ = generic_icon(px);
            if (SUCCEEDED(hr)) CoUninitialize();
        }
        if (requested_.isValid() && !image_.isNull() && image_.size() != requested_)
            image_ = image_.scaled(requested_, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        emit finished();
    }

private:
    QString path_;
    QSize requested_;
    IconCache& cache_;
    QImage image_;
};

class IconProvider : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override
    {
        auto* r = new IconResponse(QUrl::fromPercentEncoding(id.toUtf8()), requestedSize, cache_);
        pool_.start(r);
        return r;
    }

    IconProvider() { pool_.setMaxThreadCount(2); }

private:
    IconCache cache_;
    QThreadPool pool_;
};

} // namespace

void register_icon_provider(QQmlEngine& engine)
{
    engine.addImageProvider(QStringLiteral("procicon"), new IconProvider);
}
