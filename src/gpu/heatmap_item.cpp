// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/heatmap_item.hpp"
#include "app/snapshot_bridge.hpp"
#include "core/timebase.hpp"

#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {


// Owns an R8 QRhiTexture and re-uploads only when the item hands it new cells.
// The QRhiTexture is recreated only when the grid size changes.
class HeatTexture final : public QSGTexture {
public:
    ~HeatTexture() override
    {
        if (tex_) tex_->deleteLater();
    }

    void setCells(const std::vector<std::uint8_t>& cells, QSize size)
    {
        pending_ = cells;
        size_ = size;
        dirty_ = true;
    }

    qint64 comparisonKey() const override { return qint64(reinterpret_cast<quintptr>(this)); }
    QRhiTexture* rhiTexture() const override { return tex_; }
    QSize textureSize() const override { return size_; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }

    void commitTextureOperations(QRhi* rhi, QRhiResourceUpdateBatch* batch) override
    {
        if (!dirty_ || size_.isEmpty()) return;
        if (!tex_ || tex_->pixelSize() != size_) {
            if (tex_) tex_->deleteLater();
            tex_ = rhi->newTexture(QRhiTexture::R8, size_);
            if (!tex_->create()) {
                delete tex_;
                tex_ = nullptr;
                return;
            }
        }
        // R8 rows are tightly packed in the source; the stride tells the backend so
        // it can pad to its own row alignment.
        QRhiTextureSubresourceUploadDescription sub(pending_.data(), quint32(pending_.size()));
        sub.setDataStride(quint32(size_.width()));
        sub.setSourceSize(size_);
        batch->uploadTexture(tex_, QRhiTextureUploadDescription({ 0, 0, sub }));
        dirty_ = false;
    }

private:
    QRhiTexture* tex_ = nullptr;
    std::vector<std::uint8_t> pending_;
    QSize size_;
    bool dirty_ = false;
};

class HeatMaterial final : public QSGMaterial {
public:
    HeatMaterial()
    {
        setFlag(Blending, true);
        texture.setFiltering(QSGTexture::Linear);
        texture.setHorizontalWrapMode(QSGTexture::ClampToEdge);
        texture.setVerticalWrapMode(QSGTexture::ClampToEdge);
    }

    QSGMaterialType* type() const override
    {
        static QSGMaterialType t;
        return &t;
    }
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial* other) const override
    {
        // Every heatmap owns distinct cells, so instances never batch together.
        return this == other ? 0 : (this < other ? -1 : 1);
    }

    HeatTexture texture;
    float rows = 1, cols = 1, phase = 1, row_px = 1, gap = 0;
    float hue[4] = { 0, 0, 0, 1 };
};

class HeatShader final : public QSGMaterialShader {
public:
    HeatShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/heatmap.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/heatmap.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* newMat, QSGMaterial*) override
    {
        auto* m = static_cast<HeatMaterial*>(newMat);
        QByteArray* buf = state.uniformData();
        Q_ASSERT(buf->size() >= 104);
        char* d = buf->data();
        // Unbatched material: the dirty flags may refer to another node's draw.
        const QMatrix4x4 mvp = state.combinedMatrix();
        std::memcpy(d, mvp.constData(), 64);
        const float o = state.opacity();
        std::memcpy(d + 64, &o, 4);
        const float block[3] = { m->rows, m->cols, m->phase };
        std::memcpy(d + 68, block, sizeof block);
        std::memcpy(d + 80, m->hue, 16);
        const float tail[2] = { m->row_px, m->gap };
        std::memcpy(d + 96, tail, sizeof tail);
        return true;
    }

    void updateSampledImage(RenderState& state, int binding, QSGTexture** tex,
                            QSGMaterial* newMat, QSGMaterial*) override
    {
        if (binding != 1) return;
        auto* m = static_cast<HeatMaterial*>(newMat);
        m->texture.commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        *tex = &m->texture;
    }
};

QSGMaterialShader* HeatMaterial::createShader(QSGRendererInterface::RenderMode) const
{
    return new HeatShader;
}

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::uint8_t quantize(float pct)
{
    return std::uint8_t(std::lround(std::clamp(pct, 0.0f, 100.0f) * 2.55f));
}

} // namespace

HeatmapItem::HeatmapItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    connect(SnapshotBridge::instance(), &SnapshotBridge::changed, this, &HeatmapItem::refresh);
}

void HeatmapItem::setSource(const QString& s) { if (s != source_) { source_ = s; emit sourceChanged(); rebuild(false); } }
void HeatmapItem::setSamples(int n) { n = std::max(n, 2); if (n != samples_) { samples_ = n; emit samplesChanged(); rebuild(false); } }
void HeatmapItem::setHue(const QColor& c) { if (c != hue_) { hue_ = c; emit hueChanged(); update(); } }
void HeatmapItem::setRowGap(qreal g) { if (g != row_gap_) { row_gap_ = g; emit rowGapChanged(); update(); } }

