// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/power_plan.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>

#include <optional>

// Holds CPU package power near a target by stepping the power plan's boost mode
// and maximum processor state, once a second, against the measured package
// watts. It is a feedback loop on clocks, not a firmware limit, so a sudden load
// can overshoot for a second before the next step lands.
//
// The original plan values are saved to QSettings before the first change, so a
// crash is undone at the next launch rather than leaving the plan capped.
class PowerCap : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int target READ target NOTIFY changed)          // watts, 0 when uncapped
    Q_PROPERTY(int maxState READ maxState NOTIFY changed)      // percent currently applied
    Q_PROPERTY(bool boost READ boost NOTIFY changed)
    Q_PROPERTY(bool available READ available CONSTANT)
    // True at the last step: the plan offers nothing lower, so a target the CPU
    // still exceeds here is out of reach for this method.
    Q_PROPERTY(bool atFloor READ atFloor NOTIFY changed)

public:
    explicit PowerCap(QObject* parent = nullptr);
    ~PowerCap() override;

    int target() const { return target_; }
    int maxState() const;
    bool boost() const;
    bool available() const { return available_; }
    bool atFloor() const;

    // 0 releases the cap and restores the plan.
    Q_INVOKABLE void setTarget(int watts);

signals:
    void changed();
    void failed(const QString& message);

private:
    void tick();
    void apply(int level);
    void release();
    void save_original();
    void recover();

    bool available_ = false;
    int target_ = 0;
    int level_ = 0;                                  // index into the ladder
    std::optional<ring0::power_plan::Settings> orig_;
};
