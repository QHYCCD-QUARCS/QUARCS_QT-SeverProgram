#include "DecBacklashEstimator.h"

#include <algorithm>
#include <cmath>

namespace guiding {

static inline double dot(const QPointF& a, const QPointF& b)
{
    return a.x() * b.x() + a.y() * b.y();
}

void DecBacklashEstimator::reset()
{
    m_active = false;
    m_phase = Phase::PushNorth;
    m_hasFirst = false;
    m_startDecErrPx = 0.0;
    m_extremeDecErrPx = 0.0;
    m_pushSign = 1;
    m_northTotalMs = 0;
    m_southTotalMs = 0;
    m_consecutiveMoveFrames = 0;
    m_backlashMsEstimated = -1;
    m_reportedAutoMax = false;
}

void DecBacklashEstimator::start(const CalibrationResult& calib, const GuidingParams& params, const QPointF& lockPosPx)
{
    reset();
    m_active = true;
    m_calib = calib;
    m_params = params;
    m_lockPosPx = lockPosPx;
    m_phase = Phase::PushNorth;
}

DecBacklashEstimator::Result DecBacklashEstimator::onFrame(const QPointF& centroidPx)
{
    Result r;
    if (!m_active || !m_calib.valid)
        return r;

    const QPointF err = centroidPx - m_lockPosPx;
    const double decErrPx = dot(err, m_calib.decUnitVec);

    if (!m_hasFirst)
    {
        m_hasFirst = true;
        m_startDecErrPx = decErrPx;
        m_extremeDecErrPx = decErrPx;
        m_pushSign = 1;
        return r;
    }

    // 更新“极值”（离 start 最远的点）与 pushSign
    const double curDelta = decErrPx - m_startDecErrPx;
    const double bestDelta = m_extremeDecErrPx - m_startDecErrPx;
    if (std::abs(curDelta) > std::abs(bestDelta))
    {
        m_extremeDecErrPx = decErrPx;
        m_pushSign = (curDelta >= 0.0) ? 1 : -1;
    }

    const int northPulseMs = std::max(1, m_params.decBacklashNorthPulseMs);
    int northMaxTotalMs = std::max(0, m_params.decBacklashNorthMaxTotalMs);
    const double northTargetPx = std::max(0.0, m_params.decBacklashNorthTargetPx);

    const int probeStepMs = std::max(1, m_params.decBacklashProbeStepMs);
    const int probeMaxTotalMs = std::max(0, m_params.decBacklashProbeMaxTotalMs);
    const double detectMovePx = std::max(0.05, m_params.decBacklashDetectMovePx);
    const int detectConsec = std::max(1, m_params.decBacklashDetectConsecutiveFrames);

    // Auto widen NORTH push timeout based on calibration speed (ms/px).
    // This avoids frequent NorthPushTimeout when the mount/guide-rate is slow.
    if (northTargetPx > 0.0 && m_calib.decMsPerPixel > 0.0)
    {
        // Required time to move northTargetPx at the calibrated speed.
        const double requiredMs = northTargetPx * m_calib.decMsPerPixel;
        // Add safety margin for seeing/noise/settling; cap to 60s to avoid runaway.
        const int autoMax = std::min(60000, (int) std::ceil(requiredMs * 1.35));
        if (autoMax > northMaxTotalMs)
        {
            if (!m_reportedAutoMax && m_phase == Phase::PushNorth && m_northTotalMs == 0)
            {
                r.infoMessage = QStringLiteral("DEC回差测量：根据校准速度自动放宽 NORTH 超时 %1ms → %2ms（decMs/px=%3, target=%4px）")
                                    .arg(northMaxTotalMs)
                                    .arg(autoMax)
                                    .arg(m_calib.decMsPerPixel, 0, 'f', 2)
                                    .arg(northTargetPx, 0, 'f', 1);
                m_reportedAutoMax = true;
            }
            northMaxTotalMs = autoMax;
        }
    }

    if (m_phase == Phase::PushNorth)
    {
        const double movedAbsPx = std::abs(decErrPx - m_startDecErrPx); // 不假设符号：只看绝对位移
        r.infoMessage = QStringLiteral("DEC回差测量(PushNorth): moved=%1/%2px northTotal=%3ms northMax=%4ms decMs/px=%5")
                            .arg(movedAbsPx, 0, 'f', 2)
                            .arg(northTargetPx, 0, 'f', 2)
                            .arg(m_northTotalMs)
                            .arg(northMaxTotalMs)
                            .arg(m_calib.decMsPerPixel, 0, 'f', 2);
        if (movedAbsPx >= northTargetPx)
        {
            m_phase = Phase::ProbeSouth;
            m_consecutiveMoveFrames = 0;
            r.infoMessage = QStringLiteral("DEC回差测量：已完成NORTH预加载（moved=%1px），开始SOUTH探测...")
                                .arg(movedAbsPx, 0, 'f', 2);
            return r;
        }

        if (northMaxTotalMs > 0 && m_northTotalMs >= northMaxTotalMs)
        {
            r.done = true;
            r.failed = true;
            r.errorMessage = QStringLiteral("DecBacklashMeasureFailed:NorthPushTimeout(totalMs=%1, movedAbs=%2px)")
                                 .arg(m_northTotalMs)
                                 .arg(movedAbsPx, 0, 'f', 2);
            m_active = false;
            return r;
        }

        // 继续 NORTH 推
        r.hasPulse = true;
        r.pulse.dir = GuideDir::North;
        r.pulse.durationMs = std::min(northPulseMs, m_params.maxPulseMs);
        m_northTotalMs += r.pulse.durationMs;
        return r;
    }

    if (m_phase == Phase::ProbeSouth)
    {
        // Important: do not "detect movement" before we've sent any SOUTH probe pulse.
        // Between switching phases and issuing the first SOUTH pulse, seeing/centroid noise can
        // make the star appear to move back toward the extreme, which would bias the estimate
        // toward 0ms. Always send at least one probe pulse first.
        if (m_southTotalMs <= 0)
        {
            if (probeMaxTotalMs > 0 && m_southTotalMs >= probeMaxTotalMs)
            {
                r.done = true;
                r.failed = true;
                r.errorMessage = QStringLiteral("DecBacklashMeasureFailed:SouthProbeTimeout(totalMs=%1, pulled=%2px)")
                                     .arg(m_southTotalMs)
                                     .arg(0.0, 0, 'f', 3);
                m_active = false;
                return r;
            }

            // Send the first SOUTH probe pulse
            r.hasPulse = true;
            r.pulse.dir = GuideDir::South;
            r.pulse.durationMs = std::min(probeStepMs, m_params.maxPulseMs);
            m_southTotalMs += r.pulse.durationMs;
            return r;
        }

        // SOUTH 回拉：直到看到明显“回拉位移”（从 NORTH 预加载的极值开始向回走）
        // m_pushSign 表示预加载时 decErr-start 的符号：
        // - pushSign=+1：极值是 max，回拉时 decErr 变小 → pulled = extreme - decErr
        // - pushSign=-1：极值是 min，回拉时 decErr 变大 → pulled = decErr - extreme
        const int pushSign = (m_pushSign == 0) ? 1 : m_pushSign;
        const double pulledPx = (-pushSign) * (decErrPx - m_extremeDecErrPx);
        r.infoMessage = QStringLiteral("DEC回差测量(ProbeSouth): pulled=%1px thresh=%2px consec=%3/%4 southTotal=%5ms")
                            .arg(pulledPx, 0, 'f', 3)
                            .arg(detectMovePx, 0, 'f', 3)
                            .arg(m_consecutiveMoveFrames)
                            .arg(detectConsec)
                            .arg(m_southTotalMs);
        if (pulledPx >= detectMovePx)
        {
            m_consecutiveMoveFrames++;
            if (m_consecutiveMoveFrames == 1 && m_backlashMsEstimated < 0)
            {
                // First time reaching threshold: backlash is less than or equal to the total
                // SOUTH probing time used so far. Using m_southTotalMs (not minus one step)
                // avoids systematically reporting 0ms when the first probe step already moves.
                m_backlashMsEstimated = std::max(0, m_southTotalMs);
            }
        }
        else
        {
            m_consecutiveMoveFrames = 0;
        }

        if (m_consecutiveMoveFrames >= detectConsec)
        {
            r.done = true;
            r.failed = false;
            r.backlashMs = (m_backlashMsEstimated >= 0) ? m_backlashMsEstimated : m_southTotalMs;
            r.infoMessage = QStringLiteral("DEC回差测量完成：backlash=%1ms（southTotal=%2ms, pulled=%3px）")
                                .arg(r.backlashMs)
                                .arg(m_southTotalMs)
                                .arg(pulledPx, 0, 'f', 3);
            m_active = false;
            m_phase = Phase::Done;
            return r;
        }

        if (probeMaxTotalMs > 0 && m_southTotalMs >= probeMaxTotalMs)
        {
            r.done = true;
            r.failed = true;
            r.errorMessage = QStringLiteral("DecBacklashMeasureFailed:SouthProbeTimeout(totalMs=%1, pulled=%2px)")
                                 .arg(m_southTotalMs)
                                 .arg(pulledPx, 0, 'f', 3);
            m_active = false;
            return r;
        }

        // 继续 SOUTH 探测
        r.hasPulse = true;
        r.pulse.dir = GuideDir::South;
        r.pulse.durationMs = std::min(probeStepMs, m_params.maxPulseMs);
        m_southTotalMs += r.pulse.durationMs;
        return r;
    }

    return r;
}

} // namespace guiding