void HeatmapItem::itemChange(ItemChange change, const ItemChangeData& data)
{
    if (change == ItemSceneChange) {
        QObject::disconnect(frame_conn_);
        // afterAnimating fires on the GUI thread once per frame, which keeps the
        // scroll locked to the window's own clock instead of a free-running timer.
        if (data.window)
            frame_conn_ = connect(data.window, &QQuickWindow::afterAnimating, this, [this] {
                // Live: position follows the clock every frame, like the sparklines.
                if (live_ && rows_ > 0 && isVisible()) update();
            });
    }
    QQuickItem::itemChange(change, data);
}

void HeatmapItem::refresh()
{
    auto* bridge = SnapshotBridge::instance();
    const auto latest = bridge->history().latest();
    const std::uint64_t seq = latest ? latest->seq : 0;
    // Only a live new sample scrolls; scrubbing jumps straight to the position.
    (void)seq;
    rebuild(false);
}

void HeatmapItem::rebuild(bool animate)
{
    auto* bridge = SnapshotBridge::instance();
    const int scrub = bridge->scrubBack();
    const auto snaps = bridge->history().tail(std::size_t(samples_ + 2 + scrub));
    const std::size_t end = snaps.size() - std::min(snaps.size(), std::size_t(scrub));

    const bool gpu = source_ == QLatin1String("gpu");
    int rows = 0;
    if (end > 0) {
        const auto& s = *snaps[end - 1];
        rows = gpu ? (s.gpus.empty() ? 0 : int(s.gpus.front().engines.size())) : int(s.cores.size());
    }

    // Two spare columns: the newest sits past the right edge until its time comes.
    const int cols = samples_ + 2;
    cells_.assign(std::size_t(rows) * std::size_t(cols), 0);
    // Right-aligned so the newest sample always lands in the last column.
    const int first = cols - int(end);
    for (std::size_t i = 0; i < end; ++i) {
        const auto& s = *snaps[i];
        const int col = first + int(i);
        for (int r = 0; r < rows; ++r) {
            float v = 0;
            if (gpu) {
                if (!s.gpus.empty() && r < int(s.gpus.front().engines.size()))
                    v = s.gpus.front().engines[std::size_t(r)].pct;
            } else if (r < int(s.cores.size())) {
                v = s.cores[std::size_t(r)].total;
            }
            cells_[std::size_t(r) * std::size_t(cols) + std::size_t(col)] = quantize(v);
        }
    }
    cols_ = cols;
    cells_dirty_ = true;
    if (rows != rows_) {
        rows_ = rows;
        emit rowsChanged();
    }

    (void)animate;
    live_ = scrub == 0;
    newest_ms_ = end > 0 ? snaps[end - 1]->time_ms : 0.0;
    update();
}

float HeatmapItem::phase() const
{
    namespace tb = ring0::timebase;
    if (!live_ || newest_ms_ <= 0) return 0.0f;
    // Columns by timestamp: 0 puts the newest sample on the right edge,
    // negative values hold it off-screen until the render time reaches it.
    const double cols = ((tb::now_ms() - tb::kRenderDelayMs) - newest_ms_) / tb::kPeriodMs;
    return float(std::clamp(cols, -1.95, 0.0));
}

QSGNode* HeatmapItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    if (rows_ <= 0 || width() <= 0 || height() <= 0) {
        delete old;
        return nullptr;
    }

    auto* node = static_cast<QSGGeometryNode*>(old);
    if (!node) {
        node = new QSGGeometryNode;
        auto* geo = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
        geo->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geo);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new HeatMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }

    QSGGeometry::updateTexturedRectGeometry(node->geometry(), boundingRect(), QRectF(0, 0, 1, 1));
    node->markDirty(QSGNode::DirtyGeometry);

    auto* m = static_cast<HeatMaterial*>(node->material());
    if (cells_dirty_) {
        m->texture.setCells(cells_, QSize(cols_, rows_));
        cells_dirty_ = false;
    }
    const float p = phase();

    m->rows = float(rows_);
    m->cols = float(samples_);
    m->phase = p;
    m->row_px = float(height() / rows_);
    m->gap = float(std::max<qreal>(row_gap_, 0));
    const QColor c = hue_.toRgb();
    m->hue[0] = srgbToLinear(c.redF());
    m->hue[1] = srgbToLinear(c.greenF());
    m->hue[2] = srgbToLinear(c.blueF());
    m->hue[3] = 1.0f;
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}
