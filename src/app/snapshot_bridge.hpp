// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/history.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>

// Hands the newest published snapshot to the UI thread. Polled from a frame-rate
// timer so a burst of samples never queues up signals; the UI only ever sees the
// latest one. While the scrubber holds a position, `current()` returns history
// at that offset instead.
class SnapshotBridge : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(quint64 seq READ seq NOTIFY changed)
    Q_PROPERTY(int scrubBack READ scrubBack WRITE setScrubBack NOTIFY scrubChanged)
    Q_PROPERTY(int historySize READ historySize NOTIFY changed)
    Q_PROPERTY(double cpuTotal READ cpuTotal NOTIFY changed)
    Q_PROPERTY(double memPct READ memPct NOTIFY changed)
    Q_PROPERTY(double gpuTotal READ gpuTotal NOTIFY changed)
    Q_PROPERTY(double diskBps READ diskBps NOTIFY changed)
    Q_PROPERTY(double netBps READ netBps NOTIFY changed)
    Q_PROPERTY(int coreCount READ coreCount NOTIFY changed)
    Q_PROPERTY(int processCount READ processCount NOTIFY changed)
    Q_PROPERTY(double cpuTempC READ cpuTempC NOTIFY changed)

public:
    static SnapshotBridge* instance();
    static SnapshotBridge* create(QQmlEngine*, QJSEngine*) { return instance(); }

    ring0::History& history() { return history_; }
    ring0::SnapshotPtr current() const;

    quint64 seq() const { return seq_; }
    int scrubBack() const { return scrub_back_; }
    void setScrubBack(int back);
    int historySize() const { return static_cast<int>(history_.size()); }

    double cpuTotal() const;
    double memPct() const;
    double gpuTotal() const;
    double diskBps() const;
    double netBps() const;
    int coreCount() const;
    int processCount() const;
    double cpuTempC() const;

    Q_INVOKABLE double coreLoad(int core) const;
    // Detail for the device views; plain maps so QML can bind without extra types.
    Q_INVOKABLE QVariantMap memory() const;
    Q_INVOKABLE QVariantList disks() const;
    Q_INVOKABLE QVariantList ifaces() const;
    Q_INVOKABLE QVariantList gpus() const;
    // Oldest-to-newest values of one metric for a graph. metric: "cpu", "mem",
    // "gpu", "disk", "net", "commit", "core:N", "disk:N[:r|w]", "net:N[:rx|tx]", "gpu:A[:E]".
    Q_INVOKABLE QList<double> series(const QString& metric, int n) const;

    // Same series with each sample's timestamp (timebase ms), for renderers that
    // position by time. Up to n entries, oldest first, honoring the scrub offset.
    struct TimedValue { double time_ms; double value; };
    std::vector<TimedValue> timedSeries(const QString& metric, int n) const;

signals:
    void changed();
    void scrubChanged();

private:
    SnapshotBridge();
    void poll();

    ring0::History history_;
    std::unique_ptr<ring0::SamplerThread> sampler_;
    quint64 seq_ = 0;
    int scrub_back_ = 0;
};
