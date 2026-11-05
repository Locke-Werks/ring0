// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/process_actions.hpp"

#include "core/actions.hpp"
#include "core/inspect.hpp"
#include "core/stacks.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

using namespace ring0;
using namespace Qt::StringLiterals;

ProcessActions::ProcessActions(QObject* parent) : QObject(parent)
{
    // TrustCache has no completion callback; poll the pending set at a slow
    // cadence and announce only when something actually resolved.
    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, [this] {
        if (pending_.isEmpty()) return;
        bool any = false;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (trust_.lookup(it->toStdWString()).state != TrustState::Unknown) {
                it = pending_.erase(it);
                any = true;
            } else {
                ++it;
            }
        }
        if (any) emit trustChanged();
    });
    t->start(250);
}

bool ProcessActions::report(const QString& action, int pid, const std::expected<void, std::wstring>& r)
{
    if (r) {
        emit succeeded(action, pid);
        return true;
    }
    emit failed(action, pid, QString::fromStdWString(r.error()).trimmed());
    return false;
}

bool ProcessActions::suspend(int pid) { return report(u"suspend"_s, pid, suspend_process(std::uint32_t(pid))); }
bool ProcessActions::resume(int pid) { return report(u"resume"_s, pid, resume_process(std::uint32_t(pid))); }
bool ProcessActions::terminate(int pid) { return report(u"end"_s, pid, terminate_process(std::uint32_t(pid))); }
bool ProcessActions::singleCore(int pid) { return report(u"single core"_s, pid, restrict_to_single_core(std::uint32_t(pid))); }

bool ProcessActions::setPriority(int pid, int cls)
{
    static constexpr PriorityClass map[] = {
        PriorityClass::Idle, PriorityClass::BelowNormal, PriorityClass::Normal,
        PriorityClass::AboveNormal, PriorityClass::High, PriorityClass::Realtime};
    if (cls < 0 || cls > 5) return false;
    return report(u"priority"_s, pid, set_priority(std::uint32_t(pid), map[cls]));
}

int ProcessActions::priority(int pid) const
{
    const auto p = get_priority(std::uint32_t(pid));
    if (!p) return -1;
    switch (*p) {
    case PriorityClass::Idle: return 0;
    case PriorityClass::BelowNormal: return 1;
    case PriorityClass::Normal: return 2;
    case PriorityClass::AboveNormal: return 3;
    case PriorityClass::High: return 4;
    case PriorityClass::Realtime: return 5;
    }
    return -1;
}

bool ProcessActions::openLocation(const QString& path)
{
    return report(u"open location"_s, 0, open_file_location(path.toStdWString()));
}

void ProcessActions::copyText(const QString& text)
{
    QGuiApplication::clipboard()->setText(text);
}

QString ProcessActions::trust(const QString& path)
{
    if (path.isEmpty()) return {};
    const auto info = trust_.lookup(path.toStdWString());
    switch (info.state) {
    case TrustState::Signed: return u"signed"_s;
    case TrustState::Unsigned: return u"unsigned"_s;
    case TrustState::Invalid: return u"invalid"_s;
    case TrustState::Unknown: break;
    }
    if (!pending_.contains(path)) {
        pending_.insert(path);
        trust_.request(path.toStdWString());
    }
    return {};
}

QString ProcessActions::signer(const QString& path) const
{
    return QString::fromStdWString(trust_.lookup(path.toStdWString()).signer);
}

void ProcessActions::captureStacks(int pid)
{
    QPointer<ProcessActions> self(this);
    (void)QtConcurrent::run([self, pid] {
        auto r = capture_stacks(std::uint32_t(pid));
        QVariantList threads;
        QString error;
        if (r) {
            for (const auto& t : *r) {
                QVariantList frames;
                for (const auto& f : t.frames)
                    frames.push_back(QVariantMap{
                        {u"module"_s, QString::fromStdWString(f.module)},
                        {u"symbol"_s, QString::fromStdWString(f.symbol)},
                        {u"offset"_s, double(f.offset)},
                        {u"address"_s, u"0x"_s + QString::number(f.address, 16)}});
                threads.push_back(QVariantMap{{u"tid"_s, int(t.tid)}, {u"frames"_s, frames}});
            }
        } else {
            error = QString::fromStdWString(r.error()).trimmed();
        }
        QMetaObject::invokeMethod(qApp, [self, pid, threads, error] {
            if (!self) return;
            if (error.isEmpty()) emit self->stacksReady(pid, threads);
            else emit self->stacksFailed(pid, error);
        });
    });
}

namespace {

QString trust_word(ring0::TrustState t)
{
    switch (t) {
    case ring0::TrustState::Signed: return u"signed"_s;
    case ring0::TrustState::Unsigned: return u"unsigned"_s;
    case ring0::TrustState::Invalid: return u"invalid"_s;
    case ring0::TrustState::Unknown: break;
    }
    return {};
}

QVariantMap to_map(const ring0::ProcessDetail& d)
{
    const auto q = [](const std::wstring& w) { return QString::fromStdWString(w); };
    QVariantList ancestry, modules, conns, findings;
    for (auto p : d.ancestry) ancestry.push_back(int(p));
    for (const auto& m : d.modules)
        modules.push_back(QVariantMap{
            {u"path"_s, q(m.path)},
            {u"base"_s, u"0x"_s + QString::number(m.base, 16)},
            {u"size"_s, double(m.size)},
            {u"trust"_s, trust_word(m.trust.state)},
            {u"signer"_s, q(m.trust.signer)},
            {u"userDir"_s, m.user_writable_dir}});
    for (const auto& c : d.connections)
        conns.push_back(QVariantMap{{u"proto"_s, q(c.proto)}, {u"local"_s, q(c.local)}, {u"remote"_s, q(c.remote)}, {u"state"_s, q(c.state)}});
    for (const auto& f : d.findings) findings.push_back(q(f));
    return QVariantMap{
        {u"pid"_s, int(d.pid)}, {u"ppid"_s, int(d.ppid)},
        {u"path"_s, q(d.image_path)}, {u"commandLine"_s, q(d.command_line)},
        {u"user"_s, q(d.user)}, {u"integrity"_s, q(d.integrity)},
        {u"elevated"_s, d.elevated}, {u"arch"_s, q(d.arch)},
        {u"createTime"_s, double(d.create_time)},
        {u"trust"_s, trust_word(d.image_trust.state)}, {u"signer"_s, q(d.image_trust.signer)},
        {u"ancestry"_s, ancestry}, {u"modules"_s, modules}, {u"connections"_s, conns},
        {u"signals"_s, findings}, {u"error"_s, q(d.error)}};
}

} // namespace

void ProcessActions::inspect(int pid)
{
    QPointer<ProcessActions> self(this);
    (void)QtConcurrent::run([self, pid] {
        const QVariantMap detail = to_map(ring0::inspect_process(std::uint32_t(pid)));
        QMetaObject::invokeMethod(qApp, [self, pid, detail] {
            if (self) emit self->inspected(pid, detail);
        });
    });
}
