// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <vector>

// Rows x time grid (one row per core or per GPU engine) drawn as a single quad
// sampling an R8 texture, so the cost does not grow with the cell count. Each
// new sample slides the grid left by one column over a short ease rather than
// jumping.
class HeatmapItem : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(Heatmap)
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(int samples READ samples WRITE setSamples NOTIFY samplesChanged)
    Q_PROPERTY(QColor hue READ hue WRITE setHue NOTIFY hueChanged)
    Q_PROPERTY(qreal rowGap READ rowGap WRITE setRowGap NOTIFY rowGapChanged)
    Q_PROPERTY(int rows READ rows NOTIFY rowsChanged)

public:
    explicit HeatmapItem(QQuickItem* parent = nullptr);

    QString source() const { return source_; }
    void setSource(const QString& s);
    int samples() const { return samples_; }
    void setSamples(int n);
    QColor hue() const { return hue_; }
    void setHue(const QColor& c);
    qreal rowGap() const { return row_gap_; }
    void setRowGap(qreal g);
    int rows() const { return rows_; }

signals:
    void sourceChanged();
    void samplesChanged();
    void hueChanged();
    void rowGapChanged();
    void rowsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    void refresh();
    void rebuild(bool animate);
    float phase() const;

    QString source_ = QStringLiteral("cores");
    int samples_ = 120;
    QColor hue_ = QColor(0xd6, 0x26, 0x2a);
    qreal row_gap_ = 1.0;

    int rows_ = 0;
    int cols_ = 0;                    // texture width, samples_ + 2
    std::vector<std::uint8_t> cells_; // row-major, rows_ x cols_
    bool cells_dirty_ = false;
    double newest_ms_ = 0;            // timestamp of the newest column
    bool live_ = false;               // following the clock (not scrubbed)
    QMetaObject::Connection frame_conn_;
};
