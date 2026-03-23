#include "Phd2MountGuiding.h"

#include <algorithm>
#include <cmath>

namespace guiding::phd2 {

MountGuiding::MountGuiding()
{
    reset();
}

void MountGuiding::reset()
{
    m_raAlgo.reset();
    m_decAlgo.reset();
}

bool MountGuiding::decomposeInBasis(const QPointF& err,
                                   const QPointF& raUnit,
                                   const QPointF& decUnit,
                                   double& raCoord,
                                   double& decCoord)
{
    const double a = raUnit.x();
    const double b = decUnit.x();
    const double c = raUnit.y();
    const double d = decUnit.y();
    const double det = a * d - b * c;
    if (std::abs(det) < 1e-10)
        return false;
    raCoord  = ( err.x() * d - b * err.y() ) / det;
    decCoord = ( a * err.y() - err.x() * c ) / det;
    return true;
}

MountGuiding::Output MountGuiding::compute(const CalibrationResult& calib,
                                          const GuidingParams& params,
                                          const QPointF& lockPos,
                                          const QPointF& currentPos)
{
    Output out;
    if (!calib.valid)
        return out;

    // PHD2 conventions: guide algorithms operate on mount-coords distances in pixels.
    // Here we use RA/DEC axis coordinates (px) derived from calibration basis.
    // Error vector: current - lock
    const QPointF err = currentPos - lockPos;

    double raErr = 0.0, decErr = 0.0;
    if (!decomposeInBasis(err, calib.raUnitVec, calib.decUnitVec, raErr, decErr))
    {
        // fallback to dot projection (rare)
        raErr = err.x() * calib.raUnitVec.x() + err.y() * calib.raUnitVec.y();
        decErr = err.x() * calib.decUnitVec.x() + err.y() * calib.decUnitVec.y();
    }

    out.raErrPx = raErr;
    out.decErrPx = decErr;

    // Configure algorithms from params (map existing params to PHD2-like knobs)
    const double minMove = std::max(0.0, params.deadbandPx);
    m_raAlgo.setMinMove(minMove);
    m_raAlgo.setAggression(std::max(0.0, params.raAggression));
    m_raAlgo.setHysteresis(params.enableRaHysteresis ? std::clamp(params.raHysteresis, 0.0, 1.0) : 0.0);

    m_decAlgo.setMinMove(minMove);
    m_decAlgo.setAggression(std::max(0.0, params.decAggression));

    // PHD2 uses sign to pick direction, then duration from px/(px/ms)
    // IMPORTANT (PHD2 sign convention):
    // Guide algorithms operate on the *correction* in mount coords, not the raw star error.
    // Our raErr/decErr are "current - lock" in axis coords, so the correction is the negative.
    out.raAlgoOutPx = m_raAlgo.result(-raErr);
    out.decAlgoOutPx = m_decAlgo.result(-decErr);

    // Convert px -> ms using calibration rates (ms/px) to avoid division by tiny xRate
    auto pxToMs = [&](double px, double msPerPixel) -> int {
        if (!std::isfinite(px) || !std::isfinite(msPerPixel) || msPerPixel <= 0.0)
            return 0;
        const double ms = std::fabs(px) * msPerPixel;
        double clamped = ms;
        if (clamped < params.minPulseMs) clamped = params.minPulseMs;
        if (clamped > params.maxPulseMs) clamped = params.maxPulseMs;
        return (int)std::llround(clamped);
    };

    const int raMs = pxToMs(out.raAlgoOutPx, calib.raMsPerPixel);
    const int decMs = pxToMs(out.decAlgoOutPx, calib.decMsPerPixel);

    const bool raNeed = raMs > 0 && std::fabs(out.raAlgoOutPx) >= minMove;
    const bool decNeed = decMs > 0 && std::fabs(out.decAlgoOutPx) >= minMove;

    // Direction mapping:
    // - calib RA positive is WEST (by our calibration definition)
    // - if algoOutPx positive -> pulse WEST, negative -> pulse EAST
    const GuideDir raDir = (out.raAlgoOutPx >= 0.0) ? GuideDir::West : GuideDir::East;
    const GuideDir decDir = (out.decAlgoOutPx >= 0.0) ? GuideDir::North : GuideDir::South;

    // Apply allowed-dir gating (keep existing behavior)
    const bool raAllowed = raNeed && (params.allowedRaDirs.count(raDir) > 0);
    const bool decAllowed = decNeed && (params.allowedDecDirs.count(decDir) > 0);

    // Expose details for higher-level policies (GuiderCore)
    out.raNeed = raNeed;
    out.decNeed = decNeed;
    out.raAllowed = raAllowed;
    out.decAllowed = decAllowed;
    out.raDir = raDir;
    out.decDir = decDir;
    out.raMs = raMs;
    out.decMs = decMs;

    // Multi-axis pulses in same frame: follow existing enableMultiAxisPulses (sequential in GuiderCore)
    // Here we emit only one pulse; GuiderCore may call again or allow multi-axis in future refactor.
    if (params.enableMultiAxisPulses)
    {
        // prioritize axis with larger ms demand
        if (raAllowed && (!decAllowed || raMs >= decMs))
            out.pulse = PulseCommand{raDir, raMs};
        else if (decAllowed)
            out.pulse = PulseCommand{decDir, decMs};
    }
    else
    {
        // Single-axis mode: still avoid starving DEC when RA always has small residual error.
        // Choose the axis with larger correction demand when both are allowed.
        if (raAllowed && decAllowed)
            out.pulse = (raMs >= decMs) ? PulseCommand{raDir, raMs} : PulseCommand{decDir, decMs};
        else if (raAllowed)
            out.pulse = PulseCommand{raDir, raMs};
        else if (decAllowed)
            out.pulse = PulseCommand{decDir, decMs};
    }

    return out;
}

} // namespace guiding::phd2

