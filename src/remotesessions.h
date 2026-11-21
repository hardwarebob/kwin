/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 KWin Contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "kwin_export.h"

#include <QDBusContext>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace KWin
{

struct RemoteSessionInfo
{
    QString sessionId;
    QString provider;
    QString clientName;
    QString outputName;
    QString ownerService;
};

/**
 * Exposes /RemoteSessions (org.kde.KWin.RemoteSessions) on the session bus so
 * remote-display providers (Hermes, Sunshine, KRDP, etc.) can register active
 * sessions, configure per-output lock/DPMS policies, and let the lock screen
 * display active remote session status and request disconnection.
 */
class KWIN_EXPORT RemoteSessionManager : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin.RemoteSessions")
    Q_PROPERTY(QVariantList activeSessions READ activeSessions NOTIFY activeSessionsChanged)

public:
    explicit RemoteSessionManager(QObject *parent = nullptr);
    ~RemoteSessionManager() override = default;

    QVariantList activeSessions() const;

public Q_SLOTS:
    QString registerSession(const QString &provider, const QString &clientName, const QString &outputName);
    void unregisterSession(const QString &sessionId);
    void requestDisconnect(const QString &sessionId);
    bool setOutputPolicies(const QString &outputName, bool unlockedWhileLocked, bool alwaysOnDpms);

Q_SIGNALS:
    void activeSessionsChanged();
    void disconnectRequested(const QString &sessionId);

private:
    QList<RemoteSessionInfo> m_sessions;
};

} // namespace KWin
