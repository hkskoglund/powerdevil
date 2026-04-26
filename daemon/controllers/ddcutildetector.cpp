/*  This file is part of the KDE project
 *    SPDX-FileCopyrightText: 2017 Dorian Vogel <dorianvogel@gmail.com>
 *    SPDX-FileCopyrightText: 2024 Jakob Petsovits <jpetso@petsovits.com>
 *
 *    SPDX-License-Identifier: LGPL-2.0-only
 *
 */

#include "ddcutildetector.h"

#include <powerdevil_debug.h>

#include <QMutex>
#include <QTimer>

#include <map>
#include <memory> // std::unique_ptr
#include <span>

using namespace Qt::StringLiterals;

#ifdef WITH_DDCUTIL
#include <ddcutil_c_api.h>

#include "ddcutildisplay.h"

enum class DisplayState {
    Absent,
    Initializing,
    Ready
};

struct DisplayEntry {
    std::unique_ptr<DDCutilDisplay> display;
    DisplayState state = DisplayState::Initializing;
};

#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
void ddcaCallback(DDCA_Display_Status_Event event);
#endif

class DDCutilPrivateSingleton : public QObject
{
    Q_OBJECT

#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
    friend void ddcaCallback(DDCA_Display_Status_Event event);
#endif
    friend class DDCutilDetector;

public:
    static DDCutilPrivateSingleton &instance();

private:
    explicit DDCutilPrivateSingleton();
    ~DDCutilPrivateSingleton();

    void detect();
    const std::map<QString, DisplayEntry> &displays() const;

#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
    void displayStatusChanged(DDCA_Display_Status_Event &event);
#endif

Q_SIGNALS:
    void displaysChanged();

private Q_SLOTS:
    void performRedetect();

private:
    std::map<QString, DisplayEntry> m_displays;
    // ddcutil has global state, let's avoid simultaneous access to its open display map
    QMutex m_openDisplayMutex;
    bool m_performedDetection = false;
    bool m_noDdcutil = false;

    QTimer m_redetectTimer;
};

#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
void ddcaCallback(DDCA_Display_Status_Event event)
{
    DDCutilPrivateSingleton::instance().displayStatusChanged(event);
}
#endif

DDCutilPrivateSingleton &DDCutilPrivateSingleton::instance()
{
    static DDCutilPrivateSingleton singleton;
    return singleton;
}

DDCutilPrivateSingleton::DDCutilPrivateSingleton()
    : QObject()
{
    m_redetectTimer.setSingleShot(true);
    m_redetectTimer.setInterval(2500); // allow DP/DDC to stabilize

    m_noDdcutil = qEnvironmentVariableIntValue("POWERDEVIL_NO_DDCUTIL") > 0;
    if (m_noDdcutil) {
        return;
    }
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 0, 0)
    qCDebug(POWERDEVIL) << "[DDCutilDetector]: Initializing ddcutil API (create ddcutil configuration file for tracing & more)...";
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
    DDCA_Status status = ddca_init2(nullptr, DDCA_SYSLOG_NOTICE, DDCA_INIT_OPTIONS_CLIENT_OPENED_SYSLOG, nullptr);
#else
    DDCA_Status status = ddca_init(nullptr, DDCA_SYSLOG_NOTICE, DDCA_INIT_OPTIONS_CLIENT_OPENED_SYSLOG);
#endif

    if (status < 0) {
        m_noDdcutil = true;
        qCWarning(POWERDEVIL) << "[DDCutilDetector]: Could not initialize ddcutil API. Not using DDC for monitor brightness.";
        return;
    }
#endif
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
    if (ddca_register_display_status_callback(ddcaCallback)) {
        qCWarning(POWERDEVIL) << "[DDCutilDetector]: Failed to initialize callback";
        return;
    }

    connect(&m_redetectTimer, &QTimer::timeout, this, &DDCutilPrivateSingleton::performRedetect);

    ddca_start_watch_displays(DDCA_Display_Event_Class(DDCA_EVENT_CLASS_ALL));
#endif
}

DDCutilPrivateSingleton::~DDCutilPrivateSingleton()
{
    if (m_noDdcutil) {
        return;
    }
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
    ddca_stop_watch_displays(false);
    ddca_unregister_display_status_callback(ddcaCallback);
#endif
}

