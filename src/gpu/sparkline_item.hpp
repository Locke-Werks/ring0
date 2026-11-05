// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

#include <vector>

class QQuickWindow;

// History graph drawn entirely in the scene graph. Each sample is placed by its
// own timestamp, and every frame the whole graph is drawn at "now minus a fixed
// delay", so it moves at one constant speed regardless of when samples are
// delivered. Data changes rebuild the vertex buffer; frames only update uniforms.
class SparklineItem : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(Sparkline)
    Q_PROPERTY(QString metric READ metric WRITE setMetric NOTIFY metricChanged)
    Q_PROPERTY(int samples READ samples WRITE setSamples NOTIFY samplesChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(double maxValue READ maxValue WRITE setMaxValue NOTIFY maxValueChanged) // <= 0: auto-scale
    Q_PROPERTY(bool fill READ fill WRITE setFill NOTIFY fillChanged)
    Q_PROPERTY(QList<double> values READ values WRITE setValues NOTIFY valuesChanged)
    Q_PROPERTY(double lineWidth READ lineWidth WRITE setLineWidth NOTIFY lineWidthChanged)
    // Auto-scale never zooms in past this, so trickle traffic stays near the baseline.
    Q_PROPERTY(double minScale READ minScale WRITE setMinScale NOTIFY minScaleChanged)
    // The scale currently applied (eased), for an on-graph reference label.
    Q_PROPERTY(double scaleTop READ scaleTop NOTIFY scaleTopChanged)
    // Peak of this series' own visible data (with headroom and the minScale floor),
    // regardless of maxValue. Lets two graphs agree on one shared scale.
    Q_PROPERTY(double dataPeak READ dataPeak NOTIFY dataPeakChanged)

public:
    explicit SparklineItem(QQuickItem* parent = nullptr);

    QString metric() const { return metric_; }
    void setMetric(const QString& m);
    int samples() const { return samples_; }
    void setSamples(int n);
    QColor color() const { return color_; }
    void setColor(const QColor& c);
    double maxValue() const { return max_; }
    void setMaxValue(double v);
    bool fill() const { return fill_; }
    void setFill(bool f);
    QList<double> values() const { return user_values_; }
    void setValues(const QList<double>& v);
    double lineWidth() const { return line_width_; }
    void setLineWidth(double w);
    double minScale() const { return min_scale_; }
    void setMinScale(double v);
    double scaleTop() const { return top_; }
    double dataPeak() const { return data_peak_; }

signals:
    void metricChanged();
    void samplesChanged();
    void colorChanged();
    void maxValueChanged();
    void fillChanged();
    void valuesChanged();
    void lineWidthChanged();
    void minScaleChanged();
    void scaleTopChanged();
    void dataPeakChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    struct Knot { double time; double value; };

    void refresh();
    void onFrame();
    void attachWindow(QQuickWindow* w);
    double targetTop() const;

    QString metric_;
    int samples_ = 60;
    QColor color_ = QColor(0xd6, 0x26, 0x2a);
    double max_ = 100.0;
    bool fill_ = true;
    double line_width_ = 1.5;
    double min_scale_ = 1.0;
    double reported_top_ = 0;
    double data_peak_ = 0;
    QList<double> user_values_;

    std::vector<Knot> knots_;     // oldest first; times in timebase ms
    std::vector<float> tess_t_, tess_v_;  // tessellated curve, times relative to the newest knot
    float edgeValue() const;      // the curve's value where it meets the right edge
    bool live_ = false;           // following the clock rather than frozen (scrub, static values)
    bool geometry_dirty_ = true;

    // Per-frame state, written on the GUI thread and read during sync.
    double age_ms_ = 0;           // render time minus newest knot time
    double top_ = 0;              // displayed scale, eased toward the target
    double last_frame_ms_ = 0;

    QPointer<QQuickWindow> window_;
    QMetaObject::Connection frame_conn_;
};
