#include "MultiStarTracker.h"

#include <algorithm>
#include <cmath>

namespace guiding {

static inline double hypot2(double x, double y)
{
    return std::hypot(x, y);
}

// Strict centroid: return false when no pixels pass the threshold (sumW==0).
// This is important for secondary stars; otherwise we would happily "lock" to noise peaks.
static bool FindCentroidWeightedStrict(const cv::Mat& image16,
                                       const QPointF& approx,
                                       int halfSize,
                                       QPointF& out,
                                       double kSigma)
{
    if (image16.empty()) return false;

    const int cx = static_cast<int>(std::llround(approx.x()));
    const int cy = static_cast<int>(std::llround(approx.y()));
    const int x0 = std::max(0, cx - halfSize);
    const int y0 = std::max(0, cy - halfSize);
    const int x1 = std::min(image16.cols - 1, cx + halfSize);
    const int y1 = std::min(image16.rows - 1, cy + halfSize);
    if (x1 <= x0 || y1 <= y0) return false;

    cv::Mat roi = image16(cv::Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
    cv::Mat f;
    roi.convertTo(f, CV_32F);

    cv::Scalar mean, stddev;
    cv::meanStdDev(f, mean, stddev);
    const double thr = mean[0] + kSigma * stddev[0];

    double sumW = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;

    for (int yy = 0; yy < f.rows; ++yy)
    {
        const float* row = f.ptr<float>(yy);
        for (int xx = 0; xx < f.cols; ++xx)
        {
            const float v = row[xx];
            if (v < thr) continue;
            const double w = std::max(0.0, static_cast<double>(v) - static_cast<double>(mean[0]));
            sumW += w;
            sumX += (x0 + xx) * w;
            sumY += (y0 + yy) * w;
        }
    }

    if (sumW <= 0.0)
        return false;

    out = QPointF(sumX / sumW, sumY / sumW);
    return true;
}

double MultiStarTracker::SlidingStats::sigma() const
{
    if (v.size() < 2) return 0.0;
    double mean = 0.0;
    for (double x : v) mean += x;
    mean /= static_cast<double>(v.size());
    double var = 0.0;
    for (double x : v)
    {
        const double d = x - mean;
        var += d * d;
    }
    var /= static_cast<double>(v.size() - 1);
    return std::sqrt(std::max(0.0, var));
}

void MultiStarTracker::reset()
{
    m_stars.clear();
    m_primaryDistStats.clear();
    m_stabilizing = false;
    m_lockPositionMoved = false;
    m_referencePointsUpdated = false;
    m_maxStars = 9;
    m_stabilitySigmaX = 5.0;
    m_searchHalfSizePx = 8;
}

void MultiStarTracker::setReferenceStars(const std::vector<MultiGuideStar>& stars,
                                        int maxStars,
                                        double stabilitySigmaX,
                                        int searchHalfSizePx)
{
    m_stars = stars;
    if (m_stars.size() > static_cast<size_t>(std::max(1, maxStars)))
        m_stars.resize(static_cast<size_t>(std::max(1, maxStars)));

    m_maxStars = std::max(1, maxStars);
    m_stabilitySigmaX = stabilitySigmaX;
    m_searchHalfSizePx = std::max(2, searchHalfSizePx);

    m_primaryDistStats.clear();
    m_stabilizing = true; // collect a few frames before enabling secondary stars (PHD2-style)
    m_lockPositionMoved = false;
    m_referencePointsUpdated = true; // reference points were set/changed
}

void MultiStarTracker::notifyLockPositionMoved()
{
    m_lockPositionMoved = true;
    m_stabilizing = true;
}

QVector<QPointF> MultiStarTracker::secondaryReferencePoints() const
{
    QVector<QPointF> out;
    if (m_stars.size() <= 1)
        return out;
    out.reserve(static_cast<int>(m_stars.size()) - 1);
    for (size_t i = 1; i < m_stars.size(); ++i)
        out.push_back(m_stars[i].referencePoint);
    return out;
}

bool MultiStarTracker::consumeReferencePointsUpdatedFlag()
{
    const bool v = m_referencePointsUpdated;
    m_referencePointsUpdated = false;
    return v;
}

MultiStarRefineResult MultiStarTracker::refineOffset(const cv::Mat& image16,
                                                     const QPointF& primaryPos,
                                                     const QPointF& singleOffset)
{
    MultiStarRefineResult r;
    r.totalStars = static_cast<int>(m_stars.size());
    r.starsUsed = 1;
    r.refinedOffset = singleOffset;
    r.stabilizing = m_stabilizing;

    if (m_stars.size() < 2)
    {
        r.reason = MultiStarRefineResult::NoRefineReason::NotEnoughReferenceStars;
        return r;
    }

    // If lock position was moved during pre-guiding steps (calibration/backlash),
    // immediately refresh secondary reference points on the first guiding frame.
    // This ensures circles/offsets are consistent with the new lock position.
    if (m_lockPositionMoved)
    {
        m_lockPositionMoved = false;
        for (size_t i = 1; i < m_stars.size(); ++i)
        {
            auto& s = m_stars[i];
            const QPointF expectedLoc = primaryPos + s.offsetFromPrimary;
            QPointF c;
            const bool found = FindCentroidWeightedStrict(image16, expectedLoc, m_searchHalfSizePx, c, 2.0);
            if (found)
            {
                s.referencePoint = c;
                s.lastPoint = c;
                s.wasLost = false;
                s.missCount = 0;
                s.zeroCount = 0;
            }
            else
            {
                s.wasLost = true;
            }
        }
        m_referencePointsUpdated = true;
        r.reason = MultiStarRefineResult::NoRefineReason::LockMovedRefreshReference;
        // Do not refine on this frame; reference points now reflect current positions.
        return r;
    }

    // Primary distance statistics (for stabilization + outlier detection)
    const double primaryDistance = hypot2(singleOffset.x(), singleOffset.y());
    r.primaryDistancePx = primaryDistance;
    m_primaryDistStats.push(primaryDistance);

    if (m_primaryDistStats.count() <= 5)
    {
        m_stabilizing = true;
        r.stabilizing = true;
        r.reason = MultiStarRefineResult::NoRefineReason::NotEnoughPrimaryStats;
        return r;
    }

    const double primarySigma = m_primaryDistStats.sigma();
    r.primarySigmaPx = primarySigma;
    const double sigmaEff = std::max(primarySigma, m_minPrimarySigmaPx);
    r.primarySigmaEffPx = sigmaEff;
    r.stabilityEnterThreshPx = m_stabilitySigmaX * sigmaEff;
    r.outlierRejectThreshPx = 2.5 * sigmaEff;
    r.lastStabEnterDistPx = m_lastStabEnterDistPx;
    r.lastStabEnterThreshPx = m_lastStabEnterThreshPx;

    if (primarySigma > 0.0)
    {
        if (!m_stabilizing && primaryDistance > m_stabilitySigmaX * sigmaEff)
        {
            m_stabilizing = true;
            r.stabilizing = true;
            r.diag = "MultiStar: enter stabilization";
            r.reason = MultiStarRefineResult::NoRefineReason::EnterStabilization;
            m_lastStabEnterDistPx = primaryDistance;
            m_lastStabEnterSigmaEffPx = sigmaEff;
            m_lastStabEnterThreshPx = m_stabilitySigmaX * sigmaEff;
            r.lastStabEnterDistPx = m_lastStabEnterDistPx;
            r.lastStabEnterThreshPx = m_lastStabEnterThreshPx;
        }
        else if (m_stabilizing && primaryDistance <= 2.0 * sigmaEff)
        {
            m_stabilizing = false;
            r.stabilizing = false;
            r.diag = "MultiStar: exit stabilization";

            // If lock position changed, refresh reference points of secondary stars (PHD2-style).
            if (m_lockPositionMoved && m_stars.size() > 1)
            {
                m_lockPositionMoved = false;
                for (size_t i = 1; i < m_stars.size(); ++i)
                {
                    auto& s = m_stars[i];
                    const QPointF expectedLoc = primaryPos + s.offsetFromPrimary;
                    QPointF c;
                    const bool found = FindCentroidWeightedStrict(image16, expectedLoc, m_searchHalfSizePx, c, 2.0);
                    if (found)
                    {
                        s.referencePoint = c;
                        s.lastPoint = c;
                        s.wasLost = false;
                        s.missCount = 0;
                        s.zeroCount = 0;
                    }
                    else
                    {
                        s.wasLost = true;
                    }
                }
                m_referencePointsUpdated = true;
                // Reference points updated; don't refine on this frame.
                r.reason = MultiStarRefineResult::NoRefineReason::LockMovedRefreshReference;
                return r;
            }
        }
    }
    else
    {
        // sigma == 0: likely early frames; keep stabilizing behavior conservative.
        m_stabilizing = true;
        r.stabilizing = true;
        r.reason = MultiStarRefineResult::NoRefineReason::NotEnoughPrimaryStats;
        return r;
    }

    if (m_stabilizing)
    {
        r.stabilizing = true;
        r.reason = MultiStarRefineResult::NoRefineReason::InStabilization;
        return r;
    }

    if (singleOffset.isNull())
    {
        r.reason = MultiStarRefineResult::NoRefineReason::PrimaryOffsetZero;
        return r;
    }

    // Weighted sum starts with primary star (weight=1)
    double sumWeights = 1.0;
    double sumX = singleOffset.x();
    double sumY = singleOffset.y();

    const double primarySNR = std::max(1e-6, m_stars[0].snr);
    int validStars = 0; // secondaries used in average

    auto approxEq0 = [](double x) { return std::abs(x) < 1e-6; };

    QString diag;
    diag.reserve(256);
    diag += "MultiStar: ";

    // Iterate secondary stars with possible erasures
    for (size_t i = 1; i < m_stars.size(); /*increment inside*/)
    {
        if (r.starsUsed >= m_maxStars)
            break;

        auto& s = m_stars[i];

        const QPointF approx = s.wasLost ? (primaryPos + s.offsetFromPrimary) : s.lastPoint;

        QPointF c;
        bool found = FindCentroidWeightedStrict(image16, approx, m_searchHalfSizePx, c, 2.0);
        if (!found && m_searchHalfSizePx < 24)
        {
            // small escalation for recovery
            found = FindCentroidWeightedStrict(image16, approx, std::min(24, m_searchHalfSizePx * 2), c, 2.0);
        }

        if (!found)
        {
            s.wasLost = true;
            r.secondaryLost++;
            diag += QString("[#%1 L] ").arg(static_cast<int>(i));
            ++i;
            continue;
        }

        s.wasLost = false;
        s.lastPoint = c;
        r.secondaryFound++;
        r.starsUsed++;

        const double dX = c.x() - s.referencePoint.x();
        const double dY = c.y() - s.referencePoint.y();

        // Handle suspicious zero-like movements (hot pixels)
        if (approxEq0(dX) || approxEq0(dY))
            ++s.zeroCount;
        else if (s.zeroCount > 0)
            --s.zeroCount;

        if (s.zeroCount >= 5)
        {
            diag += QString("[#%1 DZ] ").arg(static_cast<int>(i));
            m_stars.erase(m_stars.begin() + static_cast<long>(i));
            r.secondaryRejected++;
            // do not increment i
            continue;
        }

        const double secondaryDistance = hypot2(dX, dY);
        if (secondaryDistance > 2.5 * sigmaEff)
        {
            if (++s.missCount > 10)
            {
                // Reset reference point to current position (PHD2-style)
                s.referencePoint = c;
                s.missCount = 0;
                diag += QString("[#%1 R dx=%2 dy=%3] ").arg(static_cast<int>(i)).arg(dX, 0, 'f', 2).arg(dY, 0, 'f', 2);
            }
            else
            {
                diag += QString("[#%1 M%2] ").arg(static_cast<int>(i)).arg(static_cast<int>(s.missCount));
            }
            r.secondaryRejected++;
            ++i;
            continue;
        }
        else if (s.missCount > 0)
        {
            --s.missCount;
        }

        const double wt = std::max(0.0, s.snr / primarySNR);
        sumX += wt * dX;
        sumY += wt * dY;
        sumWeights += wt;
        validStars++;
        r.secondaryUsed++;
        diag += QString("[#%1 U w=%2] ").arg(static_cast<int>(i)).arg(wt, 0, 'f', 2);

        ++i;
    }

    r.diag = diag;

    if (validStars <= 0 || sumWeights <= 0.0)
    {
        r.reason = MultiStarRefineResult::NoRefineReason::NoValidSecondaryStars;
        return r;
    }

    const double avgX = sumX / sumWeights;
    const double avgY = sumY / sumWeights;

    // Conservative apply: use refined offset only if it is smaller than primary offset (PHD2-style)
    if (hypot2(avgX, avgY) < primaryDistance)
    {
        r.refined = true;
        r.refinedOffset = QPointF(avgX, avgY);
        r.reason = MultiStarRefineResult::NoRefineReason::None;
    }
    else
    {
        r.reason = MultiStarRefineResult::NoRefineReason::RefinedNotSmallerThanSingle;
    }

    return r;
}

} // namespace guiding

