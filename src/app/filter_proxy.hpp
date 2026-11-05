// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QQmlEngine>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <optional>

// Filter and sort over ProcessModel. Query syntax follows the legacy filter:
// `pid:N`, `session:N`, anything else a case-insensitive substring of the name
// or command line. Space-separated terms combine with AND.
class FilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
    Q_PROPERTY(int scope READ scope WRITE setScope NOTIFY scopeChanged)
    Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY sortKeyChanged)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY descendingChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Scope { All = 0, User = 1, System = 2 };
    Q_ENUM(Scope)

    explicit FilterProxy(QObject* parent = nullptr);

    QString filterText() const { return filter_text_; }
    void setFilterText(const QString& text);
    int scope() const { return scope_; }
    void setScope(int scope);
    QString sortKey() const { return sort_key_; }
    void setSortKey(const QString& key);
    bool descending() const { return descending_; }
    void setDescending(bool d);
    int count() const { return rowCount(); }

    // Maps a proxy row back to the source row, for actions that need the pid.
    Q_INVOKABLE int sourceRow(int proxyRow) const;
    Q_INVOKABLE int proxyRowForPid(int pid) const;

signals:
    void filterTextChanged();
    void scopeChanged();
    void sortKeyChanged();
    void descendingChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    void applySort();
    int roleFor(const QString& name) const;

    QString filter_text_;
    int scope_ = All;
    QString sort_key_ = QStringLiteral("cpu");
    bool descending_ = true;

    std::optional<uint> pid_;
    std::optional<uint> session_;
    QStringList terms_;
};
