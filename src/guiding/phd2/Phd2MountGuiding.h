#pragma once

#include "../GuiderTypes.h"
#include "Phd2GuideAlgorithms.h"

#include <optional>

namespace guiding::phd2 {

// PHD2-like guide step computation:
// 1) convert camera error -> RA/DEC axis coords (px) using calibration basis
// 2) run PHD2 algorithms (Hysteresis for RA, ResistSwitch for DEC by default)
// 3) convert algorithm output (px) -> pulse ms using calibration rates
class MountGuiding
{
public:
    struct Output
    {
        std::optional<PulseCommand> pulse;
        double raErrPx = 0.0;
        double decErrPx = 0.0;
        double raAlgoOutPx = 0.0;
        double decAlgoOutPx = 0.0;

        // Extra diagnostics for higher-level policies (e.g. DEC uni-direction auto-correction)
        bool raNeed = false;
        bool decNeed = false;
        bool raAllowed = false;
        bool decAllowed = false;
        GuideDir raDir = GuideDir::West;
        GuideDir decDir = GuideDir::North;
        int raMs = 0;
        int decMs = 0;
    };

    MountGuiding();

    void reset();

    // run one guiding update
    Output compute(const CalibrationResult& calib,
                   const GuidingParams& params,
                   const QPointF& lockPos,
                   const QPointF& currentPos);

private:
    // err = current - lock decomposed in basis raUnit/decUnit -> coords (px)
    static bool decomposeInBasis(const QPointF& err,
                                 const QPointF& raUnit,
                                 const QPointF& decUnit,
                                 double& raCoord,
                                 double& decCoord);

private:
    GuideAlgorithmHysteresis m_raAlgo;
    GuideAlgorithmResistSwitch m_decAlgo;
};

} // namespace guiding::phd2

