#include "../guiding/GuidingStarDetector.h"
#include "../guiding/phd2/Phd2MountGuiding.h"
#include "../guiding/phd2/Phd2MountCalibration.h"
#include "../guiding/DecBacklashEstimator.h"
#include "../guiding/GuiderCore.h"
#include "../guiding/MultiStarTracker.h"

#include <fitsio.h>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>

#include <QCoreApplication>
#include <QString>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

static void writeFits16(const std::string& path, const cv::Mat& img16)
{
    if (img16.empty() || img16.type() != CV_16UC1)
        throw std::runtime_error("writeFits16 expects CV_16UC1");

    fitsfile* fptr = nullptr;
    int status = 0;

    std::string out = "!" + path; // overwrite
    long naxes[2] = { img16.cols, img16.rows };
    fits_create_file(&fptr, out.c_str(), &status);
    fits_create_img(fptr, USHORT_IMG, 2, naxes, &status);
    long fpixel = 1;
    fits_write_img(fptr, TUSHORT, fpixel, naxes[0] * naxes[1],
                   const_cast<uint16_t*>(img16.ptr<uint16_t>()), &status);
    fits_close_file(fptr, &status);
    if (status)
    {
        char err_text[FLEN_STATUS];
        fits_get_errstatus(status, err_text);
        throw std::runtime_error(std::string("CFITSIO error: ") + err_text);
    }
}

static cv::Mat makeFrame(int w, int h, double starX, double starY, double sigmaPx, uint16_t peakADU)
{
    cv::Mat img(h, w, CV_16UC1, cv::Scalar(0));

    // 生成一个高斯星点（加少量背景噪声）
    for (int y = 0; y < h; ++y)
    {
        uint16_t* row = img.ptr<uint16_t>(y);
        for (int x = 0; x < w; ++x)
        {
            double dx = x - starX;
            double dy = y - starY;
            double g = std::exp(-(dx * dx + dy * dy) / (2.0 * sigmaPx * sigmaPx));
            double val = g * peakADU;
            // 简单噪声
            val += (std::rand() % 50);
            if (val > 65535.0) val = 65535.0;
            row[x] = static_cast<uint16_t>(val);
        }
    }
    return img;
}

static cv::Mat makeMultiStarFrame(int w,
                                  int h,
                                  const std::vector<QPointF>& stars,
                                  const std::vector<uint16_t>& peaks,
                                  double sigmaPx,
                                  int noiseAmp = 50)
{
    cv::Mat img(h, w, CV_16UC1, cv::Scalar(0));
    for (int y = 0; y < h; ++y)
    {
        uint16_t* row = img.ptr<uint16_t>(y);
        for (int x = 0; x < w; ++x)
        {
            double val = 0.0;
            for (size_t i = 0; i < stars.size(); ++i)
            {
                const double dx = x - stars[i].x();
                const double dy = y - stars[i].y();
                const double g = std::exp(-(dx * dx + dy * dy) / (2.0 * sigmaPx * sigmaPx));
                val += g * static_cast<double>(peaks[i]);
            }
            val += (std::rand() % std::max(1, noiseAmp));
            if (val > 65535.0) val = 65535.0;
            row[x] = static_cast<uint16_t>(val);
        }
    }
    return img;
}

