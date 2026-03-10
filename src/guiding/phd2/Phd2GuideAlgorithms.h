#pragma once

// PHD2 algorithm ports (BSD license in PHD2 source tree)
// This module intentionally mirrors PHD2 behavior to match guiding results.

#include <deque>
#include <cmath>

namespace guiding::phd2 {

class IGuideAlgorithm
{
public:
    virtual ~IGuideAlgorithm() = default;
    virtual void reset() = 0;
    // input/output are in "mount coordinates" pixels (same units as PHD2 xDistance/yDistance)
    virtual double result(double input) = 0;
};

// ===== Hysteresis (RA typical) =====
// PHD2 reference: guide_algorithm_hysteresis.*
class GuideAlgorithmHysteresis final : public IGuideAlgorithm
{
public:
    // minMove in pixels
    void setMinMove(double v) { m_minMove = v; }
    void setAggression(double v) { m_aggression = v; }
    void setHysteresis(double v) { m_hysteresis = v; } // 0..1

    void reset() override
    {
        m_lastMove = 0.0;
    }

    double result(double input) override
    {
        // Match PHD2 logic:
        //   dReturn = (1-h)*input + h*lastMove
        //   dReturn *= aggression
        //   if |input| < minMove => dReturn = 0
        //   lastMove = dReturn
        double dReturn = (1.0 - m_hysteresis) * input + m_hysteresis * m_lastMove;
        dReturn *= m_aggression;
        if (std::fabs(input) < m_minMove)
            dReturn = 0.0;
        m_lastMove = dReturn;
        return dReturn;
    }

private:
    double m_minMove = 0.2;
    double m_aggression = 1.0;
    double m_hysteresis = 0.7;
    double m_lastMove = 0.0;
};

// ===== Resist Switch (DEC typical) =====
// PHD2 reference: guide_algorithm_resistswitch.*
class GuideAlgorithmResistSwitch final : public IGuideAlgorithm
{
public:
    void setMinMove(double v) { m_minMove = v; }
    void setAggression(double v) { m_aggression = v; }
    void setFastSwitchEnabled(bool v) { m_fastSwitchEnabled = v; }
    void setDriftLeak(double v) { m_driftLeak = v; }
    void setDriftTriggerFactor(double v) { m_driftTriggerFactor = v; }

    void reset() override
    {
        m_history.clear();
        while (m_history.size() < HISTORY_SIZE)
            m_history.push_back(0.0);
        m_currentSide = 0;
        m_driftAccum = 0.0;
    }

    double result(double input) override
    {
        // Convert sub-minMove steady drift into an occasional correction.
        double effective = input;
        if (std::fabs(input) < m_minMove)
        {
            // leaky integrator
            m_driftAccum = (m_driftAccum * m_driftLeak) + input;
            const double trigger = m_minMove * m_driftTriggerFactor;
            if (std::fabs(m_driftAccum) >= trigger)
            {
                effective = m_driftAccum;
                m_driftAccum = 0.0;
            }
            else
            {
                effective = 0.0;
            }
        }
        else
        {
            m_driftAccum = 0.0;
        }

        double rslt = effective;

        // update history
        m_history.push_back(effective);
        if (m_history.size() > HISTORY_SIZE)
            m_history.pop_front();

        auto sgn = [](double x) -> int {
            if (x > 0.0) return 1;
            if (x < 0.0) return -1;
            return 0;
        };

        try
        {
            if (std::fabs(effective) < m_minMove)
                throw 1;

            if (m_fastSwitchEnabled)
            {
                const double thresh = 3.0 * m_minMove;
                if (sgn(effective) != m_currentSide && std::fabs(effective) > thresh)
                {
                    // force switch
                    m_currentSide = 0;
                    size_t i = 0;
                    for (; i + 3 < m_history.size(); ++i)
                        m_history[i] = 0.0;
                    for (; i < m_history.size(); ++i)
                        m_history[i] = effective;
                }
            }

            int decHistory = 0;
            for (double v : m_history)
            {
                if (std::fabs(v) > m_minMove)
                    decHistory += sgn(v);
            }

            if (m_currentSide == 0 || sgn(m_currentSide) == -sgn(decHistory))
            {
                auto calcSlope = [&]() -> double {
                    const size_t n = m_history.size();
                    if (n < 2) return 0.0;
                    double sumT = 0.0, sumTT = 0.0, sumX = 0.0, sumTX = 0.0;
                    const double t0 = (double)(n - 1) / 2.0;
                    for (size_t i = 0; i < n; i++)
                    {
                        const double t = (double)i - t0;
                        const double x = m_history[i];
                        sumT += t;
                        sumTT += t * t;
                        sumX += x;
                        sumTX += t * x;
                    }
                    const double denom = (double)n * sumTT - sumT * sumT;
                    if (denom == 0.0) return 0.0;
                    return ((double)n * sumTX - sumT * sumX) / denom;
                };

                if (std::abs(decHistory) < 3)
                {
                    const double slope = calcSlope();
                    const double slopeThresh = 0.10 * m_minMove; // px/sample
                    if (!(std::abs(decHistory) >= 2 && std::fabs(slope) >= slopeThresh && sgn(slope) == sgn(decHistory)))
                        throw 2;
                }

                double oldest = 0.0, newest = 0.0;
                for (int i = 0; i < 3 && (size_t)i < m_history.size(); i++)
                {
                    oldest += m_history[i];
                    newest += m_history[m_history.size() - (size_t)(i + 1)];
                }
                if (std::fabs(newest) <= std::fabs(oldest))
                    throw 3;

                m_currentSide = sgn((double)decHistory);
            }

            if (m_currentSide != sgn(effective))
                throw 4; // overshot
        }
        catch (...)
        {
            rslt = 0.0;
        }

        rslt *= m_aggression;
        return rslt;
    }

private:
    static constexpr size_t HISTORY_SIZE = 10;
    std::deque<double> m_history;
    double m_minMove = 0.2;
    double m_aggression = 1.0;
    bool m_fastSwitchEnabled = true;
    int m_currentSide = 0;
    double m_driftAccum = 0.0;
    double m_driftLeak = 0.85;
    double m_driftTriggerFactor = 1.0;
};

} // namespace guiding::phd2

