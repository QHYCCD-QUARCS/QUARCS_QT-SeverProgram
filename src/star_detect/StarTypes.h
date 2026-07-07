#pragma once

#include <optional>
#include <string>
#include <vector>

namespace star_detect {

enum class RejectReason
{
    None,
    LowSnr,
    CentroidFailed,
    Duplicate,
};

struct DetectedStar
{
    double x = 0.0;
    double y = 0.0;

    double fullX = 0.0;
    double fullY = 0.0;

    double snr = 0.0;
    double hfd = 0.0;
    double hfr = 0.0;
    double peakADU = 0.0;
    double flux = 0.0;
    double bgMean = 0.0;
    double bgStd = 0.0;
    double edgeDistPx = 0.0;

    bool saturated = false;
    bool centroidRefined = false;
};

struct RejectedStar
{
    DetectedStar star;
    RejectReason reason = RejectReason::None;
    std::string detail;
};

struct DetectionDebugInfo
{
    std::string summary;
    int rawPeakCount = 0;
    int dedupCount = 0;
    int snrPassedCount = 0;
    int validCount = 0;
    int rejectedCount = 0;
};

struct DetectionResult
{
    std::vector<DetectedStar> allCandidates;
    std::vector<DetectedStar> dedupCandidates;
    std::vector<DetectedStar> snrCandidates;
    std::vector<DetectedStar> validCandidates;
    std::vector<RejectedStar> rejectedCandidates;
    DetectionDebugInfo debug;
};

enum class SelectionMode
{
    HighestSnr,
    Brightest,
    NearestToPoint,
    BestGuideStar,
    BestFocusStar,
};

struct SelectedStar
{
    DetectedStar star;
    int sourceIndex = -1;
    double score = 0.0;
    std::string reason;
};

struct SelectionResult
{
    bool success = false;
    std::optional<SelectedStar> selected;
    std::vector<DetectedStar> rankedCandidates;
    std::string summary;
};

} // namespace star_detect
