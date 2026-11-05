// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/sparkline_item.hpp"
#include "app/snapshot_bridge.hpp"

#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>

#include <algorithm>

SparklineItem::SparklineItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    connect(SnapshotBridge::instance(), &SnapshotBridge::changed, this, &SparklineItem::refresh);
}

void SparklineItem::setMetric(const QString& m) { if (m != metric_) { metric_ = m; emit metricChanged(); refresh(); } }
void SparklineItem::setSamples(int n) { if (n != samples_) { samples_ = n; emit samplesChanged(); refresh(); } }
void SparklineItem::setColor(const QColor& c) { if (c != color_) { color_ = c; emit colorChanged(); update(); } }
void SparklineItem::setMaxValue(double v) { if (v != max_) { max_ = v; emit maxValueChanged(); update(); } }
void SparklineItem::setFill(bool f) { if (f != fill_) { fill_ = f; emit fillChanged(); update(); } }

void SparklineItem::refresh()
{
    if (metric_.isEmpty() || !isVisible()) return;
    values_ = SnapshotBridge::instance()->series(metric_, samples_);
    update();
}

QSGNode* SparklineItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    auto* node = static_cast<QSGGeometryNode*>(old);
    if (!node) {
        node = new QSGGeometryNode;
        auto* geo = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geo);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGFlatColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }

    const auto n = values_.size();
    auto* geo = node->geometry();
    if (n < 2 || width() <= 0 || height() <= 0) {
        geo->allocate(0);
        node->markDirty(QSGNode::DirtyGeometry);
        return node;
    }

    double top = max_;
    if (top <= 0) top = std::max(1.0, *std::ranges::max_element(values_));

    // Filled area as a strip of (baseline, value) pairs, right-aligned so the
    // newest sample sits at the right edge.
    geo->allocate(int(n) * 2);
    auto* v = geo->vertexDataAsPoint2D();
    const double step = width() / double(std::max<qsizetype>(samples_ - 1, 1));
    const double x0 = width() - step * double(n - 1);
    const float h = float(height());
    for (qsizetype i = 0; i < n; ++i) {
        const float x = float(x0 + step * double(i));
        const float y = h - float(std::clamp(values_[i] / top, 0.0, 1.0)) * h;
        v[2 * i].set(x, h);
        v[2 * i + 1].set(x, y);
    }
    node->markDirty(QSGNode::DirtyGeometry);

    auto* mat = static_cast<QSGFlatColorMaterial*>(node->material());
    QColor c = color_;
    c.setAlphaF(fill_ ? 0.55f : 1.0f);
    mat->setColor(c);
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}
