// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/filter_proxy.hpp"

using namespace Qt::StringLiterals;

namespace {

int role_id(const QAbstractItemModel* m, const char* name)
{
    if (!m) return -1;
    const auto roles = m->roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        if (it.value() == name) return it.key();
    return -1;
}

// Mirrors parse_u in the legacy filter: leading digits only, so `pid:12x` is 12.
uint leading_uint(QStringView s)
{
    uint v = 0;
    for (QChar c : s) {
        if (c < u'0' || c > u'9') break;
        v = v * 10 + uint(c.unicode() - u'0');
    }
    return v;
}

} // namespace

FilterProxy::FilterProxy(QObject* parent) : QSortFilterProxyModel(parent)
{
    // Re-sorting on dataChanged keeps the order live as cpu figures move.
    setDynamicSortFilter(true);
    connect(this, &QAbstractProxyModel::sourceModelChanged, this, &FilterProxy::applySort);
    connect(this, &QAbstractItemModel::rowsInserted, this, &FilterProxy::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &FilterProxy::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &FilterProxy::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &FilterProxy::countChanged);
}

void FilterProxy::setFilterText(const QString& text)
{
    if (text == filter_text_) return;
    beginFilterChange();
    filter_text_ = text;
    pid_.reset();
    session_.reset();
    terms_.clear();
    for (const auto tok : QStringView(text).split(u' ', Qt::SkipEmptyParts)) {
        if (tok.startsWith(u"pid:", Qt::CaseInsensitive))
            pid_ = leading_uint(tok.sliced(4));
        else if (tok.startsWith(u"session:", Qt::CaseInsensitive))
            session_ = leading_uint(tok.sliced(8));
        else
            terms_.append(tok.toString());
    }
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    emit filterTextChanged();
}

void FilterProxy::setScope(int scope)
{
    if (scope == scope_) return;
    beginFilterChange();
    scope_ = scope;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    emit scopeChanged();
}

void FilterProxy::setSortKey(const QString& key)
{
    if (key == sort_key_) return;
    sort_key_ = key;
    applySort();
    emit sortKeyChanged();
}

void FilterProxy::setDescending(bool d)
{
    if (d == descending_) return;
    descending_ = d;
    applySort();
    emit descendingChanged();
}

int FilterProxy::roleFor(const QString& name) const
{
    return role_id(sourceModel(), name.toUtf8().constData());
}

void FilterProxy::applySort()
{
    const int role = roleFor(sort_key_);
    if (role < 0) return;
    setSortRole(role);
    sort(0, descending_ ? Qt::DescendingOrder : Qt::AscendingOrder);
}

bool FilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const auto* m = sourceModel();
    if (!m) return false;
    const auto idx = m->index(sourceRow, 0, sourceParent);
    auto get = [&](const char* role) { return m->data(idx, role_id(m, role)); };

    if (scope_ != All) {
        const bool sys = get("isSystem").toBool();
        if ((scope_ == System) != sys) return false;
    }
    if (pid_ && get("pid").toUInt() != *pid_) return false;
    if (session_ && get("session").toUInt() != *session_) return false;
    if (!terms_.isEmpty()) {
        const auto name = get("name").toString();
        const auto cmd = get("commandLine").toString();
        for (const auto& t : terms_)
            if (!name.contains(t, Qt::CaseInsensitive) && !cmd.contains(t, Qt::CaseInsensitive))
                return false;
    }
    return true;
}

bool FilterProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    const auto a = left.data(sortRole());
    const auto b = right.data(sortRole());
    if (a.typeId() == QMetaType::QString) {
        const int c = a.toString().compare(b.toString(), Qt::CaseInsensitive);
        if (c != 0) return c < 0;
    } else {
        const auto ord = QVariant::compare(a, b);
        if (ord == QPartialOrdering::Less) return true;
        if (ord == QPartialOrdering::Greater) return false;
    }
    // Equal keys (idle processes all at 0% cpu) would otherwise swap places on
    // every re-sort; pid gives a total order so rows hold still.
    const int pidRole = role_id(sourceModel(), "pid");
    return left.data(pidRole).toUInt() < right.data(pidRole).toUInt();
}

int FilterProxy::sourceRow(int proxyRow) const
{
    return mapToSource(index(proxyRow, 0)).row();
}

int FilterProxy::proxyRowForPid(int pid) const
{
    const int pidRole = roleFor(u"pid"_s);
    for (int r = 0; r < rowCount(); ++r)
        if (index(r, 0).data(pidRole).toInt() == pid) return r;
    return -1;
}