void DDCutilPrivateSingleton::detect()
{
    if (m_noDdcutil)
        return;

    DDCA_Display_Ref *displayRefs = nullptr;
    // Perform a hardware rescan only on first run or if m_performedDetection was reset.
    // Otherwise, rely on udev events and background recheck threads.
    if (ddca_get_display_refs(!m_performedDetection, &displayRefs) != DDCRC_OK || !displayRefs)
        return;

    bool changed = false;
    QSet<QString> currentIds;
    m_performedDetection = true;
    for (int i = 0; displayRefs[i] != nullptr; ++i) {
        DDCA_Status status = DDCRC_OK;
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
        status = ddca_validate_display_ref(displayRefs[i], false /*require_not_asleep*/);
        if (status != DDCRC_OK && status != DDCRC_DISCONNECTED)
            continue;
#endif

        // Optimization: Check display info to skip known non-DDC internal panels
        // before instantiating the full DDCutilDisplay object.
        DDCA_Display_Info *info = nullptr;
        if (ddca_get_display_info(displayRefs[i], &info) == DDCRC_OK) {
            const QString id = DDCutilDisplay::generatePathId(info->path);

            // If libddcutil already knows DDC isn't working and it's not a retry candidate,
            // we can skip it. Laptop displays are often identified via their I/O path or EDID.
            if (info->path.io_mode == DDCA_IO_I2C && info->model_name[0] == '\0') {
                qCDebug(POWERDEVIL) << "[DDCutilDetector]: Skipping internal/invalid panel on" << id;
                ddca_free_display_info(info);
                continue;
            }
            ddca_free_display_info(info);

            if (m_displays.contains(id)) {
                if (m_displays[id].display->displayRef() != displayRefs[i]) {
                    m_displays[id].display->updateDisplayRef(displayRefs[i]);
                }
                currentIds.insert(id);
                continue;
            }
        }

        auto display = std::make_unique<DDCutilDisplay>(displayRefs[i], &m_openDisplayMutex);
        const QString id = display->id();
        if (id.isEmpty())
            continue;

        currentIds.insert(id);
        m_displays[id] = {std::move(display), DisplayState::Initializing};

        changed = true;
    }

    // Synchronize the display list by removing any stale objects that are no longer reported
    for (auto it = m_displays.begin(); it != m_displays.end();) {
        if (!currentIds.contains(it->first)) {
            it = m_displays.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    if (changed) {
        Q_EMIT displaysChanged();
    }

    free(displayRefs);
}

const std::map<QString, DisplayEntry> &DDCutilPrivateSingleton::displays() const
{
    return m_displays;
}

void DDCutilPrivateSingleton::performRedetect()
{
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
    if (m_noDdcutil) {
        return;
    }
    qCDebug(POWERDEVIL) << "[DDCutilDetector]: Redetect";

    m_displays.clear();

    if (ddca_redetect_displays() == DDCRC_OK) {
        m_performedDetection = false;
        detect();
    }
#endif
}

#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
[[maybe_unused]] static const char *eventName(DDCA_Display_Event_Type type)
{
    switch (type) {
    case DDCA_EVENT_DISPLAY_CONNECTED:
        return "DDCA_EVENT_DISPLAY_CONNECTED";
    case DDCA_EVENT_DPMS_AWAKE:
        return "DDCA_EVENT_DPMS_AWAKE";
    case DDCA_EVENT_DDC_ENABLED:
        return "DDCA_EVENT_DDC_ENABLED";
    case DDCA_EVENT_DISPLAY_DISCONNECTED:
        return "DDCA_EVENT_DISPLAY_DISCONNECTED";
    case DDCA_EVENT_DPMS_ASLEEP:
        return "DDCA_EVENT_DPMS_ASLEEP";
    case DDCA_EVENT_UNUSED2:
        return "DDCA_EVENT_UNUSED2";
    }
    return "UNKNOWN";
}

void DDCutilPrivateSingleton::displayStatusChanged(DDCA_Display_Status_Event &event)
{
    const QString id = DDCutilDisplay::generatePathId(event.io_path);
    if (id.isEmpty())
        return;

    QMetaObject::invokeMethod(
        this,
        [this, id, event]() {
            if (!m_displays.contains(id))
                return;

            if (event.event_type == DDCA_EVENT_DISPLAY_CONNECTED) {
                m_displays[id].display->updateDisplayRef(event.dref);
                m_displays[id].state = DisplayState::Ready;
            } else if (event.event_type == DDCA_EVENT_DISPLAY_DISCONNECTED) {
                m_displays[id].state = DisplayState::Absent;
            }

            Q_EMIT displaysChanged();
        },
        Qt::QueuedConnection);
}
#endif

#endif

DDCutilDetector::DDCutilDetector(QObject *parent)
    : DisplayBrightnessDetector(parent)
{
}

DDCutilDetector::~DDCutilDetector()
{
}

void DDCutilDetector::detect()
{
#ifdef WITH_DDCUTIL
    connect(&DDCutilPrivateSingleton::instance(),
            &DDCutilPrivateSingleton::displaysChanged,
            this,
            &DisplayBrightnessDetector::displaysChanged,
            Qt::UniqueConnection);

    DDCutilPrivateSingleton::instance().detect();
    Q_EMIT detectionFinished(!DDCutilPrivateSingleton::instance().displays().empty());
#else
    qCInfo(POWERDEVIL) << "[DDCutilDetector] compiled without DDC/CI support";
    Q_EMIT detectionFinished(false);
#endif
}

QList<DisplayBrightness *> DDCutilDetector::displays() const
{
    QList<DisplayBrightness *> result;
#ifdef WITH_DDCUTIL
    for (const auto &[id, entry] : DDCutilPrivateSingleton::instance().displays())
        result.append(entry.display.get());
#endif
    return result;
}

#include "ddcutildetector.moc"
#include "moc_ddcutildetector.cpp"
