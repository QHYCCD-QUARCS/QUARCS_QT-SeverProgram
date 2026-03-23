#pragma once

// PHD2 mount calibration state machine port (subset focused on pulse-guiding mounts)
// Reference: PHD2 `scope.cpp` (`Scope::BeginCalibration` / `Scope::UpdateCalibrationState`)
// License: BSD (see PHD2 source headers)

#include "../GuiderTypes.h"

#include <QPointF>
#include <QString>
#include <cmath>
#include <vector>

namespace guiding::phd2 {

struct CalibrationStepResult
{
    bool hasPulse = false;
    PulseCommand pulse{};

    bool done = false;
    bool failed = false;
    QString infoMessage;
    QString errorMessage;
    CalibrationResult result{};
};

class MountCalibration
{
public:
    void reset();
    bool isActive() const { return m_active; }

    // Start a calibration sequence. `calibrationDurationMs` is PHD2 "Calibration step size".
    void start(const QPointF& lockPosPx, double calibrationDistancePx, int calibrationDurationMs);

    // Advance the state machine with the latest centroid (camera coords, pixels).
    CalibrationStepResult onCentroid(const QPointF& currentPosPx);

private:
    enum class State
    {
        Cleared,
        GoWest,
        GoEast,
        ClearBacklash,
        GoNorth,
        GoSouth,
        Complete,
    };

    static inline QPointF toVec(const QPointF& a, const QPointF& b) { return QPointF(b.x() - a.x(), b.y() - a.y()); }
    static inline double dist(const QPointF& a, const QPointF& b)
    {
        const double dx = b.x() - a.x();
        const double dy = b.y() - a.y();
        return std::hypot(dx, dy);
    }

private:
    CalibrationStepResult fail(const QString& msg);
    CalibrationResult finalize() const;

private:
    bool m_active = false;
    State m_state = State::Cleared;

    // PHD2 settings
    double m_distancePx = 25.0;    // dist_crit
    int m_durationMs = 500;        // m_calibrationDuration
    int m_maxSteps = 60;           // MAX_CALIBRATION_STEPS safety

    // dynamic
    int m_steps = 0;
    int m_recenterRemainingMs = 0;
    int m_recenterDurationMs = 0;

    // locations
    QPointF m_initialLock{0.0, 0.0};
    QPointF m_start{0.0, 0.0};         // current axis start
    QPointF m_last{0.0, 0.0};
    QPointF m_eastStart{0.0, 0.0};
    QPointF m_southStart{0.0, 0.0};

    // backlash clearing
    QPointF m_blMarker{0.0, 0.0};
    double m_blExpectedStepPx = 0.0;
    int m_blAcceptedMoves = 0;
    int m_blMaxClearingPulses = 0;
    double m_blLastCumDistance = 0.0;

    // derived calibration quantities (PHD2: xAngle/xRate, yAngle/yRate)
    double m_xAngleRad = 0.0;
    double m_yAngleRad = 0.0;
    double m_xRatePxPerMs = 0.0;
    double m_yRatePxPerMs = 0.0;

    // for output
    int m_raSteps = 0;
    int m_decSteps = 0;
    double m_raTravelPx = 0.0;
    double m_decTravelPx = 0.0;

    // diagnostics: centroid samples during the two main axis runs
    // - RA samples collected during GoWest
    // - DEC samples collected during GoNorth
    std::vector<QPointF> m_raSamples;
    std::vector<QPointF> m_decSamples;
};

} // namespace guiding::phd2

