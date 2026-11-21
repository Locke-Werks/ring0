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
    if (!available_) {
        emit failed(u"the active power plan cannot be read"_s);
        return;
    }
    if (watts <= 0) {
        // Uncapped is full speed in the active plan, written permanently, whatever
        // the plan held before.
        release();
        if (const auto a = power_plan::active()) write_step(*a, 0);
        target_ = 0;
        emit changed();
        return;
    }
    if (!scheme_) remember(power_plan::active());
    target_ = watts;
    emit changed();
    tick();
}

void PowerCap::tick()
{
    if (target_ <= 0 || !scheme_) return;

    // The user switched plans while capped: the plan that was capped is left at
    // full speed, and the cap carries on against the new one.
    if (const auto a = power_plan::active(); a && *a != *scheme_) {
        write_step(*scheme_, 0);
        remember(a);
        level_ = 0;
        emit changed();
    }

    const double w = SnapshotBridge::instance()->cpuPowerW();
    if (w < 0) return;   // no power reading on this CPU: hold the current level

    int next = level_;
    if (w > target_ * 1.25) next += 2;
    else if (w > target_ * 1.03) next += 1;
    else if (w < target_ * 0.85) next -= 1;
    next = std::clamp(next, 0, kLast);
    if (next != level_ || !applied_) apply(next);
}

void PowerCap::apply(int level)
{
    if (!scheme_) return;
    if (!write_step(*scheme_, level)) {
        emit failed(u"cannot write the power plan"_s);
        return;
    }
    level_ = level;
    applied_ = true;
    emit changed();
}

bool PowerCap::write_step(const power_plan::SchemeId& scheme, int level)
{
    const Step st = kLadder[level];
    power_plan::Settings s;
    s.scheme = scheme;
    s.max_state_ac = s.max_state_dc = std::uint32_t(st.max_state);
    s.boost_ac = s.boost_dc = st.boost ? 2u : 0u;   // 2 is "aggressive", the Windows default
    return power_plan::write(s);
}

// Leaves the capped plan at full speed.
void PowerCap::release()
{
    if (scheme_) write_step(*scheme_, 0);
    scheme_.reset();
    level_ = 0;
    applied_ = false;
    QSettings().remove(u"powercap"_s);
}

void PowerCap::remember(const std::optional<power_plan::SchemeId>& scheme)
{
    scheme_ = scheme;
    applied_ = false;
    if (!scheme_) return;
    QSettings().setValue(u"powercap/scheme"_s,
        QByteArray(reinterpret_cast<const char*>(scheme_->data()), int(scheme_->size())).toHex());
}

// A previous run that died with the cap engaged: put that plan back to full speed.
// Entries from 0.5.0 carry no plan id and are dropped rather than guessed at.
void PowerCap::recover()
{
    QSettings q;
    const QByteArray id = QByteArray::fromHex(q.value(u"powercap/scheme"_s).toByteArray());
    if (id.size() == 16) {
        power_plan::SchemeId s;
        std::copy(id.begin(), id.end(), s.begin());
        write_step(s, 0);
    }
    q.remove(u"powercap"_s);
}
