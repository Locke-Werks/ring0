// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/sparkline_item.hpp"
#include "app/snapshot_bridge.hpp"
#include "core/timebase.hpp"

#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace tb = ring0::timebase;

namespace {

constexpr int kSubdiv = 6;          // spline segments per sample interval
constexpr float kDotRadius = 1.75f; // 3.5 px dot
constexpr float kGlowRadius = 6.0f;
constexpr float kGlowMax = 0.30f;   // halo alpha at a pegged metric
constexpr double kScaleTauMs = 220; // time constant for the auto-scale glide

struct Uniforms {
    float matrix[16];
    float opacity;
    float age;          // ms the render time sits past the newest knot
    float px_per_ms;
    float half_width;
    float color[4];
    float size[2];
    float top;          // value mapped to the top edge
    float fill_on;
    float glow;
    float dot_radius;
    float glow_radius;
    float edge_value;   // curve value at the right edge, where the dot sits
};
static_assert(sizeof(Uniforms) == 128);
static_assert(offsetof(Uniforms, color) == 80);
static_assert(offsetof(Uniforms, size) == 96);
static_assert(offsetof(Uniforms, glow) == 112);

// a0: time A, time B (ms relative to the newest knot, <= 0), kind, side
// a1: value A, value B, endpoint (0 A, 1 B), unused
// a2: dot corner x, dot corner y
struct Vertex {
    float ta, tb, kind, side;
    float va, vb, end_b, unused;
    float cx, cy;
};
static_assert(sizeof(Vertex) == 10 * sizeof(float));

const QSGGeometry::AttributeSet& vertexAttributes()
{
    static const QSGGeometry::Attribute attrs[] = {
        QSGGeometry::Attribute::createWithAttributeType(0, 4, QSGGeometry::FloatType, QSGGeometry::PositionAttribute),
        QSGGeometry::Attribute::createWithAttributeType(1, 4, QSGGeometry::FloatType, QSGGeometry::UnknownAttribute),
        QSGGeometry::Attribute::createWithAttributeType(2, 2, QSGGeometry::FloatType, QSGGeometry::UnknownAttribute),
    };
    static const QSGGeometry::AttributeSet set = {3, int(sizeof(Vertex)), attrs};
    return set;
}

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
        // Every sparkline carries its own clock state; batching two would mix them.
        return this == other ? 0 : (this < other ? -1 : 1);
    }

    Uniforms u{};
};

bool SparklineShader::updateUniformData(RenderState& state, QSGMaterial* next, QSGMaterial*)
{
    QByteArray* buf = state.uniformData();
    Q_ASSERT(buf->size() >= int(sizeof(Uniforms)));
    auto* mat = static_cast<SparklineMaterial*>(next);
    // Unbatched material: the dirty flags may describe another node's draw, so
    // write the whole block every time. It is 128 bytes.
    const QMatrix4x4 m = state.combinedMatrix();
    std::memcpy(mat->u.matrix, m.constData(), 64);
    mat->u.opacity = state.opacity();
    std::memcpy(buf->data(), &mat->u, sizeof(Uniforms));
    return true;
}

// Fritsch-Carlson monotone cubic over non-uniform knots: no overshoot, so a
// curve through values in [0, top] never dips below the baseline or above max.
std::vector<double> monotoneTangents(const std::vector<double>& x, const std::vector<double>& y)
{
    const std::size_t n = y.size();
    std::vector<double> m(n, 0.0);
    if (n < 2) return m;
    std::vector<double> h(n - 1), d(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        h[i] = std::max(1e-6, x[i + 1] - x[i]);
        d[i] = (y[i + 1] - y[i]) / h[i];
    }
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i) {
        if (d[i - 1] * d[i] <= 0) { m[i] = 0; continue; }
        // Weighted harmonic mean keeps the shape monotone with uneven spacing.
        const double w1 = 2 * h[i] + h[i - 1], w2 = h[i] + 2 * h[i - 1];
        m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
    }
    return m;
}

} // namespace

SparklineItem::SparklineItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // The newest samples sit past the right edge until their time comes.
    setClip(true);
    connect(SnapshotBridge::instance(), &SnapshotBridge::changed, this, &SparklineItem::refresh);
}

