#include "Phd2MountCalibration.h"

#include <algorithm>
#include <QStringList>
#include <cmath>

namespace guiding::phd2 {

static inline double angleRad(const QPointF& from, const QPointF& to)
{
    // In PHD2: Angle uses dX = from.X - to.X, dY = from.Y - to.Y
    const double dx = from.x() - to.x();
    const double dy = from.y() - to.y();
    if (dx == 0.0 && dy == 0.0)
        return 0.0;
    return std::atan2(dy, dx);
}

static inline double normAngle(double a)
{
    while (a <= -M_PI) a += 2.0 * M_PI;
    while (a >  M_PI) a -= 2.0 * M_PI;
    return a;
}

static inline double dot2(const QPointF& a, const QPointF& b)
{
    return a.x() * b.x() + a.y() * b.y();
}

static inline double norm2(const QPointF& v)
{
    return std::hypot(v.x(), v.y());
}

static QString axisFitDiagnostics(const QString& axisName,
                                 const QPointF& start,
                                 const std::vector<QPointF>& samples,
                                 const QPointF& end)
{
    if (samples.size() < 2)
        return QString();
    const QPointF dir = end - start;
    const double len = norm2(dir);
    if (!std::isfinite(len) || len < 1e-6)
        return QString();
    const QPointF u(dir.x() / len, dir.y() / len);

    // FitErr = RMS of perpendicular distances to the axis line (through start, direction u)
    double sumPerp2 = 0.0;
    for (const auto& p : samples)
    {
        const QPointF d = p - start;
        const double proj = dot2(d, u);
        const QPointF perp(d.x() - proj * u.x(), d.y() - proj * u.y());
        const double pd = norm2(perp);
        sumPerp2 += pd * pd;
    }
    const double rms = std::sqrt(std::max(0.0, sumPerp2 / (double)samples.size()));

    // Also include a per-step projection summary: proj(perp) for each sample
    QStringList parts;
    parts << QStringLiteral("%1 fitErrRms=%2px samples=%3").arg(axisName).arg(rms, 0, 'f', 3).arg((int)samples.size());
    for (int i = 0; i < (int)samples.size(); ++i)
    {
        const QPointF d = samples[i] - start;
        const double proj = dot2(d, u);
        const QPointF perp(d.x() - proj * u.x(), d.y() - proj * u.y());
        const double pd = norm2(perp);
        parts << QStringLiteral("i=%1 dx=%2 dy=%3 proj=%4 perp=%5")
                     .arg(i)
                     .arg(d.x(), 0, 'f', 2)
                     .arg(d.y(), 0, 'f', 2)
                     .arg(proj, 0, 'f', 2)
                     .arg(pd, 0, 'f', 3);
    }
    // Use '; ' to keep it single-line in most log sinks
    return parts.join(QStringLiteral("; "));
}

void MountCalibration::reset()
{
    m_active = false;
    m_state = State::Cleared;
    m_steps = 0;
    m_recenterRemainingMs = 0;
    m_recenterDurationMs = 0;
    m_blAcceptedMoves = 0;
    m_blMaxClearingPulses = 0;
    m_blLastCumDistance = 0.0;
    m_xAngleRad = 0.0;
    m_yAngleRad = 0.0;
    m_xRatePxPerMs = 0.0;
    m_yRatePxPerMs = 0.0;
    m_raSteps = 0;
    m_decSteps = 0;
    m_raTravelPx = 0.0;
    m_decTravelPx = 0.0;
    m_raSamples.clear();
    m_decSamples.clear();
}

void MountCalibration::start(const QPointF& lockPosPx, double calibrationDistancePx, int calibrationDurationMs)
{
    reset();
    m_active = true;
    m_state = State::GoWest;
    m_initialLock = lockPosPx;
    m_start = lockPosPx;
    m_last = lockPosPx;
    m_distancePx = std::max(1.0, calibrationDistancePx);
    m_durationMs = std::max(50, calibrationDurationMs);
    m_steps = 0;
    m_raSamples.clear();
    m_decSamples.clear();
}

CalibrationStepResult MountCalibration::fail(const QString& msg)
{
    CalibrationStepResult r;
    r.failed = true;
    r.done = true;
    r.errorMessage = msg;
    m_active = false;
    m_state = State::Cleared;
    return r;
}

CalibrationResult MountCalibration::finalize() const
{
    CalibrationResult out;
    out.valid = false;
    if (m_xRatePxPerMs <= 0.0 || m_yRatePxPerMs <= 0.0)
        return out;

    // Convert PHD2 xAngle/yAngle to our unit vectors:
    // - PHD2 angles are based on (start - current) vector. Our guiding basis expects movement direction (current - start),
    //   so add pi (negate).
    const double raMoveAng = normAngle(m_xAngleRad + M_PI);
    const double decMoveAng = normAngle(m_yAngleRad + M_PI);

    out.raUnitVec = QPointF(std::cos(raMoveAng), std::sin(raMoveAng));
    out.decUnitVec = QPointF(std::cos(decMoveAng), std::sin(decMoveAng));

    out.raRatePxPerSec = m_xRatePxPerMs * 1000.0;
    out.decRatePxPerSec = m_yRatePxPerMs * 1000.0;
    out.raMsPerPixel = (m_xRatePxPerMs > 0.0) ? (1.0 / m_xRatePxPerMs) : 0.0;
    out.decMsPerPixel = (m_yRatePxPerMs > 0.0) ? (1.0 / m_yRatePxPerMs) : 0.0;

    out.cameraAngleDeg = normAngle(m_xAngleRad) * 180.0 / M_PI;
    // ortho error like PHD2 details: delta from nearest 90deg
    const double delta = std::fabs(std::fabs(normAngle(m_xAngleRad - m_yAngleRad)) - M_PI / 2.0);
    out.orthoErrDeg = delta * 180.0 / M_PI;

    out.raStepCount = m_raSteps;
    out.decStepCount = m_decSteps;
    out.raTravelPx = m_raTravelPx;
    out.decTravelPx = m_decTravelPx;

    out.valid = true;
    return out;
}

CalibrationStepResult MountCalibration::onCentroid(const QPointF& currentPosPx)
{
    if (!m_active)
        return fail(QStringLiteral("CalibrationNotActive"));

    CalibrationStepResult r;

    auto appendInfo = [&](const QString& s) {
        if (s.isEmpty()) return;
        if (r.infoMessage.isEmpty())
            r.infoMessage = s;
        else
            r.infoMessage += QStringLiteral(" | ") + s;
    };

    // initialize per-axis start position from first centroid if needed
    if (m_steps == 0 && m_state == State::GoWest)
        m_start = currentPosPx;

    const double d = dist(m_start, currentPosPx);
    const double distCrit = m_distancePx;

    switch (m_state)
    {
    case State::GoWest:
    {
        // collect RA samples (GoWest path)
        m_raSamples.push_back(currentPosPx);

        // per-step diagnostics: Δx/Δy/dist
        const QPointF dxy = currentPosPx - m_start;
        appendInfo(QStringLiteral("CAL step=%1 state=GoWest dx=%2 dy=%3 dist=%4/%5px")
                       .arg(m_steps)
                       .arg(dxy.x(), 0, 'f', 2)
                       .arg(dxy.y(), 0, 'f', 2)
                       .arg(d, 0, 'f', 2)
                       .arg(distCrit, 0, 'f', 2));

        if (d < distCrit)
        {
            if (m_steps++ > m_maxSteps)
                return fail(QStringLiteral("RA Calibration Failed: star did not move enough"));
            r.hasPulse = true;
            r.pulse = PulseCommand{GuideDir::West, m_durationMs};
            return r;
        }

        // West complete: compute xAngle/xRate
        m_xAngleRad = angleRad(m_start, currentPosPx);
        m_xRatePxPerMs = d / (double)(std::max(1, m_steps) * m_durationMs);
        m_raSteps = m_steps;
        m_raTravelPx = d;

        // axis diagnostics summary (proj/perp + fitErr)
        appendInfo(axisFitDiagnostics(QStringLiteral("CALDBG_RA"), m_start, m_raSamples, currentPosPx));

        // Setup recenter East
        m_recenterRemainingMs = m_steps * m_durationMs;
        m_recenterDurationMs = m_durationMs;
        m_steps = (m_recenterRemainingMs + m_recenterDurationMs - 1) / m_recenterDurationMs;
        m_state = State::GoEast;
        m_eastStart = currentPosPx;
        m_last = currentPosPx;
        // fall through
    }
    [[fallthrough]];
    case State::GoEast:
    {
        if (m_recenterRemainingMs > 0)
        {
            int dur = m_recenterDurationMs;
            if (dur > m_recenterRemainingMs) dur = m_recenterRemainingMs;
            m_recenterRemainingMs -= dur;
            --m_steps;
            r.hasPulse = true;
            r.pulse = PulseCommand{GuideDir::East, dur};
            m_last = currentPosPx;
            return r;
        }

        // Prepare for backlash clearing / north calibration
        m_steps = 0;
        m_start = currentPosPx;
        m_blMarker = currentPosPx;
        m_blExpectedStepPx = (m_xRatePxPerMs * m_durationMs) * 0.6; // PHD2 heuristic
        m_blMaxClearingPulses = std::max(8, 60000 / std::max(1, m_durationMs));
        m_blLastCumDistance = 0.0;
        m_blAcceptedMoves = 0;

        m_state = State::ClearBacklash;
        // fall through
    }
    [[fallthrough]];
    case State::ClearBacklash:
    {
        // Want to see mount moving north for 3 moves >= expected distance without direction reversals
        const double blDelta = dist(m_blMarker, currentPosPx);
        const double blCum = dist(m_start, currentPosPx);

        if (m_steps == 0)
        {
            r.hasPulse = true;
            r.pulse = PulseCommand{GuideDir::North, m_durationMs};
            m_steps = 1;
            return r;
        }

        if (blDelta >= m_blExpectedStepPx)
        {
            if (m_blAcceptedMoves == 0 || (blCum > m_blLastCumDistance))
                m_blAcceptedMoves++;
            else
                m_blAcceptedMoves = 0; // direction reversal
        }
        else
        {
            if (blCum < m_blLastCumDistance)
                m_blAcceptedMoves = 0;
        }

        if (m_blAcceptedMoves < 3)
        {
            if (m_steps < m_blMaxClearingPulses && blCum < distCrit)
            {
                r.hasPulse = true;
                r.pulse = PulseCommand{GuideDir::North, m_durationMs};
                m_steps++;
                m_blMarker = currentPosPx;
                m_blLastCumDistance = blCum;
                return r;
            }

            // out of attempts - accept if moved enough
            if (blCum < 3.0)
                return fail(QStringLiteral("Backlash Clearing Failed: star did not move enough"));

            // proceed anyway
            m_steps = 0;
            m_start = currentPosPx;
        }
        else
        {
            // Use last clearing move as step 1 of north calibration
            m_steps = 1;
            m_start = m_blMarker;
        }

        m_state = State::GoNorth;
        // fall through
    }
    [[fallthrough]];
    case State::GoNorth:
    {
        // collect DEC samples (GoNorth path)
        m_decSamples.push_back(currentPosPx);

        const double dn = dist(m_start, currentPosPx);
        const QPointF dxy = currentPosPx - m_start;
        appendInfo(QStringLiteral("CAL step=%1 state=GoNorth dx=%2 dy=%3 dist=%4/%5px")
                       .arg(m_steps)
                       .arg(dxy.x(), 0, 'f', 2)
                       .arg(dxy.y(), 0, 'f', 2)
                       .arg(dn, 0, 'f', 2)
                       .arg(distCrit, 0, 'f', 2));
        if (dn < distCrit)
        {
            if (m_steps++ > m_maxSteps)
                return fail(QStringLiteral("DEC Calibration Failed: star did not move enough"));
            r.hasPulse = true;
            r.pulse = PulseCommand{GuideDir::North, m_durationMs};
            return r;
        }

        m_yAngleRad = angleRad(m_start, currentPosPx);
        m_yRatePxPerMs = dn / (double)(std::max(1, m_steps) * m_durationMs);
        m_decSteps = m_steps;
        m_decTravelPx = dn;

        appendInfo(axisFitDiagnostics(QStringLiteral("CALDBG_DEC"), m_start, m_decSamples, currentPosPx));

        // Recenter South
        m_recenterRemainingMs = m_steps * m_durationMs;
        m_recenterDurationMs = m_durationMs;
        m_steps = (m_recenterRemainingMs + m_recenterDurationMs - 1) / m_recenterDurationMs;
        m_state = State::GoSouth;
        m_southStart = currentPosPx;
        m_last = currentPosPx;
        // fall through
    }
    [[fallthrough]];
    case State::GoSouth:
    {
        if (m_recenterRemainingMs > 0)
        {
            int dur = m_recenterDurationMs;
            if (dur > m_recenterRemainingMs) dur = m_recenterRemainingMs;
            m_recenterRemainingMs -= dur;
            --m_steps;
            r.hasPulse = true;
            r.pulse = PulseCommand{GuideDir::South, dur};
            m_last = currentPosPx;
            return r;
        }

        m_state = State::Complete;
        // fall through
    }
    [[fallthrough]];
    case State::Complete:
    {
        r.done = true;
        r.result = finalize();
        r.failed = !r.result.valid;
        r.errorMessage = r.failed ? QStringLiteral("CalibrationFailed:InvalidResult") : QString{};
        m_active = false;
        return r;
    }

    case State::Cleared:
    default:
        return fail(QStringLiteral("CalibrationStateError"));
    }
}

} // namespace guiding::phd2

