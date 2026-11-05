// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

// History graph drawn entirely in the scene graph. Pulls its series from
// SnapshotBridge each time a new sample lands, uploads it as vertex data, and
// tweens between the previous and current series on the GPU.
class SparklineItem : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(Sparkline)
    Q_PROPERTY(QString metric READ metric WRITE setMetric NOTIFY metricChanged)
    Q_PROPERTY(int samples READ samples WRITE setSamples NOTIFY samplesChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(double maxValue READ maxValue WRITE setMaxValue NOTIFY maxValueChanged) // <= 0: auto-scale
    Q_PROPERTY(bool fill READ fill WRITE setFill NOTIFY fillChanged)

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

signals:
    void metricChanged();
    void samplesChanged();
    void colorChanged();
    void maxValueChanged();
    void fillChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;

private:
    void refresh();

    QString metric_;
    int samples_ = 60;
    QColor color_ = QColor(0xd6, 0x26, 0x2a);
    double max_ = 100.0;
    bool fill_ = true;
    QList<double> values_;
};
