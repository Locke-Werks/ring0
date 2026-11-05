// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/sparkline_item.hpp"
#include "app/snapshot_bridge.hpp"

#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr int kSubdiv = 5;          // spline segments per sample interval
constexpr float kTweenMs = 250.0f;
constexpr float kDotRadius = 1.75f; // 3.5 px dot
constexpr float kGlowRadius = 6.0f;
constexpr float kGlowMax = 0.30f;   // halo alpha at a pegged metric

struct Uniforms {
    float matrix[16];
    float opacity;
    float t;
    float step;
    float half_width;
    float color[4];
    float size[2];
    float scroll;
    float fill_on;
    float glow;
    float dot_radius;
    float glow_radius;
    float pad;
};
static_assert(sizeof(Uniforms) == 128);
static_assert(offsetof(Uniforms, color) == 80);
static_assert(offsetof(Uniforms, size) == 96);
static_assert(offsetof(Uniforms, glow) == 112);

struct Vertex {
    float slot_a, slot_b, kind, side;
    float ya_prev, ya_cur, yb_prev, yb_cur;
    float end_b, cx, cy;
};
static_assert(sizeof(Vertex) == 11 * sizeof(float));

const QSGGeometry::AttributeSet& vertexAttributes()
{
    static const QSGGeometry::Attribute attrs[] = {
        QSGGeometry::Attribute::createWithAttributeType(0, 4, QSGGeometry::FloatType, QSGGeometry::PositionAttribute),
        QSGGeometry::Attribute::createWithAttributeType(1, 4, QSGGeometry::FloatType, QSGGeometry::UnknownAttribute),
        QSGGeometry::Attribute::createWithAttributeType(2, 3, QSGGeometry::FloatType, QSGGeometry::UnknownAttribute),
    };
    static const QSGGeometry::AttributeSet set = {3, int(sizeof(Vertex)), attrs};
    return set;
}

class SparklineMaterial;

class SparklineShader : public QSGMaterialShader {
public:
    SparklineShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/sparkline.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/sparkline.frag.qsb"));
    }
    bool updateUniformData(RenderState& state, QSGMaterial* next, QSGMaterial* prev) override;
};

class SparklineMaterial : public QSGMaterial {
public:
    SparklineMaterial() { setFlag(Blending, true); }

    QSGMaterialType* type() const override
    {
        static QSGMaterialType t;
        return &t;
    }
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override
    {
        return new SparklineShader;
    }
    int compare(const QSGMaterial* other) const override
    {
        // Every sparkline carries its own tween phase, so batching two of them
        // would only be correct by accident.
        return this == other ? 0 : (this < other ? -1 : 1);
    }

    Uniforms u{};
    bool dirty = true;
};

bool SparklineShader::updateUniformData(RenderState& state, QSGMaterial* next, QSGMaterial* prev)
{
    QByteArray* buf = state.uniformData();
    Q_ASSERT(buf->size() >= int(sizeof(Uniforms)));
    bool changed = false;
    // Every sparkline is its own unbatched material, so the dirty flags describe
    // the previous draw, not this one. Always write matrix and opacity.
    {
        const QMatrix4x4 m = state.combinedMatrix();
        std::memcpy(buf->data(), m.constData(), 64);
        const float o = state.opacity();
        std::memcpy(buf->data() + 64, &o, 4);
        changed = true;
    }
    auto* mat = static_cast<SparklineMaterial*>(next);
    if (next != prev || mat->dirty) {
        constexpr auto off = offsetof(Uniforms, t);
        std::memcpy(buf->data() + off, reinterpret_cast<const char*>(&mat->u) + off, sizeof(Uniforms) - off);
        mat->dirty = false;
        changed = true;
    }
    return changed;
}

// Fritsch-Carlson monotone cubic over unit-spaced knots: no overshoot, so a
// curve through values in [0, top] never dips below the baseline or above max.
std::vector<float> monotoneTangents(const std::vector<float>& y)
{
    const size_t n = y.size();
    std::vector<float> m(n, 0.0f);
    if (n < 2) return m;
    std::vector<float> d(n - 1);
    for (size_t i = 0; i + 1 < n; ++i) d[i] = y[i + 1] - y[i];
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t i = 1; i + 1 < n; ++i)
        m[i] = (d[i - 1] * d[i] <= 0.0f) ? 0.0f : 0.5f * (d[i - 1] + d[i]);
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0.0f) { m[i] = m[i + 1] = 0.0f; continue; }
        const float a = m[i] / d[i], b = m[i + 1] / d[i];
        const float s = a * a + b * b;
        if (s > 9.0f) {
            const float k = 3.0f / std::sqrt(s);
            m[i] = k * a * d[i];
            m[i + 1] = k * b * d[i];
        }
    }
    return m;
}

