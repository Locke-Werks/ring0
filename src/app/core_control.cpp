// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/core_control.hpp"

#include <QTimer>

using namespace Qt::StringLiterals;

CoreControl::CoreControl(QObject* parent) : QObject(parent)
{
    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, [this] {
        if (!evac_.any_excluded()) return;
        evac_.sweep();
        const auto s = evac_.stats();
        if (s.moved != stats_.moved || s.refused != stats_.refused) refresh();
    });
    t->start(1000);
}

QVariantList CoreControl::evacuated() const
{
    QVariantList out;
    const auto& ex = evac_.excluded();
    for (std::size_t i = 0; i < ex.size(); ++i)
        if (ex[i]) out.append(int(i));
    return out;
}

bool CoreControl::evacuate(const QVariantList& cores)
{
    auto next = evac_.excluded();
    for (const auto& v : cores) {
        const int i = v.toInt();
        if (i >= 0 && std::size_t(i) < next.size()) next[i] = true;
    }
    return commit(std::move(next));
}

bool CoreControl::restore(const QVariantList& cores)
{
    auto next = evac_.excluded();
    for (const auto& v : cores) {
        const int i = v.toInt();
        if (i >= 0 && std::size_t(i) < next.size()) next[i] = false;
    }
    return commit(std::move(next));
}

void CoreControl::restoreAll()
{
    evac_.restore_all();
    refresh();
}

bool CoreControl::commit(std::vector<bool> next)
{
    if (!evac_.set_excluded(next)) {
        emit failed(u"at least one core has to stay available"_s);
        return false;
    }
    refresh();
    return true;
}

void CoreControl::refresh()
{
    stats_ = evac_.stats();
    emit changed();
}
