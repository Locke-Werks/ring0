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
// Uncapped means full speed: releasing the cap, or dragging to uncapped, writes
// 100% maximum processor state and boost on into the plan and leaves it there.
// Which plan is capped is saved to QSettings, so a crash is undone at the next
// launch rather than leaving that plan throttled.
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
    bool write_step(const ring0::power_plan::SchemeId& scheme, int level);
    void release();
    void remember(const std::optional<ring0::power_plan::SchemeId>& scheme);
    void recover();

    bool available_ = false;
    int target_ = 0;
    int level_ = 0;                                  // index into the ladder
    bool applied_ = false;                           // level_ has been written to scheme_
    std::optional<ring0::power_plan::SchemeId> scheme_;   // the plan being capped
};
