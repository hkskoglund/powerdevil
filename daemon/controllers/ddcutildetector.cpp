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
    const std::map<QString, std::unique_ptr<DDCutilDisplay>> &displays();

#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
    void displayStatusChanged(DDCA_Display_Status_Event &event);
#endif

Q_SIGNALS:
    void displaysChanged();

    // emitted from the ddcutil callback from potentially outside the object's thread
    void displayAdded();
    void displayRemoved(const QString &id);

private Q_SLOTS:
    void performRedetect();
    void removeDisplay(const QString &id);

private:
    std::map<QString, std::unique_ptr<DDCutilDisplay>> m_displays;
    std::map<QString, std::unique_ptr<DDCutilDisplay>> m_pendingDisplays;
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
    connect(this, &DDCutilPrivateSingleton::displayAdded, this, [this]() {
        // debounce instead of immediate detect
        qCDebug(POWERDEVIL) << "[DDCutilDetector]: displayAdded → scheduling delayed detect";
        m_redetectTimer.start();
    });

    connect(this, &DDCutilPrivateSingleton::displayRemoved, this, &DDCutilPrivateSingleton::removeDisplay);
    // Removals can also come in bursts during display configuration changes, debounce them as well.
    // Instead of immediate removal, we trigger a redetection which will synchronize the state.
    connect(this, &DDCutilPrivateSingleton::displayRemoved, this, [this](const QString &id) {
        Q_UNUSED(id);
        m_redetectTimer.start();
    });

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
            currentIds.insert(id);

            // If libddcutil already knows DDC isn't working and it's not a retry candidate,
            // we can skip it. Laptop displays are often identified via their I/O path or EDID.
            if (info->path.io_mode == DDCA_IO_I2C && info->model_name[0] == '\0') {
                qCDebug(POWERDEVIL) << "[DDCutilDetector]: Skipping internal/invalid panel on" << id;
                ddca_free_display_info(info);
                continue;
            }
            ddca_free_display_info(info);

            if (m_displays.contains(id) || m_pendingDisplays.contains(id)) {
                qCDebug(POWERDEVIL) << "[DDCutilDetector]: Already tracking display" << id;
                continue;
            }
        }

        auto display = std::make_unique<DDCutilDisplay>(displayRefs[i], &m_openDisplayMutex);
        const QString id = DDCutilDisplay::generatePathId(display->ioPath());

        if (id.isEmpty()) {
            continue;
        }

        connect(display.get(), &DDCutilDisplay::supportsBrightnessChanged, this, [this, id](bool supported) {
            if (!supported)
                removeDisplay(id);
        });

        if (!display->supportsBrightness() || display->label().isEmpty()) {
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
            if (status == DDCRC_DISCONNECTED)
                continue;
#endif
            display->scheduleRetryInit();
            connect(display.get(), &DDCutilDisplay::retryInitFinished, this, [this, id](bool success) {
                if (auto node = m_pendingDisplays.extract(id); success && !node.empty()) {
                    m_displays.insert(std::move(node));
                    Q_EMIT displaysChanged();
                }
            });
            m_pendingDisplays[id] = std::move(display);
            continue;
        }

        m_pendingDisplays.erase(id);
        m_displays[id] = std::move(display);
    }

    // Synchronize the display list by removing any stale objects that are no longer reported
    bool changed = false;
    for (auto it = m_displays.begin(); it != m_displays.end();) {
        if (!currentIds.contains(it->first)) {
            qCDebug(POWERDEVIL) << "[DDCutilDetector]: Pruning stale display" << it->first;
            it = m_displays.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    for (auto it = m_pendingDisplays.begin(); it != m_pendingDisplays.end();) {
        if (!currentIds.contains(it->first)) {
            it = m_pendingDisplays.erase(it);
        } else {
            ++it;
        }
    }

    if (changed) {
        Q_EMIT displaysChanged();
    }

    free(displayRefs);
}

const std::map<QString, std::unique_ptr<DDCutilDisplay>> &DDCutilPrivateSingleton::displays()
{
    return m_displays;
}

void DDCutilPrivateSingleton::performRedetect()
{
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
    if (m_noDdcutil) {
        return;
    }
    qCDebug(POWERDEVIL) << "[DDCutilDetector]: Screen configuration changed. Redetecting displays";

    const size_t oldSize = m_displays.size();
    m_pendingDisplays.clear(); // Clear pending displays as their refs are invalidated by redetection

    // We must clear m_displays because ddca_redetect_displays invalidates the raw DDCA_Display_Ref
    // pointers held by the DDCutilDisplay objects.
    m_displays.clear();

    if (ddca_redetect_displays() == DDCRC_OK) {
        m_performedDetection = false;
        detect();
    } else {
        qCCritical(POWERDEVIL) << "[DDCutilDetector]: Redetection failed";
    }

    if (m_displays.size() != oldSize || !m_displays.empty()) {
        Q_EMIT displaysChanged();
    }
#endif
}

#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
static const char *eventName(DDCA_Display_Event_Type type)
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
    QString flagStr;
    // DDCA_DISPLAY_EVENT_DDC_WORKING is usually 0x08 in libddcutil 2.2.0+
    if (event.event_type == DDCA_EVENT_DISPLAY_CONNECTED) {
        if (event.flags & DDCA_DISPLAY_EVENT_DDC_WORKING) {
            flagStr = u" [DDC Working]"_s;
        } else {
            flagStr = u" [DDC Not Ready - Background check started]"_s;
        }
    }

    qCDebug(POWERDEVIL) << "[DDCutilDetector]: Event arrived from ddcutil:" << eventName(event.event_type) << "(type:" << event.event_type << ", flags: 0x"
                        << Qt::hex << (int)event.flags << ")" << flagStr;

    switch (event.event_type) {
    case DDCA_EVENT_DISPLAY_CONNECTED:
        if (event.flags & DDCA_DISPLAY_EVENT_DDC_WORKING) {
            qCDebug(POWERDEVIL) << "[DDCutilDetector]: DISPLAY_CONNECTED (DDC working) → schedule detect";
            Q_EMIT displayAdded();
        } else {
            // DDC not ready yet — avoid early detect
            qCDebug(POWERDEVIL) << "[DDCutilDetector]: DISPLAY_CONNECTED (DDC not ready)";
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 2, 0)
            // wait for DDCA_EVENT_DDC_ENABLED
#else
            // fallback: delayed detect for older libddcutil
            m_redetectTimer.start();
#endif
        }
        break;
    case DDCA_EVENT_DPMS_AWAKE:
        qCDebug(POWERDEVIL) << "[DDCutilDetector]: DPMS_AWAKE";
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 2, 0)
        // wait for DDC_ENABLED instead of detecting immediately