static double rms(const std::vector<double>& v)
{
    if (v.empty()) return 0.0;
    double s2 = 0.0;
    for (double x : v) s2 += x * x;
    return std::sqrt(s2 / static_cast<double>(v.size()));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // 用法：
    // guiding_offline_test <outDir>
    std::string outDir = "/tmp/guiding_offline";
    if (argc >= 2) outDir = argv[1];
    std::filesystem::create_directories(outDir);

    std::srand(0);

    const int w = 640, h = 480;
    QPointF lock(320.0, 240.0);

    // ===== 1) 选星测试 =====
    {
        cv::Mat frame = makeFrame(w, h, lock.x(), lock.y(), 2.0, 40000);
        std::string p = outDir + "/select_000.fits";
        writeFits16(p, frame);

        guiding::GuidingStarDetector det;
        guiding::StarSelectionParams sp;
        sp.minSNR = 5.0; // 离线测试放宽一点
        auto best = det.selectGuideStar(frame, sp);
        if (best.has_value())
        {
            std::cout << "[Select] best x=" << best->x << " y=" << best->y
                      << " snr=" << best->snr << " hfd=" << best->hfd << "\n";
        }
        else
        {
            std::cout << "[Select] no star\n";
        }
    }

    // ===== 2) 校准测试（PHD2 状态机：GoWest->GoEast->ClearBacklash->GoNorth->GoSouth） =====
    guiding::phd2::MountCalibration calib;
    calib.start(lock, 25.0, 500);

    // 设定“脉冲响应”：
    // WEST：沿 10deg 方向移动，速度 0.05 px/ms → 500ms=25px
    // NORTH：沿 100deg 方向移动，速度 0.04 px/ms → 500ms=20px（需要多次脉冲）
    auto raUnit = QPointF(std::cos(10.0 * M_PI / 180.0), std::sin(10.0 * M_PI / 180.0));
    auto decUnit = QPointF(std::cos(100.0 * M_PI / 180.0), std::sin(100.0 * M_PI / 180.0));
    const double raPxPerMs = 0.05;
    const double decPxPerMs = 0.04;

    QPointF star = lock;
    guiding::CalibrationResult calibResult;

    for (int i = 0; i < 200; ++i)
    {
        auto step = calib.onCentroid(star);
        if (step.hasPulse)
        {
            if (step.pulse.dir == guiding::GuideDir::West)
                star += raUnit * (raPxPerMs * step.pulse.durationMs);
            else if (step.pulse.dir == guiding::GuideDir::East)
                star -= raUnit * (raPxPerMs * step.pulse.durationMs);
            else if (step.pulse.dir == guiding::GuideDir::North)
                star += decUnit * (decPxPerMs * step.pulse.durationMs);
            else if (step.pulse.dir == guiding::GuideDir::South)
                star -= decUnit * (decPxPerMs * step.pulse.durationMs);
        }
        if (step.done)
        {
            calibResult = step.result;
            break;
        }
    }

    if (calibResult.valid)
    {
        std::cout << "[Calib] cameraAngleDeg=" << calibResult.cameraAngleDeg
                  << " orthoErrDeg=" << calibResult.orthoErrDeg
                  << " raRatePxPerSec=" << calibResult.raRatePxPerSec
                  << " decRatePxPerSec=" << calibResult.decRatePxPerSec
                  << " raMsPerPixel=" << calibResult.raMsPerPixel
                  << " decMsPerPixel=" << calibResult.decMsPerPixel
                  << "\n";
    }
    else
    {
        std::cout << "[Calib] failed\n";
    }

    // ===== 3) 单向导星门控测试 =====
    if (calibResult.valid)
    {
        guiding::GuidingParams gp;
        // 默认：RA 只允许 WEST，DEC 只允许 NORTH
        gp.allowedRaDirs = {guiding::GuideDir::West};
        gp.allowedDecDirs = {guiding::GuideDir::North};
        gp.deadbandPx = 0.35;

        guiding::phd2::MountGuiding ctrl;

        // 制造一个“需要 EAST”才能纠正的 RA 误差：沿 raUnitVec 正向偏移 5px
        QPointF cur = lock + calibResult.raUnitVec * 5.0;
        auto out = ctrl.compute(calibResult, gp, lock, cur);
        std::cout << "[GuideGate] raErrPx=" << out.raErrPx << " decErrPx=" << out.decErrPx << "\n";
        if (out.pulse.has_value())
        {
            std::cout << "[GuideGate] pulse(dir=" << static_cast<int>(out.pulse->dir)
                      << ", ms=" << out.pulse->durationMs << ")\n";
        }
        else
        {
            std::cout << "[GuideGate] no pulse (expected: gated)\n";
        }
    }

    // ===== 3b) DEC 锁错方向 + RA 持续有误差：验证“不再饿死 DEC” =====
    // 目的：
    // - 当 DEC 单向锁定错误（例如只允许 NORTH）时，decNeed=true 但 decAllowed=false；
    //   这时上层策略应能检测并翻向 allowedDecDirs。
    // - 翻向后，即使 RA 仍有误差，也应优先发出“修正量更大”的轴，避免 DEC 长期不被纠正。
    if (calibResult.valid)
    {
        guiding::GuidingParams gp;
        gp.allowedRaDirs = {guiding::GuideDir::East, guiding::GuideDir::West};
        gp.allowedDecDirs = {guiding::GuideDir::North}; // 故意锁错：假设真实需要 SOUTH
        gp.deadbandPx = 0.10;
        gp.minPulseMs = 20;
        gp.maxPulseMs = 2000;
        gp.raAggression = 1.0;
        gp.decAggression = 1.0;

        guiding::phd2::MountGuiding ctrl;

        // 构造：DEC 误差显著（需要 SOUTH），RA 有一点小误差（避免“仅 DEC”过于理想）
        QPointF cur = lock + calibResult.decUnitVec * 10.0 + calibResult.raUnitVec * 1.0;
        auto out1 = ctrl.compute(calibResult, gp, lock, cur);
        std::cout << "[DecLockWrong] raErrPx=" << out1.raErrPx << " decErrPx=" << out1.decErrPx
                  << " raNeed=" << out1.raNeed << " decNeed=" << out1.decNeed
                  << " raAllowed=" << out1.raAllowed << " decAllowed=" << out1.decAllowed
                  << " decDir=" << static_cast<int>(out1.decDir) << "\n";

        // 模拟上层“自动翻向”为 out1.decDir（期望 SOUTH）
        gp.allowedDecDirs.clear();
        gp.allowedDecDirs.insert(out1.decDir);
        auto out2 = ctrl.compute(calibResult, gp, lock, cur);
        std::cout << "[DecLockFixed] pulse=" << (out2.pulse.has_value() ? "yes" : "no")
                  << " dir=" << (out2.pulse.has_value() ? std::to_string((int)out2.pulse->dir) : "NA")
                  << " ms=" << (out2.pulse.has_value() ? std::to_string(out2.pulse->durationMs) : "0")
                  << " (expect: DEC correction not starved)\n";
    }

    // ===== 4) DEC 回差测量测试（NORTH预加载→SOUTH探测，单位ms） =====
    if (calibResult.valid)
    {
        guiding::GuidingParams gp;
        gp.enableDecBacklashMeasure = true;
        gp.decBacklashNorthTargetPx = 20.0;
        gp.decBacklashNorthPulseMs = 300;
        gp.decBacklashNorthMaxTotalMs = 8000;
        gp.decBacklashProbeStepMs = 100;
        gp.decBacklashProbeMaxTotalMs = 6000;
        gp.decBacklashDetectMovePx = 0.4;
        gp.decBacklashDetectConsecutiveFrames = 2;

        const int trueDecBacklashMs = 400; // 模拟“从NORTH切到SOUTH时需要吃掉的回差”
        int backlashRemainMs = 0;
        std::optional<guiding::GuideDir> lastDecDir;

        guiding::DecBacklashEstimator est;
        est.start(calibResult, gp, lock);

        QPointF star2 = lock;
        int estBacklash = -1;

        for (int i = 0; i < 200; ++i)
        {
            // 生成帧并喂给测量器（测量器只看 centroid）
            auto step = est.onFrame(star2);
            if (step.hasPulse)
            {
                if (step.pulse.dir == guiding::GuideDir::North)
                {
                    // NORTH：直接移动
                    star2 += decUnit * (decPxPerMs * step.pulse.durationMs);
                    if (lastDecDir.has_value() && *lastDecDir != guiding::GuideDir::North)
                        backlashRemainMs = 0;
                    lastDecDir = guiding::GuideDir::North;
                }
                else if (step.pulse.dir == guiding::GuideDir::South)
                {
                    // SOUTH：若从NORTH反转过来，先吃回差
                    if (lastDecDir.has_value() && *lastDecDir == guiding::GuideDir::North && backlashRemainMs == 0)
                        backlashRemainMs = trueDecBacklashMs;
                    lastDecDir = guiding::GuideDir::South;

                    int effectiveMs = step.pulse.durationMs;
                    if (backlashRemainMs > 0)
                    {
                        const int eat = std::min(backlashRemainMs, effectiveMs);
                        backlashRemainMs -= eat;
                        effectiveMs -= eat;
                    }
                    if (effectiveMs > 0)
                        star2 -= decUnit * (decPxPerMs * effectiveMs);
                }
            }
            if (step.done)
            {
                if (!step.failed)
                    estBacklash = step.backlashMs;
                break;
            }
        }

        if (estBacklash >= 0)
        {
            std::cout << "[DecBacklash] trueMs=" << trueDecBacklashMs
                      << " estimatedMs=" << estBacklash
                      << " probeStepMs=" << gp.decBacklashProbeStepMs
                      << "\n";
        }
        else
        {
            std::cout << "[DecBacklash] failed\n";
        }
    }

    // ===== 5) MultiStar refineOffset 离线验证（RMS 更稳 + 丢星自动回退） =====
    {
        guiding::MultiStarTracker ms;

        // 构造 1 主 + 3 副参考星
        QPointF primaryRef(320.0, 240.0);
        std::vector<QPointF> refPts = {
            primaryRef,
            QPointF(200.0, 200.0),
            QPointF(450.0, 260.0),
            QPointF(300.0, 350.0),
        };
        std::vector<uint16_t> peaks = { 42000, 30000, 28000, 25000 };

        std::vector<guiding::MultiGuideStar> stars;
        stars.reserve(refPts.size());
        for (size_t i = 0; i < refPts.size(); ++i)
        {
            guiding::MultiGuideStar s;
            s.referencePoint = refPts[i];
            s.lastPoint = refPts[i];
            s.offsetFromPrimary = refPts[i] - primaryRef;
            s.snr = static_cast<double>(peaks[i]); // 用 peak 近似 SNR，仅用于相对权重
            s.wasLost = false;
            stars.push_back(s);
        }
        ms.setReferenceStars(stars, 9, 5.0, 8);

        const int N = 120;
        std::vector<double> singleDx, singleDy;
        std::vector<double> multiDx, multiDy;
        singleDx.reserve(N);
        singleDy.reserve(N);
        multiDx.reserve(N);
        multiDy.reserve(N);

        for (int i = 0; i < N; ++i)
        {
            // 真实平移（模拟导星误差）：缓慢漂移 + 抖动
            const double driftX = 0.05 * i;
            const double driftY = -0.03 * i;
            const double jitterX = (std::rand() % 1000 - 500) / 500.0 * 0.25; // ±0.25px
            const double jitterY = (std::rand() % 1000 - 500) / 500.0 * 0.25;
            const QPointF trueOfs(driftX + jitterX, driftY + jitterY);

            std::vector<QPointF> curPts = refPts;
            for (auto& p : curPts) p += trueOfs;

            // 丢星场景：中间一段时间让某颗副星“消失”
            std::vector<uint16_t> curPeaks = peaks;
            if (i >= 40 && i < 70)
                curPeaks[2] = 0; // 第2颗副星变暗到不可用

            cv::Mat frame = makeMultiStarFrame(w, h, curPts, curPeaks, 2.0, 60);

            // 单星测量：这里直接用主星真值代表“主星已成功跟踪”
            // 关键：给“主星测量值”加额外噪声，模拟 seeing/质心抖动，让多星平均可以带来收益
            const double measNoiseX = (std::rand() % 1000 - 500) / 500.0 * 0.6; // ±0.6px
            const double measNoiseY = (std::rand() % 1000 - 500) / 500.0 * 0.6;
            QPointF measuredPrimary = curPts[0] + QPointF(measNoiseX, measNoiseY);
            QPointF singleOffset = measuredPrimary - primaryRef;

            auto mr = ms.refineOffset(frame, measuredPrimary, singleOffset);
            QPointF multiOffset = mr.refined ? mr.refinedOffset : singleOffset;

            singleDx.push_back(singleOffset.x());
            singleDy.push_back(singleOffset.y());
            multiDx.push_back(multiOffset.x());
            multiDy.push_back(multiOffset.y());
        }

        std::cout << "[MultiStar] singleRMS(dx)=" << rms(singleDx)
                  << " singleRMS(dy)=" << rms(singleDy)
                  << " multiRMS(dx)=" << rms(multiDx)
                  << " multiRMS(dy)=" << rms(multiDy)
                  << "\n";
    }

    // ===== 6) 校准质量硬门槛（应触发 Error） =====
    {
        GuiderCore core;
        guiding::GuidingParams gp;
        gp.enableDecBacklashMeasure = false;
        gp.enableEmergency = false;
        gp.calibMaxOrthoErrDeg = 5.0; // very strict -> should fail with current simulated geometry
        core.setParams(gp);

        core.startLoop();
        core.startGuiding();

        // Simulate mount response with deliberate cross-coupling so orthoErr is non-trivial.
        QPointF star = lock;
        const double raPxPerMs2 = 0.01;
        const double decPxPerMs2 = 0.01;

        QObject::connect(&core, &GuiderCore::requestPulse, [&](const guiding::PulseCommand& cmd) {
            if (cmd.dir == guiding::GuideDir::West)  { star.rx() += raPxPerMs2 * cmd.durationMs; }
            if (cmd.dir == guiding::GuideDir::East)  { star.rx() -= raPxPerMs2 * cmd.durationMs; }
            // Deliberate cross-coupling: DEC pulses also move X significantly so orthoErr becomes large.
            if (cmd.dir == guiding::GuideDir::North) { star.ry() += decPxPerMs2 * cmd.durationMs; star.rx() += 0.30 * decPxPerMs2 * cmd.durationMs; }
            if (cmd.dir == guiding::GuideDir::South) { star.ry() -= decPxPerMs2 * cmd.durationMs; star.rx() -= 0.30 * decPxPerMs2 * cmd.durationMs; }
        });

        bool sawQualityFail = false;
        QObject::connect(&core, &GuiderCore::infoMessage, [&](const QString& msg) {
            if (msg.contains("校准质量不达标"))
                sawQualityFail = true;
        });

        for (int i = 0; i < 120; ++i)
        {
            cv::Mat frame = makeFrame(w, h, star.x(), star.y(), 2.0, 40000);
            std::string p = outDir + "/calq_" + std::to_string(i) + ".fits";
            writeFits16(p, frame);
            core.onNewFrame(QString::fromStdString(p));
            if (core.state() == guiding::State::Error)
                break;
        }

        if (!sawQualityFail || core.state() != guiding::State::Error)
        {
            std::cerr << "[CalibQuality] FAILED: expected hard-fail (Error) when calibMaxOrthoErrDeg is too strict\n";
            return 1;
        }
        std::cout << "[CalibQuality] OK: hard-fail triggered\n";
    }

    // ===== 7b) ms/px 越界不应硬失败（应告警但继续进入 Guiding） =====
    {
        GuiderCore core;
        guiding::GuidingParams gp;
        gp.enableDecBacklashMeasure = false;
        gp.enableEmergency = false;
        // 放宽真正硬失败项，确保只测试 ms/px 的行为
        gp.calibMaxOrthoErrDeg = 90.0;
        gp.calibMinAxisMovePx = 0.0;
        core.setParams(gp);

        core.startLoop();
        core.startGuiding();

        // slow response -> large ms/px (pulse durations accumulate faster than travel)
        QPointF star = lock;
        const double raPxPerMs2 = 0.0015; // 500ms -> 0.75px
        const double decPxPerMs2 = 0.01;

        QObject::connect(&core, &GuiderCore::requestPulse, [&](const guiding::PulseCommand& cmd) {
            if (cmd.dir == guiding::GuideDir::West)  star.rx() += raPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::East)  star.rx() -= raPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::North) star.ry() += decPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::South) star.ry() -= decPxPerMs2 * cmd.durationMs;
        });

        bool sawMsWarn = false;
        QObject::connect(&core, &GuiderCore::infoMessage, [&](const QString& msg) {
            if (msg.contains("校准质量警告（继续导星）"))
                sawMsWarn = true;
        });

        for (int i = 0; i < 220; ++i)
        {
            cv::Mat frame = makeFrame(w, h, star.x(), star.y(), 2.0, 40000);
            std::string p = outDir + "/cal_ms_warn_" + std::to_string(i) + ".fits";
            writeFits16(p, frame);
            core.onNewFrame(QString::fromStdString(p));
            if (core.state() == guiding::State::Guiding)
                break;
            if (core.state() == guiding::State::Error)
                break;
        }

        if (core.state() != guiding::State::Guiding || !sawMsWarn)
        {
            std::cerr << "[CalibMsPerPxWarn] FAILED: expected warning + continue to Guiding (not Error)\n";
            return 1;
        }
        std::cout << "[CalibMsPerPxWarn] OK: saw warning and reached Guiding\n";
    }

    // ===== 7c) PulseNoEffect 应快速失败（校准阶段） =====
    {
        GuiderCore core;
        guiding::GuidingParams gp;
        gp.enableDecBacklashMeasure = false;
        gp.enableEmergency = false;
        gp.calibMaxOrthoErrDeg = 90.0;
        gp.calibMinAxisMovePx = 0.0;
        core.setParams(gp);

        core.startLoop();
        core.startGuiding();

        QPointF star = lock;
        const double raPxPerMs2 = 0.00005; // 500ms -> 0.025px, almost no effect
        const double decPxPerMs2 = 0.00005;

        QObject::connect(&core, &GuiderCore::requestPulse, [&](const guiding::PulseCommand& cmd) {
            if (cmd.dir == guiding::GuideDir::West)  star.rx() += raPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::East)  star.rx() -= raPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::North) star.ry() += decPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::South) star.ry() -= decPxPerMs2 * cmd.durationMs;
        });

        bool sawNoEffect = false;
        QObject::connect(&core, &GuiderCore::infoMessage, [&](const QString& msg) {
            if (msg.contains("PulseNoEffect") || msg.contains("校准失败"))
                sawNoEffect = true;
        });

        for (int i = 0; i < 80; ++i)
        {
            cv::Mat frame = makeFrame(w, h, star.x(), star.y(), 2.0, 40000);
            std::string p = outDir + "/cal_noeff_" + std::to_string(i) + ".fits";
            writeFits16(p, frame);
            core.onNewFrame(QString::fromStdString(p));
            if (core.state() == guiding::State::Error)
                break;
        }

        if (core.state() != guiding::State::Error || !sawNoEffect)
        {
            std::cerr << "[CalibPulseNoEffect] FAILED: expected Error with PulseNoEffect\n";
            return 1;
        }
        std::cout << "[CalibPulseNoEffect] OK: failed fast with PulseNoEffect\n";
    }

    // ===== 7d) PHD2 公式步长：给定 imageScale/guideSpeed 时应影响 pulseMs =====
    {
        GuiderCore core;
        guiding::GuidingParams gp;
        gp.enableDecBacklashMeasure = false;
        gp.enableEmergency = false;
        gp.calibMaxOrthoErrDeg = 90.0;
        gp.calibMinAxisMovePx = 0.0;
        gp.pixelScaleArcsecPerPixel = 2.0;
        gp.guideSpeedSidereal = 0.5;
        gp.calibDesiredSteps = 12;
        gp.calibDistancePx = 25.0;
        core.setParams(gp);

        QPointF star = lock;
        const double raPxPerMs2 = 0.01;
        const double decPxPerMs2 = 0.01;
        int firstPulseMs = 0;

        QObject::connect(&core, &GuiderCore::requestPulse, [&](const guiding::PulseCommand& cmd) {
            if (firstPulseMs == 0)
                firstPulseMs = cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::West)  star.rx() += raPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::East)  star.rx() -= raPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::North) star.ry() += decPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::South) star.ry() -= decPxPerMs2 * cmd.durationMs;
        });

        core.startLoop();
        core.startGuiding();

        // Feed enough frames to reach first calibration pulse
        for (int i = 0; i < 10 && firstPulseMs == 0; ++i)
        {
            cv::Mat frame = makeFrame(w, h, star.x(), star.y(), 2.0, 40000);
            std::string p = outDir + "/cal_step_" + std::to_string(i) + ".fits";
            writeFits16(p, frame);
            core.onNewFrame(QString::fromStdString(p));
        }

        // Expected: distance=25px, scale=2\"/px, speed=0.5, steps=12 => pulse≈600ms (rounded up to 50ms)
        if (firstPulseMs != 600)
        {
            std::cerr << "[CalibStepSize] FAILED: expected first pulse 600ms, got " << firstPulseMs << "ms\n";
            return 1;
        }
        std::cout << "[CalibStepSize] OK: first pulse is 600ms\n";
    }

    // ===== 8) 丢星恢复：连续若干帧 strict centroid 失败 -> 回退 Selecting =====
    {
        GuiderCore core;
        guiding::GuidingParams gp;
        gp.enableDecBacklashMeasure = false;
        gp.enableEmergency = false;
        gp.autoDecGuideDir = false;
        gp.allowedDecDirs = { guiding::GuideDir::North, guiding::GuideDir::South };
        gp.calibMaxOrthoErrDeg = 90.0; // allow calibration
        gp.calibMinAxisMovePx = 0.0;
        gp.maxConsecutiveCentroidFails = 3; // small threshold for test
        core.setParams(gp);

        QPointF star = lock;
        const double raPxPerMs2 = 0.01;
        const double decPxPerMs2 = 0.01;

        QObject::connect(&core, &GuiderCore::requestPulse, [&](const guiding::PulseCommand& cmd) {
            if (cmd.dir == guiding::GuideDir::West)  star.rx() += raPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::East)  star.rx() -= raPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::North) star.ry() += decPxPerMs2 * cmd.durationMs;
            if (cmd.dir == guiding::GuideDir::South) star.ry() -= decPxPerMs2 * cmd.durationMs;
        });

        core.startLoop();
        core.startGuiding();

        bool guidingStarted = false;
        // first, drive until guiding
        for (int i = 0; i < 200; ++i)
        {
            cv::Mat frame = makeFrame(w, h, star.x(), star.y(), 2.0, 40000);
            std::string p = outDir + "/ls_init_" + std::to_string(i) + ".fits";
            writeFits16(p, frame);
            core.onNewFrame(QString::fromStdString(p));
            if (core.state() == guiding::State::Guiding)
            {
                guidingStarted = true;
                break;
            }
        }
        if (!guidingStarted)
        {
            std::cerr << "[LostStar] FAILED: could not reach Guiding state\n";
            return 1;
        }

        // then feed blank frames (strict centroid should fail) to trigger recovery to Selecting
        for (int i = 0; i < 10; ++i)
        {
            cv::Mat blank(h, w, CV_16UC1, cv::Scalar(0));
            std::string p = outDir + "/ls_blank_" + std::to_string(i) + ".fits";
            writeFits16(p, blank);
            core.onNewFrame(QString::fromStdString(p));
            if (core.state() == guiding::State::Selecting)
                break;
        }

        if (core.state() != guiding::State::Selecting)
        {
            std::cerr << "[LostStar] FAILED: expected fallback to Selecting after blank frames\n";
            return 1;
        }
        std::cout << "[LostStar] OK: fell back to Selecting\n";
    }

    // ===== 9) RA Hysteresis 行为：对同样的 RA 漂移，脉冲抖动应更小 =====
    {
        auto runScenario = [&](bool enableHys) {
            GuiderCore core;
            guiding::GuidingParams gp;
            gp.enableDecBacklashMeasure = false;
            gp.enableEmergency = false;
            gp.autoDecGuideDir = false;
            // Disable DEC corrections by gating DEC completely (avoid minPulse clamp producing DEC pulses when aggression=0).
            gp.allowedDecDirs.clear();
            gp.enableErrorEma = false;
            gp.enableRaHysteresis = enableHys;
            gp.raHysteresis = 0.7;
            // turn off step limiting so we can observe hysteresis smoothing effect clearly
            gp.raMaxStepMsPerFrame = 0;
            gp.maxPulseStepPerFrameMs = 0;
            gp.calibMaxOrthoErrDeg = 90.0;
            gp.calibMinAxisMovePx = 0.0;
            core.setParams(gp);

            QPointF star = lock;
            const double raPxPerMs2 = 0.01;
            const double decPxPerMs2 = 0.01;
            std::vector<int> raPulsesSigned;
            raPulsesSigned.reserve(200);

            QObject::connect(&core, &GuiderCore::requestPulse, [&](const guiding::PulseCommand& cmd) {
                if (cmd.dir == guiding::GuideDir::West)  star.rx() += raPxPerMs2 * cmd.durationMs;
                if (cmd.dir == guiding::GuideDir::East)  star.rx() -= raPxPerMs2 * cmd.durationMs;
                if (cmd.dir == guiding::GuideDir::North) star.ry() += decPxPerMs2 * cmd.durationMs;
                if (cmd.dir == guiding::GuideDir::South) star.ry() -= decPxPerMs2 * cmd.durationMs;
                if ((cmd.dir == guiding::GuideDir::West || cmd.dir == guiding::GuideDir::East)
                    && core.state() == guiding::State::Guiding)
                {
                    const int s = (cmd.dir == guiding::GuideDir::West) ? +cmd.durationMs : -cmd.durationMs;
                    raPulsesSigned.push_back(s);
                }
            });

            core.startLoop();
            core.startGuiding();

            // reach guiding
            for (int i = 0; i < 200; ++i)
            {
                cv::Mat frame = makeFrame(w, h, star.x(), star.y(), 2.0, 40000);
                std::string p = outDir + "/rahys_init_" + std::to_string(enableHys) + "_" + std::to_string(i) + ".fits";
                writeFits16(p, frame);
                core.onNewFrame(QString::fromStdString(p));
                if (core.state() == guiding::State::Guiding)
                    break;
            }

            // deterministic RA drift + small noise
            for (int i = 0; i < 120; ++i)
            {
                const double drift = 0.6 * std::sin(i * 0.25) + 0.15 * std::sin(i * 1.2);
                star.rx() += drift; // introduce RA error
                cv::Mat frame = makeFrame(w, h, star.x(), star.y(), 2.0, 40000);
                std::string p = outDir + "/rahys_" + std::to_string(enableHys) + "_" + std::to_string(i) + ".fits";
                writeFits16(p, frame);
                core.onNewFrame(QString::fromStdString(p));
            }

            // compute average absolute delta between successive RA pulses
            double avgAbsDelta = 0.0;
            int cnt = 0;
            for (size_t i = 1; i < raPulsesSigned.size(); ++i)
            {
                avgAbsDelta += std::abs(raPulsesSigned[i] - raPulsesSigned[i - 1]);
                cnt++;
            }
            avgAbsDelta = (cnt > 0) ? (avgAbsDelta / cnt) : 0.0;
            return avgAbsDelta;
        };

        const double dNo = runScenario(false);
        const double dHy = runScenario(true);
        std::cout << "[RaHysteresis] avg|Δpulse| noHys=" << dNo << "ms, hys=" << dHy << "ms\n";
        if (!(dHy <= dNo))
        {
            std::cerr << "[RaHysteresis] FAILED: expected hysteresis to reduce pulse jitter\n";
            return 1;
        }
        std::cout << "[RaHysteresis] OK\n";
    }

    std::cout << "Wrote FITS frames to: " << outDir << "\n";
    return 0;
}