void SparklineItem::setMetric(const QString& m) { if (m != metric_) { metric_ = m; emit metricChanged(); refresh(); } }
void SparklineItem::setSamples(int n) { if (n != samples_) { samples_ = n; emit samplesChanged(); refresh(); } }
void SparklineItem::setColor(const QColor& c) { if (c != color_) { color_ = c; emit colorChanged(); update(); } }
void SparklineItem::setFill(bool f) { if (f != fill_) { fill_ = f; emit fillChanged(); update(); } }
void SparklineItem::setMaxValue(double v) { if (v != max_) { max_ = v; emit maxValueChanged(); update(); } }
void SparklineItem::setMinScale(double v) { if (v != min_scale_) { min_scale_ = v; emit minScaleChanged(); update(); } }
void SparklineItem::setLineWidth(double w) { if (w != line_width_) { line_width_ = w; emit lineWidthChanged(); update(); } }

void SparklineItem::setValues(const QList<double>& v)
{
    if (v == user_values_) return;
    user_values_ = v;
    emit valuesChanged();
    if (metric_.isEmpty()) refresh();
}

void SparklineItem::refresh()
{
    std::vector<Knot> next;
    auto* bridge = SnapshotBridge::instance();
    if (metric_.isEmpty()) {
        // Plain values carry no time; space them one period apart, frozen.
        const auto from = std::max<qsizetype>(0, user_values_.size() - samples_);
        for (qsizetype i = from; i < user_values_.size(); ++i)
            next.push_back({double(i - user_values_.size() + 1) * tb::kPeriodMs, user_values_[i]});
        live_ = false;
    } else {
        if (!isVisible()) return;
        // Two extra knots so the curve stays continuous past the left edge while scrolling.
        for (const auto& tv : bridge->timedSeries(metric_, samples_ + 2))
            next.push_back({tv.time_ms, tv.value});
        live_ = bridge->scrubBack() == 0;
    }
    knots_ = std::move(next);
    geometry_dirty_ = true;
    {
        double peak = std::max(1.0, min_scale_);
        for (const auto& k : knots_) peak = std::max(peak, k.value * 1.08);
        if (peak != data_peak_) { data_peak_ = peak; emit dataPeakChanged(); }
    }
    if (top_ <= 0) top_ = targetTop();
    update();
}

double SparklineItem::targetTop() const
{
    if (max_ > 0) return max_;
    double peak = std::max(1.0, min_scale_);
    for (const auto& k : knots_) peak = std::max(peak, k.value);
    // A little headroom keeps the line off the top edge where the glow would clip.
    return peak * 1.08;
}

void SparklineItem::onFrame()
{
    if (!isVisible() || knots_.empty()) return;
    const double now = tb::now_ms();
    const double dt = last_frame_ms_ > 0 ? now - last_frame_ms_ : 16.0;
    last_frame_ms_ = now;

    age_ms_ = live_ ? (now - tb::kRenderDelayMs) - knots_.back().time : 0.0;

    // Ease the scale toward its target with a time-based exponential, so a new
    // peak widens the graph over a few frames instead of in one jump.
    const double target = targetTop();
    const double k = 1.0 - std::exp(-std::max(0.0, dt) / kScaleTauMs);
    top_ = top_ <= 0 ? target : top_ + (target - top_) * k;
    // The label only needs to move when the change would show in its digits.
    if (std::abs(top_ - reported_top_) > reported_top_ * 0.01) { reported_top_ = top_; emit scaleTopChanged(); }

    // Live graphs and a scale still settling both need the next frame.
    if (live_ || std::abs(target - top_) > target * 1e-4) update();
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
    if (change == ItemVisibleHasChanged && data.boolValue) refresh();
    QQuickItem::itemChange(change, data);
}

