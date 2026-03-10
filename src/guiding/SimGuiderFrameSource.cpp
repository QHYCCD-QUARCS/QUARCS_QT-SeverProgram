#include "SimGuiderFrameSource.h"

#include <QFile>
#include <QtGlobal>

#include <cmath>
#include <optional>
#include <random>
#include <vector>

#include <fitsio.h>
#include <opencv2/core/core.hpp>

namespace guiding {

static inline double clampd(double v, double lo, double hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

SimGuiderFrameSource::SimGuiderFrameSource()
    : SimGuiderFrameSource(Params{})
{
}

SimGuiderFrameSource::SimGuiderFrameSource(const Params& p)
    : m_p(p), m_starPosPx(p.startPosPx)
{
}

SimGuiderFrameSource::Params SimGuiderFrameSource::params() const
{
    QMutexLocker lk(&m_mutex);
    return m_p;
}

void SimGuiderFrameSource::setParams(const Params& p)
{
    QMutexLocker lk(&m_mutex);
    m_p = p;
    if (!m_started)
        m_starPosPx = p.startPosPx;
}

QPointF SimGuiderFrameSource::currentStarPosPx() const
{
    QMutexLocker lk(&m_mutex);
    return m_starPosPx;
}

void SimGuiderFrameSource::injectPulse(const PulseCommand& cmd)
{
    QMutexLocker lk(&m_mutex);

    // pulseMs -> px，映射遵循常见导星图像坐标：
    // x 右为正；y 下为正。
    // - West  : x +=
    // - East  : x -=
    // - North : y -=
    // - South : y +=
    double px = 0.0;
    switch (cmd.dir)
    {
    case GuideDir::West:
    case GuideDir::East:
        px = (m_p.raMsPerPixel > 1e-6) ? (static_cast<double>(cmd.durationMs) / m_p.raMsPerPixel) : 0.0;
        if (cmd.dir == GuideDir::West) m_pendingShiftPx.rx() += px;
        else m_pendingShiftPx.rx() -= px;
        break;
    case GuideDir::North:
    case GuideDir::South:
    {
        // ===== DEC 回差模型 =====
        // 当 DEC 方向反转时，先消耗 m_p.decBacklashMs 的“无效脉冲时长”，星点不动。
        int effectiveMs = cmd.durationMs;
        if (m_p.enableDecBacklash)
        {
            const GuideDir cur = cmd.dir;
            if (m_lastDecDir.has_value() && *m_lastDecDir != cur)
            {
                // 方向反转：重置剩余回差
                m_decBacklashRemainMs = std::max(0, m_p.decBacklashMs);
            }
            m_lastDecDir = cur;

            if (m_decBacklashRemainMs > 0)
            {
                const int eat = std::min(m_decBacklashRemainMs, effectiveMs);
                m_decBacklashRemainMs -= eat;
                effectiveMs -= eat;
            }
        }

        px = (m_p.decMsPerPixel > 1e-6) ? (static_cast<double>(effectiveMs) / m_p.decMsPerPixel) : 0.0;
        if (cmd.dir == GuideDir::North) m_pendingShiftPx.ry() -= px;
        else m_pendingShiftPx.ry() += px;
        break;
    }
    }
}

void SimGuiderFrameSource::applyPendingShift()
{
    // 轻微限制单帧跃迁，避免质心算法受冲击
    const double maxStep = 50.0;
    m_pendingShiftPx.setX(clampd(m_pendingShiftPx.x(), -maxStep, maxStep));
    m_pendingShiftPx.setY(clampd(m_pendingShiftPx.y(), -maxStep, maxStep));

    m_starPosPx += m_pendingShiftPx;
    m_pendingShiftPx = QPointF(0.0, 0.0);
}

void SimGuiderFrameSource::initStarFieldIfNeeded()
{
    if (m_fieldReady)
        return;

    m_fieldStars.clear();
    m_fieldStars.reserve(std::max(1, m_p.starCount));

    // 0) 导星星点：固定在 startPos（basePos），整体场随其漂移/脉冲移动
    FieldStar guide;
    guide.basePosPx = m_p.startPosPx;
    guide.peak = m_p.starPeak;
    guide.sigmaPx = m_p.psfSigmaPx;
    guide.radiusPx = m_p.psfRadiusPx;
    m_fieldStars.push_back(guide);

    // 1) 其它背景星：随机分布，亮度/PSF 多样化，仿照真实导星画面“多星场”
    std::mt19937 rng(m_p.randomSeed);
    std::uniform_real_distribution<double> ux(0.0, 1.0);

    auto randIn = [&](double a, double b) {
        return a + (b - a) * ux(rng);
    };

    const int total = std::max(1, m_p.starCount);
    const double margin = 30.0;
    for (int i = 1; i < total; ++i)
    {
        FieldStar s;
        s.basePosPx = QPointF(
            randIn(margin, m_p.width - 1 - margin),
            randIn(margin, m_p.height - 1 - margin)
        );

        // 亮度分布：绝大多数较暗，少量较亮（但避免近饱和）
        const double r = ux(rng);
        if (r < 0.80) {
            s.peak = static_cast<uint16_t>(std::llround(randIn(1500.0, 9000.0)));
            s.sigmaPx = randIn(1.2, 2.0);
        } else if (r < 0.97) {
            s.peak = static_cast<uint16_t>(std::llround(randIn(9000.0, 22000.0)));
            s.sigmaPx = randIn(1.6, 2.6);
        } else {
            s.peak = static_cast<uint16_t>(std::llround(randIn(22000.0, 52000.0)));
            s.sigmaPx = randIn(2.2, 3.6);
        }

        s.radiusPx = std::max(6, static_cast<int>(std::llround(s.sigmaPx * 5.0)));
        m_fieldStars.push_back(s);
    }

    m_fieldReady = true;
}

void SimGuiderFrameSource::drawStarGaussian(cv::Mat& img16,
                                           const QPointF& posPx,
                                           uint16_t peak,
                                           double sigmaPx,
                                           int radiusPx)
{
    const int cx = static_cast<int>(std::llround(posPx.x()));
    const int cy = static_cast<int>(std::llround(posPx.y()));
    const double sigma2 = sigmaPx * sigmaPx;
    const int R = radiusPx;

    for (int dy = -R; dy <= R; ++dy)
    {
        const int yy = cy + dy;
        if (yy < 0 || yy >= img16.rows) continue;
        auto* row = img16.ptr<uint16_t>(yy);
        for (int dx = -R; dx <= R; ++dx)
        {
            const int xx = cx + dx;
            if (xx < 0 || xx >= img16.cols) continue;
            const double r2 = static_cast<double>(dx * dx + dy * dy);
            const double w = std::exp(-r2 / (2.0 * sigma2));
            const int add = static_cast<int>(std::llround(static_cast<double>(peak) * w));
            int v = static_cast<int>(row[xx]) + add;
            if (v > 65535) v = 65535;
            row[xx] = static_cast<uint16_t>(v);
        }
    }
}

cv::Mat SimGuiderFrameSource::renderFrame16U(double dtSec)
{
    initStarFieldIfNeeded();

    // 生成噪声背景
    cv::Mat img(m_p.height, m_p.width, CV_16UC1);
    img.setTo(cv::Scalar(static_cast<int>(m_p.noiseMean)));

    // 轻微渐变（模拟真实画面中的亮度不均匀/暗角）
    const double cx = (m_p.width - 1) * 0.5;
    const double cy = (m_p.height - 1) * 0.5;
    const double maxR = std::sqrt(cx * cx + cy * cy);
    for (int y = 0; y < img.rows; ++y)
    {
        auto* row = img.ptr<uint16_t>(y);
        for (int x = 0; x < img.cols; ++x)
        {
            const double dx = x - cx;
            const double dy = y - cy;
            const double r = std::sqrt(dx * dx + dy * dy) / maxR;
            const double vignette = 1.0 - 0.35 * r; // 边缘更暗
            int v = static_cast<int>(std::llround(static_cast<double>(row[x]) * vignette));
            if (v < 0) v = 0;
            row[x] = static_cast<uint16_t>(v);
        }
    }

    // 高斯噪声
    cv::Mat noise16s(m_p.height, m_p.width, CV_16SC1);
    cv::randn(noise16s, 0.0, m_p.noiseStd);
    for (int y = 0; y < img.rows; ++y)
    {
        auto* dst = img.ptr<uint16_t>(y);
        const auto* n = noise16s.ptr<int16_t>(y);
        for (int x = 0; x < img.cols; ++x)
        {
            int v = static_cast<int>(dst[x]) + static_cast<int>(n[x]);
            if (v < 0) v = 0;
            if (v > 65535) v = 65535;
            dst[x] = static_cast<uint16_t>(v);
        }
    }

    // 漂移（右上：x+ y-）
    m_starPosPx.rx() += m_p.driftPxPerSec.x() * dtSec;
    m_starPosPx.ry() += m_p.driftPxPerSec.y() * dtSec;

    applyPendingShift();

    // 防止跑到边缘外（留出PSF半径）
    const double margin = static_cast<double>(m_p.psfRadiusPx + 2);
    m_starPosPx.setX(clampd(m_starPosPx.x(), margin, m_p.width - 1 - margin));
    m_starPosPx.setY(clampd(m_starPosPx.y(), margin, m_p.height - 1 - margin));

    // 整片星场随导星星点一起移动：delta = currentGuide - startGuide
    const QPointF delta = m_starPosPx - m_p.startPosPx;
    for (const auto& s : m_fieldStars)
    {
        const QPointF pos = s.basePosPx + delta;
        drawStarGaussian(img, pos, s.peak, s.sigmaPx, s.radiusPx);
    }

    return img;
}

bool SimGuiderFrameSource::writeFits16U(const QString& path, const cv::Mat& image16)
{
    if (image16.empty() || image16.type() != CV_16UC1)
        return false;

    // cfitsio overwrite: prefix '!'
    QString fitsName = path;
    if (!fitsName.startsWith("!"))
        fitsName = "!" + fitsName;

    fitsfile* fptr = nullptr;
    int status = 0;

    const long naxes[2] = { image16.cols, image16.rows };
    if (fits_create_file(&fptr, fitsName.toUtf8().constData(), &status))
    {
        if (fptr) fits_close_file(fptr, &status);
        return false;
    }

    if (fits_create_img(fptr, USHORT_IMG, 2, const_cast<long*>(naxes), &status))
    {
        fits_close_file(fptr, &status);
        return false;
    }

    // 写入像素数据（按行连续）
    const long fpixel = 1;
    const long nelements = static_cast<long>(naxes[0] * naxes[1]);
    if (fits_write_img(fptr, TUSHORT, fpixel, nelements,
                       const_cast<uint16_t*>(image16.ptr<uint16_t>(0)), &status))
    {
        fits_close_file(fptr, &status);
        return false;
    }

    // 基本头关键字（可选）
    fits_update_key(fptr, TSTRING, const_cast<char*>("OBJECT"),
                    const_cast<char*>("SIM_GUIDER"), const_cast<char*>("Sim guider frame"), &status);

    fits_close_file(fptr, &status);
    return status == 0;
}

QString SimGuiderFrameSource::generateNextFrame(int exposureMs)
{
    QMutexLocker lk(&m_mutex);

    if (!m_started)
    {
        m_started = true;
        m_starPosPx = m_p.startPosPx;
    }

    const double dtSec = qMax(0.0, static_cast<double>(exposureMs) / 1000.0);
    cv::Mat frame16 = renderFrame16U(dtSec);
    if (!writeFits16U(m_p.fitsPath, frame16))
        return QString();
    return m_p.fitsPath;
}

} // namespace guiding