#else
        // fallback: delayed detect
        m_redetectTimer.start();
#endif
        break;

    case DDCA_EVENT_DDC_ENABLED:
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 2, 0)
        // DDC is now confirmed working → safe to detect
        qCDebug(POWERDEVIL) << "[DDCutilDetector]: DDC_ENABLED → triggering detect";
#endif
        detect();
        break;
    case DDCA_EVENT_DISPLAY_DISCONNECTED:
    case DDCA_EVENT_DPMS_ASLEEP:
        Q_EMIT displayRemoved(DDCutilDisplay::generatePathId(event.io_path));
        break;
    default:
        break;
    }
}
#endif

void DDCutilPrivateSingleton::removeDisplay(const QString &id)
{
    m_pendingDisplays.erase(id); // cancel any scheduled initialization retries

    if (auto deletedAfterEmit = m_displays.extract(id); !deletedAfterEmit.empty()) {
        qCDebug(POWERDEVIL) << "[DDCutilDetector]: Removing display" << id;
        Q_EMIT displaysChanged();
    } else {
        qCDebug(POWERDEVIL) << "[DDCutilDetector]: Failed to remove display" << id;
    }
}
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
    result.reserve(DDCutilPrivateSingleton::instance().displays().size());
    for (const auto &pair : DDCutilPrivateSingleton::instance().displays()) {
        result.append(pair.second.get());
    }
#endif
    return result;
}

#include "ddcutildetector.moc"
#include "moc_ddcutildetector.cpp"