std::vector<float> tessellate(const std::vector<float>& y)
{
    const auto m = monotoneTangents(y);
    std::vector<float> out;
    out.reserve((y.size() - 1) * kSubdiv + 1);
    for (size_t i = 0; i + 1 < y.size(); ++i) {
        for (int s = 0; s < kSubdiv; ++s) {
            const float u = float(s) / kSubdiv, u2 = u * u, u3 = u2 * u;
            const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u;
            const float h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
            out.push_back(h00 * y[i] + h10 * m[i] + h01 * y[i + 1] + h11 * m[i + 1]);
        }
    }
    out.push_back(y.back());
    return out;
}

} // namespace

SparklineItem::SparklineItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // The incoming sample is drawn past the right edge and scrolls in.
    setClip(true);
    connect(SnapshotBridge::instance(), &SnapshotBridge::changed, this, &SparklineItem::refresh);
}

void SparklineItem::setMetric(const QString& m) { if (m != metric_) { metric_ = m; emit metricChanged(); refresh(); } }
void SparklineItem::setSamples(int n) { if (n != samples_) { samples_ = n; emit samplesChanged(); refresh(); } }
void SparklineItem::setColor(const QColor& c) { if (c != color_) { color_ = c; emit colorChanged(); update(); } }
void SparklineItem::setFill(bool f) { if (f != fill_) { fill_ = f; emit fillChanged(); update(); } }

void SparklineItem::setMaxValue(double v)
{
    if (v == max_) return;
    max_ = v;
    emit maxValueChanged();
    geometry_dirty_ = true;
    update();
}

void SparklineItem::setLineWidth(double w)
{
    if (w == line_width_) return;
    line_width_ = w;
    emit lineWidthChanged();
    update();
}

void SparklineItem::setValues(const QList<double>& v)
{
    if (v == user_values_) return;
    user_values_ = v;
    emit valuesChanged();
    if (metric_.isEmpty()) accept(user_values_.mid(std::max<qsizetype>(0, user_values_.size() - samples_)));
}

void SparklineItem::refresh()
{
    if (metric_.isEmpty()) {
        accept(user_values_.mid(std::max<qsizetype>(0, user_values_.size() - samples_)));
        return;
    }
    if (!isVisible()) return;
    accept(SnapshotBridge::instance()->series(metric_, samples_));
}

double SparklineItem::scaleTop(const QList<double>& v) const
{
    if (max_ > 0) return max_;
    return v.isEmpty() ? 1.0 : std::max(1.0, *std::ranges::max_element(v));
}

void SparklineItem::accept(QList<double> next)
{
    if (next == cur_) return;
    // A one-sample shift is the normal case and gets the leftward glide;
    // anything else (scrubbing, a metric switch) only morphs in place.
    const auto n = next.size();
    scroll_ = n >= 2 && !cur_.isEmpty() && next[n - 2] == cur_.back();
    prev_top_ = scaleTop(cur_.isEmpty() ? next : cur_);
    prev_ = cur_.isEmpty() ? next : cur_;
    cur_ = std::move(next);
    geometry_dirty_ = true;

    // A live shift scrolls at constant velocity across the whole gap between
    // samples, so the graph is always moving and never steps. The gap is
    // measured, not assumed, and smoothed so sampler jitter doesn't show as speed changes.
    if (scroll_ && interval_clock_.isValid()) {
        const float gap = float(interval_clock_.elapsed());
        if (gap > 100.0f && gap < 5000.0f)
            interval_ms_ = interval_ms_ <= 0.0f ? gap : interval_ms_ * 0.8f + gap * 0.2f;
    }
    interval_clock_.restart();
    linear_ = scroll_ && interval_ms_ > 0.0f;

    t_ = 0.0f;
    tweening_ = true;
    clock_.restart();
    update();
}

void SparklineItem::onFrame()
{
    if (!tweening_) return;
    const float dur = linear_ ? interval_ms_ : kTweenMs;
    const float raw = std::min(1.0f, float(clock_.elapsed()) / dur);
    if (linear_) {
        t_ = raw;
    } else {
        // Scrubbing or a metric switch: ease-out cubic so the morph lands softly.
        const float inv = 1.0f - raw;
        t_ = 1.0f - inv * inv * inv;
    }
    if (raw >= 1.0f) tweening_ = false;
    update();
}

void SparklineItem::attachWindow(QQuickWindow* w)
{
    if (window_ == w) return;
    disconnect(frame_conn_);
    window_ = w;
    if (w) frame_conn_ = connect(w, &QQuickWindow::afterAnimating, this, &SparklineItem::onFrame);
}

void SparklineItem::itemChange(ItemChange change, const ItemChangeData& data)
{
    if (change == ItemSceneChange) attachWindow(data.window);
    QQuickItem::itemChange(change, data);
}

