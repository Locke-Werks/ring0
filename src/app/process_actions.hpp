// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/trust.hpp"

#include <QObject>
#include <QSet>
#include <QQmlEngine>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <expected>
#include <string>

// QML front for everything that acts on a process or inspects one. Every action
// reports failure through `failed` with the system's own message, so the UI can
// say why ("access is denied") instead of silently doing nothing.
class ProcessActions : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit ProcessActions(QObject* parent = nullptr);

    Q_INVOKABLE bool suspend(int pid);
    Q_INVOKABLE bool resume(int pid);
    Q_INVOKABLE bool terminate(int pid);
    Q_INVOKABLE bool setPriority(int pid, int cls);   // 0 idle .. 5 realtime
    Q_INVOKABLE int priority(int pid) const;           // -1 if unreadable
    Q_INVOKABLE bool singleCore(int pid);
    Q_INVOKABLE bool openLocation(const QString& path);
    Q_INVOKABLE void copyText(const QString& text);

    // Signature state for an image path: "signed", "unsigned", "invalid" or "" while
    // pending. Calling it queues verification; `trustChanged` fires when it lands.
    Q_INVOKABLE QString trust(const QString& path);
    Q_INVOKABLE QString signer(const QString& path) const;

    // Captures every thread's stack off the UI thread; result arrives in `stacksReady`
    // as [{ tid, frames: [{ module, symbol, offset, address }] }].
    Q_INVOKABLE void captureStacks(int pid);

signals:
    void failed(const QString& action, int pid, const QString& message);
    void succeeded(const QString& action, int pid);
    void trustChanged();
    void stacksReady(int pid, const QVariantList& threads);
    void stacksFailed(int pid, const QString& message);

private:
    bool report(const QString& action, int pid, const std::expected<void, std::wstring>& r);

    ring0::TrustCache trust_;
    QSet<QString> pending_;
};