void SparklineItem::geometryChange(const QRectF& now, const QRectF& before)
{
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size()) update();
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

    const auto n = knots_.size();
    const float w = float(width()), h = float(height());
    auto* geo = node->geometry();
    if (n < 2 || w <= 0 || h <= 0) {
        if (geo->vertexCount() != 0) {
            geo->allocate(0);
            node->markDirty(QSGNode::DirtyGeometry);
        }
        return node;
    }

    if (geometry_dirty_) {
        geometry_dirty_ = false;
        const double t0 = knots_.back().time;
        std::vector<double> xs(n), ys(n);
        for (std::size_t i = 0; i < n; ++i) { xs[i] = knots_[i].time - t0; ys[i] = knots_[i].value; }
        const auto m = monotoneTangents(xs, ys);

        // Tessellate once in time; the shader maps time to x every frame.
        std::vector<float> pt, pv;
        pt.reserve((n - 1) * kSubdiv + 1);
        pv.reserve((n - 1) * kSubdiv + 1);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const double hh = xs[i + 1] - xs[i];
            for (int s = 0; s < kSubdiv; ++s) {
                const double u = double(s) / kSubdiv, u2 = u * u, u3 = u2 * u;
                const double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u;
                const double h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
                pt.push_back(float(xs[i] + u * hh));
                pv.push_back(float(std::max(0.0, h00 * ys[i] + h10 * hh * m[i] + h01 * ys[i + 1] + h11 * hh * m[i + 1])));
            }
        }
        pt.push_back(float(xs.back()));
        pv.push_back(float(ys.back()));
        tess_t_ = pt;
        tess_v_ = pv;

        const std::size_t segs = pt.size() - 1;
        geo->allocate(int(segs * 18 + 6));
        auto* v = static_cast<Vertex*>(geo->vertexData());
        std::size_t o = 0;
        const auto quad = [&](std::size_t k, float kind, float lo, float hi) {
            const auto put = [&](float side, float end) {
                v[o++] = {pt[k], pt[k + 1], kind, side, pv[k], pv[k + 1], end, 0.0f, 0.0f, 0.0f};
            };
            put(lo, 0); put(hi, 0); put(hi, 1);
            put(lo, 0); put(hi, 1); put(lo, 1);
        };
        // Draw order inside one buffer is the layer order: fill, halo, stroke.
        for (std::size_t k = 0; k < segs; ++k) quad(k, 0.0f, 0.0f, 1.0f);
        for (std::size_t k = 0; k < segs; ++k) quad(k, 2.0f, -1.0f, 1.0f);
        for (std::size_t k = 0; k < segs; ++k) quad(k, 1.0f, -1.0f, 1.0f);

        // The dot rides the line where it meets the right edge; its height comes
        // from the edge_value uniform each frame, so it only needs corners here.
        const std::size_t last = segs - 1;
        const auto dot = [&](float x, float y) {
            v[o++] = {pt[last], pt[last + 1], 3.0f, 0.0f, pv[last], pv[last + 1], 1.0f, 0.0f, x, y};
        };
        dot(-1, -1); dot(1, -1); dot(1, 1);
        dot(-1, -1); dot(1, 1); dot(-1, 1);
        node->markDirty(QSGNode::DirtyGeometry);
    }

    auto* mat = static_cast<SparklineMaterial*>(node->material());
    auto& u = mat->u;
    const double top = top_ > 0 ? top_ : targetTop();
    u.age = float(age_ms_);
    u.px_per_ms = w / float(std::max(samples_ - 1, 1) * tb::kPeriodMs);
    u.half_width = float(line_width_) * 0.5f;
    const float c[4] = {color_.redF(), color_.greenF(), color_.blueF(), color_.alphaF()};
    std::copy(std::begin(c), std::end(c), u.color);
    u.size[0] = w;
    u.size[1] = h;
    u.top = float(top);
    u.fill_on = fill_ ? 1.0f : 0.0f;
    // The halo tracks how hot the metric is at the leading edge; idle shows none.
    const float edge = edgeValue();
    u.edge_value = edge;
    u.glow = kGlowMax * float(std::clamp(double(edge) / top, 0.0, 1.0));
    u.dot_radius = kDotRadius;
    u.glow_radius = kGlowRadius;
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}

float SparklineItem::edgeValue() const
{
    if (tess_t_.empty()) return 0.0f;
    const float at = float(age_ms_);
    if (at >= tess_t_.back()) return tess_v_.back();
    if (at <= tess_t_.front()) return tess_v_.front();
    const auto it = std::upper_bound(tess_t_.begin(), tess_t_.end(), at);
    const auto i = std::size_t(it - tess_t_.begin());
    const float span = tess_t_[i] - tess_t_[i - 1];
    const float f = span > 0 ? (at - tess_t_[i - 1]) / span : 0.0f;
    return tess_v_[i - 1] + (tess_v_[i] - tess_v_[i - 1]) * f;
}
