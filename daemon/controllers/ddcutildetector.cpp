/*  This file is part of the KDE project
 *    SPDX-FileCopyrightText: 2023 Quang Ngô <ngoquang2708@gmail.com>
 *    SPDX-FileCopyrightText: 2026 Modified hybrid compatibility layer
 *
 *    SPDX-License-Identifier: LGPL-2.0-only
 */

#include "ddcutildetector.h"
#include "ddcutildisplay.h"

#include <powerdevil_debug.h>

#include <QMutex>
#include <QTimer>

#include <map>
#include <memory>

#ifdef WITH_DDCUTIL
#include <ddcutil_c_api.h>
#endif

class DDCutilPrivateSingleton : public QObject
{
    Q_OBJECT

public:
    static DDCutilPrivateSingleton &instance();

    void detect();
    const std::map<QString, std::unique_ptr<DDCutilDisplay>> &displays();

Q_SIGNALS:
    void displaysChanged();

private Q_SLOTS:
    void performRedetect();
    void removeDisplay(const QString &id);

private:
    DDCutilPrivateSingleton();
    ~DDCutilPrivateSingleton() override = default;

    std::map<QString, std::unique_ptr<DDCutilDisplay>> m_displays;
    std::map<QString, std::unique_ptr<DDCutilDisplay>> m_pendingDisplays;

    QMutex m_openDisplayMutex;
    bool m_performedDetection = false;
    bool m_noDdcutil = false;

    QTimer m_redetectTimer;
};

// -------------------- singleton --------------------

DDCutilPrivateSingleton &DDCutilPrivateSingleton::instance()
{
    static DDCutilPrivateSingleton s;
    return s;
}

DDCutilPrivateSingleton::DDCutilPrivateSingleton()
{
    m_redetectTimer.setSingleShot(true);
    m_redetectTimer.setInterval(500);

    connect(&m_redetectTimer, &QTimer::timeout, this, &DDCutilPrivateSingleton::performRedetect);

#ifdef WITH_DDCUTIL
    // optional disable via env
    m_noDdcutil = qEnvironmentVariableIntValue("POWERDEVIL_NO_DDCUTIL") > 0;

    if (m_noDdcutil) {
        qCInfo(POWERDEVIL) << "[DDCutilDetector] disabled via env var";
    }
#else
    m_noDdcutil = true;
#endif
}

// -------------------- detection --------------------

void DDCutilPrivateSingleton::detect()
{
#ifdef WITH_DDCUTIL
    if (m_noDdcutil || m_performedDetection) {
        return;
    }

    m_performedDetection = true;

    qCDebug(POWERDEVIL) << "[DDCutilDetector]: detecting displays via ddca_get_display_refs()";

    DDCA_Display_Ref *refs = nullptr;

    DDCA_Status status = ddca_get_display_refs(true, &refs);
    if (status != DDCRC_OK || !refs) {
        qCWarning(POWERDEVIL) << "[DDCutilDetector]: no displays found or ddca_get_display_refs failed";
        return;
    }

    int count = 0;
    while (refs[count] != nullptr) {
        ++count;
    }

    qCInfo(POWERDEVIL) << "[DDCutilDetector] found" << count << "display(s)";

    for (int i = 0; i < count; ++i) {
        // validate (safe even if older libddcutil ignores flags)
#if DDCUTIL_VERSION >= QT_VERSION_CHECK(2, 1, 0)
        DDCA_Status v = ddca_validate_display_ref(refs[i], false);
        if (v != DDCRC_OK && v != DDCRC_DISCONNECTED) {
            continue;
        }
#endif

        auto display = std::make_unique<DDCutilDisplay>(refs[i], &m_openDisplayMutex);

        QString id = DDCutilDisplay::generatePathId(display->ioPath());
        if (id.isEmpty()) {
            qCWarning(POWERDEVIL) << "[DDCutilDetector]: invalid display id, skipping";
            continue;
        }

        // soft initialization failure handling
        if (display->label().isEmpty()) {
            qCDebug(POWERDEVIL) << "[DDCutilDetector]: delayed init for" << id;

            display->scheduleRetryInit();

            connect(display.get(), &DDCutilDisplay::retryInitFinished, this, [this, id](bool success) {
                auto node = m_pendingDisplays.extract(id);
                if (success && !node.empty()) {
                    m_displays.insert(std::move(node));
                    Q_EMIT displaysChanged();
                }
            });

            m_pendingDisplays[id] = std::move(display);
            continue;
        }

        m_displays.emplace(id, std::move(display));
    }

    if (!m_displays.empty()) {
        Q_EMIT displaysChanged();
    }
#endif
}

// -------------------- redetect --------------------

void DDCutilPrivateSingleton::performRedetect()
{
#ifdef WITH_DDCUTIL
    if (m_noDdcutil) {
        return;
    }

    qCDebug(POWERDEVIL) << "[DDCutilDetector]: redetect triggered";

    m_pendingDisplays.clear();

    std::map<QString, std::unique_ptr<DDCutilDisplay>> old;
    std::swap(m_displays, old);

    if (ddca_redetect_displays() == DDCRC_OK) {
        m_performedDetection = false;
        detect();
    } else {
        qCWarning(POWERDEVIL) << "[DDCutilDetector]: ddca_redetect_displays failed";
    }

    if (!m_displays.empty() || !old.empty()) {
        Q_EMIT displaysChanged();
    }
#endif
}

// -------------------- removal --------------------

void DDCutilPrivateSingleton::removeDisplay(const QString &id)
{
    m_pendingDisplays.erase(id);

    if (!m_displays.contains(id)) {
        return;
    }

    m_displays.erase(id);
    qCDebug(POWERDEVIL) << "[DDCutilDetector]: removed display" << id;

    Q_EMIT displaysChanged();
}

// -------------------- public API --------------------

void DDCutilDetector::detect()
{
#ifdef WITH_DDCUTIL
    bool first = connect(&DDCutilPrivateSingleton::instance(),
                         &DDCutilPrivateSingleton::displaysChanged,
                         this,
                         &DisplayBrightnessDetector::displaysChanged,
                         Qt::UniqueConnection);

    DDCutilPrivateSingleton::instance().detect();

    if (first && !DDCutilPrivateSingleton::instance().displays().empty()) {
        Q_EMIT displaysChanged();
    }

    Q_EMIT detectionFinished(!DDCutilPrivateSingleton::instance().displays().empty());
#else
    qCInfo(POWERDEVIL) << "[DDCutilDetector] compiled without DDC support";
    Q_EMIT detectionFinished(false);
#endif
}

QList<DisplayBrightness *> DDCutilDetector::displays() const
{
    QList<DisplayBrightness *> result;

#ifdef WITH_DDCUTIL
    for (const auto &p : DDCutilPrivateSingleton::instance().displays()) {
        result.append(p.second.get());
    }
#endif

    return result;
}

#include "ddcutildetector.moc"