void SparklineItem::geometryChange(const QRectF& now, const QRectF& before)
{
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size()) { geometry_dirty_ = true; update(); }
}

QSGNode* SparklineItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    auto* node = static_cast<QSGGeometryNode*>(old);
    if (!node) {
        node = new QSGGeometryNode;
        auto* geo = new QSGGeometry(vertexAttributes(), 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geo);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new SparklineMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
        geometry_dirty_ = true;
    }

    const auto n = cur_.size();
    const float w = float(width()), h = float(height());
    auto* geo = node->geometry();
    if (n < 2 || w <= 0 || h <= 0) {
        if (geo->vertexCount() != 0) {
            geo->allocate(0);
            node->markDirty(QSGNode::DirtyGeometry);
        }
        return node;
    }

    const double top = scaleTop(cur_);
    if (geometry_dirty_) {
        geometry_dirty_ = false;
        const auto toY = [h](double v, double t) { return h - float(std::clamp(v / t, 0.0, 1.0)) * h; };

        // Knots oldest to newest. When gliding, the sample about to fall off
        // the left edge rides along one slot further out so no gap opens.
        std::vector<float> py, cy, slot;
        const bool lead = scroll_ && prev_.size() >= n;
        if (lead) {
            py.push_back(toY(prev_[prev_.size() - n], prev_top_));
            cy.push_back(toY(cur_[0], top));
            slot.push_back(float(n));
        }
        for (qsizetype i = 0; i < n; ++i) {
            // Pair each knot with the same data point in the old series, and
            // the newest with the old newest so it grows out of it.
            qsizetype j = scroll_ ? prev_.size() - n + 1 + i : prev_.size() - n + i;
            j = std::clamp<qsizetype>(j, 0, prev_.size() - 1);
            py.push_back(toY(prev_[j], prev_top_));
            cy.push_back(toY(cur_[i], top));
            slot.push_back(float(n - 1 - i));
        }

        const auto tp = tessellate(py);
        const auto tc = tessellate(cy);
        const size_t pts = tc.size();
        const auto slotAt = [&](size_t k) {
            const size_t knot = k / kSubdiv;
            const float frac = float(k % kSubdiv) / kSubdiv;
            return knot + 1 < slot.size() ? slot[knot] - frac : slot.back();
        };

        const size_t segs = pts - 1;
        geo->allocate(int(segs * 18 + 6));
        auto* v = static_cast<Vertex*>(geo->vertexData());
        size_t o = 0;
        const auto quad = [&](size_t k, float kind, float lo, float hi) {
            const float sa = slotAt(k), sb = slotAt(k + 1);
            const auto put = [&](float side, float end) {
                v[o++] = {sa, sb, kind, side, tp[k], tc[k], tp[k + 1], tc[k + 1], end, 0.0f, 0.0f};
            };
            put(lo, 0); put(hi, 0); put(hi, 1);
            put(lo, 0); put(hi, 1); put(lo, 1);
        };
        // Draw order inside one buffer is the layer order: fill, halo, stroke.
        for (size_t k = 0; k < segs; ++k) quad(k, 0.0f, 0.0f, 1.0f);
        for (size_t k = 0; k < segs; ++k) quad(k, 2.0f, -1.0f, 1.0f);
        for (size_t k = 0; k < segs; ++k) quad(k, 1.0f, -1.0f, 1.0f);

        const size_t last = pts - 1;
        const auto dot = [&](float x, float y) {
            v[o++] = {slotAt(last), slotAt(last), 3.0f, 0.0f, tp[last], tc[last], tp[last], tc[last], 1.0f, x, y};
        };
        dot(-1, -1); dot(1, -1); dot(1, 1);
        dot(-1, -1); dot(1, 1); dot(-1, 1);
        node->markDirty(QSGNode::DirtyGeometry);
    }

    auto* mat = static_cast<SparklineMaterial*>(node->material());
    auto& u = mat->u;
    u.t = t_;
    u.step = w / float(std::max(samples_ - 1, 1));
    u.half_width = float(line_width_) * 0.5f;
    const float c[4] = {color_.redF(), color_.greenF(), color_.blueF(), color_.alphaF()};
    std::copy(std::begin(c), std::end(c), u.color);
    u.size[0] = w;
    u.size[1] = h;
    u.scroll = scroll_ ? 1.0f : 0.0f;
    u.fill_on = fill_ ? 1.0f : 0.0f;
    // The halo tracks how hot the newest sample is; idle metrics show none.
    u.glow = kGlowMax * float(std::clamp(cur_.back() / top, 0.0, 1.0));
    u.dot_radius = kDotRadius;
    u.glow_radius = kGlowRadius;
    mat->dirty = true;
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}
