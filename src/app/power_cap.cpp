// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/power_cap.hpp"

#include "app/snapshot_bridge.hpp"

#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <array>

using namespace ring0;
using namespace Qt::StringLiterals;

namespace {

// Level 0 is the plan as found. Level 1 turns boost off, which on its own takes
// the largest bite out of package power; each later level lowers the maximum
// processor state.
struct Step { int max_state; bool boost; };
constexpr std::array<Step, 16> kLadder = {{
    {100, true}, {100, false}, {95, false}, {90, false}, {85, false}, {80, false},
    {75, false}, {70, false}, {65, false}, {60, false}, {55, false}, {50, false},
    {45, false}, {40, false}, {35, false}, {30, false},
}};
constexpr int kLast = int(kLadder.size()) - 1;

} // namespace

PowerCap::PowerCap(QObject* parent) : QObject(parent)
{
    available_ = power_plan::read().has_value();
    recover();
    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, &PowerCap::tick);
    t->start(1000);
}

PowerCap::~PowerCap()
{
    release();
}

int PowerCap::maxState() const { return kLadder[level_].max_state; }
bool PowerCap::boost() const { return kLadder[level_].boost; }
bool PowerCap::atFloor() const { return level_ == kLast; }

void PowerCap::setTarget(int watts)
{
    if (watts <= 0) {
        release();
        target_ = 0;
        emit changed();
        return;
    }
    if (!available_) {
        emit failed(u"the active power plan cannot be read"_s);
        return;
    }
    if (!orig_) save_original();
    target_ = watts;
    emit changed();
    tick();
}

void PowerCap::tick()
{
    if (target_ <= 0) return;
    const double w = SnapshotBridge::instance()->cpuPowerW();
    if (w < 0) return;   // no power reading on this CPU: hold the current level

    int next = level_;
    if (w > target_ * 1.25) next += 2;
    else if (w > target_ * 1.03) next += 1;
    else if (w < target_ * 0.85) next -= 1;
    next = std::clamp(next, 0, kLast);
    if (next != level_) apply(next);
}

void PowerCap::apply(int level)
{
    if (!orig_) return;
    power_plan::Settings s = *orig_;
    const Step st = kLadder[level];
    if (level > 0) {
        s.max_state_ac = std::min<std::uint32_t>(s.max_state_ac, st.max_state);
        s.max_state_dc = std::min<std::uint32_t>(s.max_state_dc, st.max_state);
        if (!st.boost) s.boost_ac = s.boost_dc = 0;
    }
    if (!power_plan::write(s)) {
        emit failed(u"cannot write the power plan"_s);
        return;
    }
    level_ = level;
    emit changed();
}

void PowerCap::release()
{
    if (!orig_) return;
    power_plan::write(*orig_);
    orig_.reset();
    level_ = 0;
    QSettings().remove(u"powercap"_s);
}

void PowerCap::save_original()
{
    orig_ = power_plan::read();
    if (!orig_) return;
    QSettings q;
    q.beginGroup(u"powercap"_s);
    q.setValue(u"max_ac"_s, orig_->max_state_ac);
    q.setValue(u"max_dc"_s, orig_->max_state_dc);
    q.setValue(u"boost_ac"_s, orig_->boost_ac);
    q.setValue(u"boost_dc"_s, orig_->boost_dc);
    q.endGroup();
}

// A previous run that died with the cap engaged left its originals behind.
void PowerCap::recover()
{
    QSettings q;
    if (!q.contains(u"powercap/max_ac"_s)) return;
    power_plan::Settings s;
    s.max_state_ac = q.value(u"powercap/max_ac"_s).toUInt();
    s.max_state_dc = q.value(u"powercap/max_dc"_s).toUInt();
    s.boost_ac = q.value(u"powercap/boost_ac"_s).toUInt();
    s.boost_dc = q.value(u"powercap/boost_dc"_s).toUInt();
    power_plan::write(s);
    q.remove(u"powercap"_s);
}
