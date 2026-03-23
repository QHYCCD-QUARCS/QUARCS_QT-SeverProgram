#pragma once

#include <QPointF>
#include <QString>
#include <QVector>

#include <opencv2/core/core.hpp>

#include <deque>
#include <vector>

namespace guiding {

struct MultiGuideStar
{
    QPointF referencePoint{0.0, 0.0};     // reference position (locked at selection time)
    QPointF lastPoint{0.0, 0.0};          // last found position (for next-frame search)
    QPointF offsetFromPrimary{0.0, 0.0};  // referencePoint - primaryReferencePoint
    double snr = 0.0;                     // relative weight (snr_i / snr_primary)

    unsigned int missCount = 0;
    unsigned int zeroCount = 0;
    bool wasLost = false;
};

struct MultiStarRefineResult
{
    enum class NoRefineReason : int
    {
        None = 0,
        NotEnoughReferenceStars,
        NotEnoughPrimaryStats,
        EnterStabilization,
        InStabilization,
        LockMovedRefreshReference,
        PrimaryOffsetZero,
        NoValidSecondaryStars,
        RefinedNotSmallerThanSingle,
    };

    bool refined = false;          // whether refinedOffset should be used
    QPointF refinedOffset{0.0, 0.0};
    int starsUsed = 1;             // how many stars were tracked this frame (>=1, includes primary)
    int totalStars = 1;            // reference stars count (includes primary)
    QString diag;                  // optional debug string (throttled by caller)

    NoRefineReason reason = NoRefineReason::None;
    bool stabilizing = false;
    int secondaryFound = 0;        // number of secondary stars found this frame (regardless of acceptance)
    int secondaryUsed = 0;         // number of secondary stars used in average
    int secondaryLost = 0;         // number of secondary stars not found
    int secondaryRejected = 0;     // number rejected due to outlier/zero/hot pixel/etc.
    double primaryDistancePx = 0.0;
    double primarySigmaPx = 0.0;
    double primarySigmaEffPx = 0.0;
    double stabilityEnterThreshPx = 0.0;
    double outlierRejectThreshPx = 0.0;
    double lastStabEnterDistPx = 0.0;
    double lastStabEnterThreshPx = 0.0;
};

// PHD2-style multistar refineOffset:
// - Primary offset is always computed by the caller (single-star).
// - Secondary stars are used to produce a weighted average offset.
// - The refined offset is applied only when it reduces the offset magnitude.
class MultiStarTracker
{
public:
    void reset();

    bool hasReferenceStars() const { return m_stars.size() >= 2; }
    int referenceStarCount() const { return static_cast<int>(m_stars.size()); }
    QVector<QPointF> secondaryReferencePoints() const;
    bool consumeReferencePointsUpdatedFlag();

    // Initialize reference stars list. Index 0 must be the primary star.
    void setReferenceStars(const std::vector<MultiGuideStar>& stars,
                           int maxStars,
                           double stabilitySigmaX,
                           int searchHalfSizePx);

    // Notify that lock position has moved (e.g. after calibration). This mimics PHD2 behavior:
    // after stabilization exit, we will attempt to update secondary reference points.
    void notifyLockPositionMoved();

    // Refine a single-star offset using secondary stars.
    // - image16: guider frame
    // - primaryPos: current primary centroid position
    // - singleOffset: primaryPos - lockPos
    MultiStarRefineResult refineOffset(const cv::Mat& image16,
                                       const QPointF& primaryPos,
                                       const QPointF& singleOffset);

private:
    struct SlidingStats
    {
        std::deque<double> v;
        size_t maxN = 50;

        void clear() { v.clear(); }
        size_t count() const { return v.size(); }

        void push(double x)
        {
            v.push_back(x);
            if (v.size() > maxN)
                v.pop_front();
        }

        double sigma() const;
    };

private:
    std::vector<MultiGuideStar> m_stars;  // [0]=primary, [1..]=secondary
    int m_maxStars = 9;
    double m_stabilitySigmaX = 5.0;
    int m_searchHalfSizePx = 8;
    double m_minPrimarySigmaPx = 0.6; // px, avoid overly aggressive outlier/stabilization when sigma is tiny (sim/very stable conditions)

    SlidingStats m_primaryDistStats;
    bool m_stabilizing = false;
    bool m_lockPositionMoved = false;
    bool m_referencePointsUpdated = false;

    // record last enter-stabilization threshold for diagnostics
    double m_lastStabEnterDistPx = 0.0;
    double m_lastStabEnterSigmaEffPx = 0.0;
    double m_lastStabEnterThreshPx = 0.0;
};

} // namespace guiding

