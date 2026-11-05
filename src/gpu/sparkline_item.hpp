// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

class QQuickWindow;

// History graph drawn entirely in the scene graph. Pulls its series from
// SnapshotBridge (or the `values` list when `metric` is empty), uploads the
// previous and current series as vertex data, and tweens between them on the
// GPU while the whole graph glides one sample to the left.
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

signals:
    void metricChanged();
    void samplesChanged();
    void colorChanged();
    void maxValueChanged();
    void fillChanged();
    void valuesChanged();
    void lineWidthChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    void refresh();
    void accept(QList<double> next);
    void onFrame();
    void attachWindow(QQuickWindow* w);
    double scaleTop(const QList<double>& v) const;

    QString metric_;
    int samples_ = 60;
    QColor color_ = QColor(0xd6, 0x26, 0x2a);
    double max_ = 100.0;
    bool fill_ = true;
    double line_width_ = 1.5;
    QList<double> user_values_;

    QList<double> prev_;
    QList<double> cur_;
    double prev_top_ = 1.0;
    bool scroll_ = false;
    bool geometry_dirty_ = true;

    float t_ = 1.0f;
    bool tweening_ = false;
    QElapsedTimer clock_;
    QElapsedTimer interval_clock_;
    float interval_ms_ = 0.0f;   // smoothed gap between live samples
    bool linear_ = false;
    QPointer<QQuickWindow> window_;
    QMetaObject::Connection frame_conn_;
};
