/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "dpmsinputeventfilter.h"
#include "core/backendoutput.h"
#include "core/inputdevice.h"
#include "core/output.h"
#include "core/outputbackend.h"
#include "input_event.h"
#include "main.h"
#include "touch_input.h"
#include "utils/keys.h"
#include "utils/proximitysensor.h"
#include "wayland_server.h"
#include "workspace.h"

#include <QGuiApplication>
#include <QKeyEvent>

namespace KWin
{

static bool isUnlockedOnlyDevice(InputDevice *device)
{
    if (!device || !workspace()->hasUnlockedAwakeOutput()) {
        return false;
    }
    if (device->inputScope() == InputDevice::InputScope::UnlockedOutputsOnly) {
        return true;
    }
    if (device->sysPath().startsWith(QLatin1String("/sys/devices/virtual/input/"))) {
        return true;
    }
    if (!device->outputName().isEmpty()) {
        if (LogicalOutput *out = workspace()->findOutput(device->outputName())) {
            return out->backendOutput()->lockPolicy() == BackendOutput::LockPolicy::Unlocked
                && out->backendOutput()->dpmsMode() == BackendOutput::DpmsMode::On;
        }
    }
    return false;
}

static bool isPosOnUnlockedAwakeOutput(const QPointF &pos)
{
    if (LogicalOutput *out = workspace()->outputAt(pos)) {
        return out->backendOutput()->lockPolicy() == BackendOutput::LockPolicy::Unlocked
            && out->backendOutput()->dpmsMode() == BackendOutput::DpmsMode::On;
    }
    return false;
}

DpmsInputEventFilter::DpmsInputEventFilter()
    : InputEventFilter(InputFilterOrder::Dpms)
{
    // Touch up events are swallowed while this filter is installed, so a touch sequence
    // that started before the screens turned off would never end and block pointer focus
    // updates. The filter is not installed yet, so the cancel reaches the seat.
    input()->touch()->cancel();

    KSharedConfig::Ptr kwinSettings = kwinApp()->config();
    m_enableDoubleTap = kwinSettings->group(QStringLiteral("Wayland")).readEntry<bool>("DoubleTapWakeup", true);

    if (m_enableDoubleTap) {
        m_sensor = std::make_unique<ProximitySensor>();
        m_sensor->setEnabled(true);

        updateProximitySensor();
        connect(m_sensor.get(), &ProximitySensor::readingReceived, this, &DpmsInputEventFilter::updateProximitySensor);
    }
}

DpmsInputEventFilter::~DpmsInputEventFilter()
{
}

void DpmsInputEventFilter::updateProximitySensor()
{
    if (m_sensor->reading()) {
        m_proximityClose = *m_sensor->reading();
    }
}

bool DpmsInputEventFilter::pointerMotion(PointerMotionEvent *event)
{
    if (isUnlockedOnlyDevice(event->device)) {
        return false;
    }
    if (!event->warp) {
        // The intention is to wake the screen on user interactions
        // warp events aren't user interactions, so ignore them.
        notify();
    }
    return true;
}

bool DpmsInputEventFilter::pointerButton(PointerButtonEvent *event)
{
    if (isUnlockedOnlyDevice(event->device)) {
        return false;
    }
    notify();
    return true;
}

bool DpmsInputEventFilter::pointerAxis(PointerAxisEvent *event)
{
    if (isUnlockedOnlyDevice(event->device)) {
        return false;
    }
    notify();
    return true;
}

bool DpmsInputEventFilter::keyboardKey(KeyboardKeyEvent *event)
{
    if (isUnlockedOnlyDevice(event->device)) {
        return false;
    }
    if (isMediaKey(event->key)) {
        // don't wake up the screens for media or volume keys
        return false;
    }

    // Wakeup key is sent by either ACPI driver or other drivers when
    // system is resumed from sleep but that is not necessarily wakeup intended
    // to do full-scale dpms on event. Let system wake-up without display, only
    // wake system up if we get actual keyboard key.
    if (event->key == Qt::Key::Key_WakeUp) {
        return false;
    }

    if (event->state == KeyboardKeyState::Pressed) {
        notify();
    } else if (event->state == KeyboardKeyState::Released) {
        return false;
    }
    return true;
}

bool DpmsInputEventFilter::touchDown(TouchDownEvent *event)
{
    if (isPosOnUnlockedAwakeOutput(event->pos)) {
        m_unlockedTouchIds.insert(event->id);
        return false;
    }
    if (m_enableDoubleTap) {
        if (m_touchPoints.isEmpty()) {
            if (!m_doubleTapTimer.isValid()) {
                // this is the first tap
                m_doubleTapTimer.start();
            } else {
                if (m_doubleTapTimer.elapsed() < qApp->doubleClickInterval()) {
                    m_secondTap = true;
                } else {
                    // took too long. Let's consider it a new click
                    m_doubleTapTimer.restart();
                }
            }
        } else {
            // not a double tap
            m_doubleTapTimer.invalidate();
            m_secondTap = false;
        }
        m_touchPoints << event->id;
    }
    return true;
}

bool DpmsInputEventFilter::touchUp(TouchUpEvent *event)
{
    if (m_unlockedTouchIds.remove(event->id)) {
        return false;
    }
    if (m_enableDoubleTap) {
        m_touchPoints.removeAll(event->id);
        if (m_touchPoints.isEmpty() && m_doubleTapTimer.isValid() && m_secondTap) {
            // if device in pocket, do not wake device up
            const bool wakeUp = m_doubleTapTimer.elapsed() < qApp->doubleClickInterval() && !m_proximityClose;
            // notify() destroys this filter, so reset our state before calling it
            m_doubleTapTimer.invalidate();
            m_secondTap = false;
            if (wakeUp) {
                notify();
            }
        }
    }
    return true;
}

bool DpmsInputEventFilter::touchMotion(TouchMotionEvent *event)
{
    if (m_unlockedTouchIds.contains(event->id)) {
        return false;
    }
    // ignore the event
    return true;
}

bool DpmsInputEventFilter::tabletToolProximityEvent(TabletToolProximityEvent *event)
{
    if (isUnlockedOnlyDevice(event->device)) {
        return false;
    }
    return true;
}

bool DpmsInputEventFilter::tabletToolAxisEvent(TabletToolAxisEvent *event)
{
    if (isUnlockedOnlyDevice(event->device)) {
        return false;
    }
    return true;
}

bool DpmsInputEventFilter::tabletToolTipEvent(TabletToolTipEvent *event)
{
    if (isUnlockedOnlyDevice(event->device)) {
        return false;
    }
    if (event->type == TabletToolTipEvent::Press) {
        // Only wake when the tool is actually pressed down not just hovered over the tablet
        notify();
    }
    return true;
}

bool DpmsInputEventFilter::tabletToolButtonEvent(TabletToolButtonEvent *event)
{
    if (isUnlockedOnlyDevice(event->device)) {
        return false;
    }
    if (event->pressed) {
        notify();
    }
    return true;
}

bool DpmsInputEventFilter::tabletPadButtonEvent(TabletPadButtonEvent *event)
{
    if (event->pressed) {
        notify();
    }
    return true;
}

bool DpmsInputEventFilter::tabletPadStripEvent(TabletPadStripEvent *event)
{
    notify();
    return true;
}

bool DpmsInputEventFilter::tabletPadRingEvent(TabletPadRingEvent *event)
{
    notify();
    return true;
}

bool DpmsInputEventFilter::tabletPadDialEvent(TabletPadDialEvent *event)
{
    notify();
    return true;
}

bool DpmsInputEventFilter::switchEvent(SwitchEvent *event)
{
    if (event->device->isLidSwitch() && event->state == SwitchState::Off) {
        notify();
    }
    return false;
}

void DpmsInputEventFilter::notify()
{
    workspace()->requestDpmsState(Workspace::DpmsState::On);
}

}
#include "moc_dpmsinputeventfilter.cpp"
