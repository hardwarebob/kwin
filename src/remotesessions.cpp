/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 KWin Contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "remotesessions.h"

#include "core/backendoutput.h"
#include "core/output.h"
#include "core/outputbackend.h"
#include "core/outputconfiguration.h"
#include "input.h"
#include "keyboard_input.h"
#include "main.h"
#include "pointer_input.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QUuid>

namespace KWin
{

RemoteSessionManager::RemoteSessionManager(QObject *parent)
    : QObject(parent)
{
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/RemoteSessions"), this,
                                                 QDBusConnection::ExportScriptableContents | QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSignals | QDBusConnection::ExportAllSlots);
}

QVariantList RemoteSessionManager::activeSessions() const
{
    QVariantList list;
    list.reserve(m_sessions.size());
    for (const RemoteSessionInfo &info : m_sessions) {
        QVariantMap map;
        map.insert(QStringLiteral("sessionId"), info.sessionId);
        map.insert(QStringLiteral("provider"), info.provider);
        map.insert(QStringLiteral("clientName"), info.clientName);
        map.insert(QStringLiteral("outputName"), info.outputName);
        list.append(map);
    }
    return list;
}

QString RemoteSessionManager::registerSession(const QString &provider, const QString &clientName, const QString &outputName)
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString owner = calledFromDBus() ? message().service() : QString();

    m_sessions.append(RemoteSessionInfo{
        .sessionId = id,
        .provider = provider,
        .clientName = clientName,
        .outputName = outputName,
        .ownerService = owner,
    });

    if (!owner.isEmpty()) {
        auto *watcher = new QDBusServiceWatcher(owner, QDBusConnection::sessionBus(),
                                                QDBusServiceWatcher::WatchForUnregistration, this);
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this, id, watcher]() {
            unregisterSession(id);
            watcher->deleteLater();
        });
    }

    Q_EMIT activeSessionsChanged();
    return id;
}

void RemoteSessionManager::unregisterSession(const QString &sessionId)
{
    const auto count = m_sessions.removeIf([&sessionId](const RemoteSessionInfo &info) {
        return info.sessionId == sessionId;
    });
    if (count > 0) {
        Q_EMIT activeSessionsChanged();
    }
}

void RemoteSessionManager::requestDisconnect(const QString &sessionId)
{
    Q_EMIT disconnectRequested(sessionId);
}

bool RemoteSessionManager::setOutputPolicies(const QString &outputName, bool unlockedWhileLocked, bool alwaysOnDpms)
{
    const auto outputs = kwinApp()->outputBackend()->outputs();
    BackendOutput *target = nullptr;
    for (BackendOutput *output : outputs) {
        if (output->name() == outputName) {
            target = output;
            break;
        }
    }
    if (!target) {
        return false;
    }

    OutputConfiguration cfg;
    auto changeSet = cfg.changeSet(target);
    changeSet->lockPolicy = unlockedWhileLocked ? BackendOutput::LockPolicy::Unlocked : BackendOutput::LockPolicy::FollowSession;
    changeSet->dpmsPolicy = alwaysOnDpms ? BackendOutput::DpmsPolicy::AlwaysOn : BackendOutput::DpmsPolicy::FollowSession;
    if (alwaysOnDpms) {
        changeSet->dpmsMode = BackendOutput::DpmsMode::On;
    } else if (workspace()->dpmsState() != Workspace::DpmsState::On) {
        changeSet->dpmsMode = BackendOutput::DpmsMode::Off;
    }

    const bool applied = workspace()->applyOutputConfiguration(cfg).has_value();
    if (applied && unlockedWhileLocked && waylandServer()->isScreenLocked()) {
        if (LogicalOutput *logicalOutput = workspace()->findOutput(target)) {
            if (!logicalOutput->geometry().contains(input()->pointer()->pos().toPoint())) {
                input()->pointer()->warp(logicalOutput->geometry().center());
            }
            const auto &stack = workspace()->stackingOrder();
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                Window *w = *it;
                if (w && w->isClient() && !w->isLockScreen() && !w->isLockScreenOverlay()
                    && w->isOnOutput(logicalOutput) && w->readyForPainting()) {
                    workspace()->activateWindow(w);
                    break;
                }
            }
            input()->keyboard()->update();
        }
    }
    return applied;
}

} // namespace KWin

#include "moc_remotesessions.cpp"
