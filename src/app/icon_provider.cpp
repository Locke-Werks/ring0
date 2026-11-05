// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/icon_provider.hpp"

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQmlEngine>
#include <QQuickAsyncImageProvider>
#include <QRunnable>
#include <QThreadPool>
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

QImage load_icon(const QString& path, bool large)
{
    const UINT size = large ? SHGFI_LARGEICON : SHGFI_SMALLICON;
    SHFILEINFOW info{};
    if (!path.isEmpty()) {
        const auto w = path.toStdWString();
        if (SHGetFileInfoW(w.c_str(), 0, &info, sizeof(info), SHGFI_ICON | size) && info.hIcon)
            return hicon_to_image(info.hIcon);
    }
    // USEFILEATTRIBUTES asks the shell for the generic .exe icon without
    // touching the disk, which is what a denied or vanished image needs.
    info = {};
    if (SHGetFileInfoW(L"x.exe", FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_USEFILEATTRIBUTES | size) && info.hIcon)
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
        const bool large = requested_.isValid() && requested_.width() > 16;
        const QString key = path_.toLower() + (large ? u"|L" : u"|S");
        if (auto hit = cache_.find(key)) {
            image_ = *hit;
        } else {
            // The shell resolves icon handlers through COM, and pool threads
            // start with no apartment.
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            image_ = load_icon(path_, large);
            if (SUCCEEDED(hr)) CoUninitialize();
            if (!image_.isNull()) cache_.put(key, image_);
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

private:
    IconCache cache_;
    QThreadPool pool_;
};

} // namespace

void register_icon_provider(QQmlEngine& engine)
{
    engine.addImageProvider(QStringLiteral("procicon"), new IconProvider);
}
