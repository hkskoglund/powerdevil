/*  This file is part of the KDE project
 *    SPDX-FileCopyrightText: 2023 Quang Ngô
 *    SPDX-FileCopyrightText: 2026 Compatibility refactor
 *
 *    SPDX-License-Identifier: LGPL-2.0-only
 */

#include "ddcutildisplay.h"

#include <powerdevil_debug.h>

#include <chrono>
#include <span>

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds s_setBrightnessDelay = 1s;
constexpr std::array<std::chrono::milliseconds, 3> s_backoffRetryIntervals = {1s, 2s, 3s};

#ifdef WITH_DDCUTIL
constexpr DDCA_Vcp_Feature_Code BRIGHTNESS_VCP_FEATURE_CODE = 0x10;
#endif

// ============================================================
// Constructor
// ============================================================

DDCutilDisplay::DDCutilDisplay(DDCA_Display_Ref displayRef, QMutex *openDisplayMutex)
    : m_displayRef(displayRef)
    , m_timer(new QTimer(this))
    , m_retryCounter(0)
    , m_openDisplayMutex(openDisplayMutex)
    , m_brightness(-1)
    , m_maxBrightness(-1)
    , m_supportsBrightness(false)
{
    Q_ASSERT(m_displayRef != nullptr);

#ifdef WITH_DDCUTIL
    qCDebug(POWERDEVIL) << "[DDCutilDisplay]: initializing display";

    DDCA_Display_Info *info = nullptr;
    DDCA_Status status = ddca_get_display_info(m_displayRef, &info);

    if (status != DDCRC_OK || !info) {
        qCWarning(POWERDEVIL) << "[DDCutilDisplay]: failed to get display info";
        return;
    }

    m_label = QString::fromLocal8Bit(info->model_name);
    m_ioPath = info->path;
    m_id = generatePathId(info->path);

    static_assert(sizeof(info->edid_bytes) == 128);
    std::span edid(info->edid_bytes, 128);
    m_edidData.assign(edid.begin(), edid.end());

    ddca_free_display_info(info);

    init();
#endif
}

// ============================================================
// Initialization
// ============================================================

void DDCutilDisplay::init()
{
#ifdef WITH_DDCUTIL
    QMutexLocker locker(m_openDisplayMutex);

    DDCA_Display_Handle handle = nullptr;
    DDCA_Status status = ddca_open_display2(m_displayRef, true, &handle);

    if (status != DDCRC_OK) {
        qCWarning(POWERDEVIL) << "[DDCutilDisplay]: open_display failed";
        return;
    }

    DDCA_Non_Table_Vcp_Value value;
    status = ddca_get_non_table_vcp_value(handle, BRIGHTNESS_VCP_FEATURE_CODE, &value);

    if (status == DDCRC_OK) {
        m_brightness = (value.sh << 8) | value.sl;
        m_maxBrightness = (value.mh << 8) | value.ml;
        m_supportsBrightness = true;
    }

    ddca_close_display(handle);

    if (!m_supportsBrightness) {
        qCDebug(POWERDEVIL) << "[DDCutilDisplay]: brightness not supported yet";
        return;
    }

    // --------------------------------------------------------
    // Timer setup (INLINE FIX — no setupRuntime() function)
    // --------------------------------------------------------

    m_timer->setSingleShot(true);
    m_timer->disconnect();

    connect(m_timer, &QTimer::timeout, this, &DDCutilDisplay::onSetBrightnessTimeout);

    Q_EMIT supportsBrightnessChanged(true);
#endif
}

// ============================================================
// Retry initialization
// ============================================================

void DDCutilDisplay::scheduleRetryInit()
{
    m_retryCounter = 0;

    m_timer->disconnect();

    connect(m_timer, &QTimer::timeout, this, &DDCutilDisplay::onInitRetryTimeout);

    m_timer->start(s_backoffRetryIntervals[0]);

    qCWarning(POWERDEVIL) << "[DDCutilDisplay]: retry init scheduled for" << m_label;
}

void DDCutilDisplay::onInitRetryTimeout()
{
#ifdef WITH_DDCUTIL
    if (!m_supportsBrightness) {
        init();
    }

    if (m_supportsBrightness) {
        Q_EMIT retryInitFinished(true);
        return;
    }

    if (++m_retryCounter < s_backoffRetryIntervals.size()) {
        m_timer->start(s_backoffRetryIntervals[m_retryCounter]);
        return;
    }
#endif

    Q_EMIT retryInitFinished(false);
}

// ============================================================
// Brightness control
// ============================================================

void DDCutilDisplay::setBrightness(int value, bool)
{
#ifdef WITH_DDCUTIL
    if (!m_supportsBrightness) {
        return;
    }

    m_brightness = value;
    m_timer->start(s_setBrightnessDelay);
#endif
}

void DDCutilDisplay::onSetBrightnessTimeout()
{
    Q_EMIT ddcBrightnessChangeRequested(m_brightness, this);
}

void DDCutilDisplay::ddcBrightnessChangeFinished(bool success)
{
    if (!success) {
        qCWarning(POWERDEVIL) << "[DDCutilDisplay]: failed brightness update for" << m_label;
        m_supportsBrightness = false;
        Q_EMIT supportsBrightnessChanged(false);
    }
}

// ============================================================
// Helpers
// ============================================================

DDCA_IO_Path DDCutilDisplay::ioPath() const
{
    return m_ioPath;
}

QString DDCutilDisplay::generatePathId(const DDCA_IO_Path &path)
{
    switch (path.io_mode) {
    case DDCA_IO_I2C:
        return QStringLiteral("i2c:%1").arg(path.path.i2c_busno);
    case DDCA_IO_USB:
        return QStringLiteral("usb:%1").arg(path.path.hiddev_devno);
    }
    return {};
}

// ============================================================
// Destructor
// ============================================================

DDCutilDisplay::~DDCutilDisplay()
{
    m_timer->stop();
}

// ============================================================
// Getters
// ============================================================

QString DDCutilDisplay::id() const
{
    return m_id;
}
QString DDCutilDisplay::label() const
{
    return m_label;
}
int DDCutilDisplay::brightness() const
{
    return m_brightness;
}
int DDCutilDisplay::maxBrightness() const
{
    return m_maxBrightness;
}
bool DDCutilDisplay::supportsBrightness() const
{
    return m_supportsBrightness;
}
bool DDCutilDisplay::usesDdcCi() const
{
    return true;
}

std::optional<QByteArray> DDCutilDisplay::edidData() const
{
#ifdef WITH_DDCUTIL
    return m_edidData;
#else
    return std::nullopt;
#endif
}

#include "moc_ddcutildisplay.cpp"