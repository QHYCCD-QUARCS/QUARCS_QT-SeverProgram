#include "../guiding/GuidingStarDetector.h"
#include "../Logger.h"
#include "../tools.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct BrightPeak
{
    QPointF pos;
    double peakAdu = 0.0;
};

double localPeakAdu(const cv::Mat& img16, const QPointF& pos, int halfSizePx)
{
    if (img16.empty())
        return 0.0;

    const int cx = static_cast<int>(std::lround(pos.x()));
    const int cy = static_cast<int>(std::lround(pos.y()));
    const int x0 = std::max(0, cx - halfSizePx);
    const int y0 = std::max(0, cy - halfSizePx);
    const int x1 = std::min(img16.cols - 1, cx + halfSizePx);
    const int y1 = std::min(img16.rows - 1, cy + halfSizePx);

    double peak = 0.0;
    for (int yy = y0; yy <= y1; ++yy)
    {
        for (int xx = x0; xx <= x1; ++xx)
        {
            const double v = static_cast<double>(img16.at<uint16_t>(yy, xx));
            peak = std::max(peak, v);
        }
    }
    return peak;
}

bool isFarEnough(const QPointF& a, const QPointF& b, double minDistPx)
{
    const double dx = a.x() - b.x();
    const double dy = a.y() - b.y();
    return (dx * dx + dy * dy) >= (minDistPx * minDistPx);
}

std::vector<BrightPeak> detectBrightPeaksSimple(const cv::Mat& image16, int maxPeaks)
{
    std::vector<BrightPeak> peaks;
    if (image16.empty() || image16.type() != CV_16UC1)
        return peaks;

    cv::Mat medianed;
    cv::medianBlur(image16, medianed, 3);

    cv::Scalar mean, stddev;
    cv::meanStdDev(medianed, mean, stddev);
    const double threshold = mean[0] + std::max(50.0, 3.0 * stddev[0]);

    struct Candidate
    {
        int x = 0;
        int y = 0;
        double peak = 0.0;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(256);

    for (int y = 2; y < medianed.rows - 2; ++y)
    {
        for (int x = 2; x < medianed.cols - 2; ++x)
        {
            const double v = static_cast<double>(medianed.at<uint16_t>(y, x));
            if (v < threshold)
                continue;

            bool isMax = true;
            for (int j = -1; j <= 1 && isMax; ++j)
            {
                for (int i = -1; i <= 1; ++i)
                {
                    if (i == 0 && j == 0)
                        continue;
                    if (static_cast<double>(medianed.at<uint16_t>(y + j, x + i)) > v)
                    {
                        isMax = false;
                        break;
                    }
                }
            }
            if (!isMax)
                continue;

            candidates.push_back({x, y, localPeakAdu(image16, QPointF(x, y), 2)});
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.peak > b.peak;
    });

    for (const auto& c : candidates)
    {
        const QPointF p(c.x, c.y);
        bool keep = true;
        for (const auto& kept : peaks)
        {
            if (!isFarEnough(p, kept.pos, 10.0))
            {
                keep = false;
                break;
            }
        }
        if (!keep)
            continue;

        peaks.push_back({p, c.peak});
        if (static_cast<int>(peaks.size()) >= maxPeaks)
            break;
    }

    return peaks;
}

cv::Mat makePreview8(const cv::Mat& img16)
{
    cv::Mat img8(img16.rows, img16.cols, CV_8UC1, cv::Scalar(0));
    uint16_t b = 0;
    uint16_t w = 65535;
    Tools::GetAutoStretch(img16, 0, b, w);
    if (w <= b)
        w = std::min<uint16_t>(65535, static_cast<uint16_t>(b + 1));
    Tools::Bit16To8_Stretch(img16, img8, b, w);
    return img8;
}

cv::Mat makeMedianPreview8(const cv::Mat& img16)
{
    cv::Mat medianed;
    cv::medianBlur(img16, medianed, 3);
    return makePreview8(medianed);
}

void drawCandidates(cv::Mat& overlay,
                    const std::vector<guiding::StarCandidate>& stars,
                    const cv::Scalar& color,
                    bool drawLabel)
{
    for (size_t i = 0; i < stars.size(); ++i)
    {
        const auto& s = stars[i];
        const int half = std::max(6, static_cast<int>(std::lround(std::max(6.0, s.hfd) * 0.5)));
        const cv::Point c(static_cast<int>(std::lround(s.x)), static_cast<int>(std::lround(s.y)));
        const cv::Rect r(c.x - half, c.y - half, half * 2, half * 2);
        cv::rectangle(overlay, r, color, 1, cv::LINE_AA);
        if (drawLabel)
        {
            const std::string text = "SNR " + cv::format("%.1f", s.snr);
            cv::putText(overlay, text, cv::Point(c.x + half + 3, c.y - half - 3),
                        cv::FONT_HERSHEY_SIMPLEX, 0.35, color, 1, cv::LINE_AA);
        }
    }
}

void drawAllMarkers(cv::Mat& overlay,
                    const std::vector<BrightPeak>& brightPeaks,
                    const std::vector<guiding::StarCandidate>& rejected,
                    const std::vector<guiding::StarCandidate>& validated,
                    const std::optional<guiding::StarCandidate>& best)
{
    for (size_t i = 0; i < brightPeaks.size(); ++i)
    {
        const cv::Point c(static_cast<int>(std::lround(brightPeaks[i].pos.x())),
                          static_cast<int>(std::lround(brightPeaks[i].pos.y())));
        cv::circle(overlay, c, 8, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
        cv::putText(overlay,
                    std::to_string(i + 1) + ":" + cv::format("%.0f", brightPeaks[i].peakAdu),
                    cv::Point(c.x + 10, c.y - 6),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.35,
                    cv::Scalar(0, 255, 255),
                    1,
                    cv::LINE_AA);
    }

    drawCandidates(overlay, rejected, cv::Scalar(255, 120, 40), false);
    drawCandidates(overlay, validated, cv::Scalar(0, 0, 255), true);

    if (best.has_value())
    {
        const cv::Point c(static_cast<int>(std::lround(best->x)), static_cast<int>(std::lround(best->y)));
        cv::rectangle(overlay, cv::Rect(c.x - 12, c.y - 12, 24, 24), cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
        cv::putText(overlay, "PRIMARY", cv::Point(c.x + 14, c.y + 14),
                    cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
    }
}

QJsonObject candidateToJson(const guiding::StarCandidate& s)
{
    QJsonObject obj;
    obj["x"] = s.x;
    obj["y"] = s.y;
    obj["snr"] = s.snr;
    obj["hfd"] = s.hfd;
    obj["peakADU"] = s.peakADU;
    obj["edgeDistPx"] = s.edgeDistPx;
    return obj;
}

QJsonArray candidatesToJson(const std::vector<guiding::StarCandidate>& stars)
{
    QJsonArray arr;
    for (const auto& s : stars)
        arr.push_back(candidateToJson(s));
    return arr;
}

QJsonArray brightPeaksToJson(const std::vector<BrightPeak>& peaks)
{
    QJsonArray arr;
    for (size_t i = 0; i < peaks.size(); ++i)
    {
        QJsonObject obj;
        obj["rank"] = static_cast<int>(i + 1);
        obj["x"] = peaks[i].pos.x();
        obj["y"] = peaks[i].pos.y();
        obj["peakADU"] = peaks[i].peakAdu;
        arr.push_back(obj);
    }
    return arr;
}

QString normalizeDir(QString path)
{
    path = QDir::cleanPath(path);
    if (!path.endsWith('/'))
        path += '/';
    return path;
}

QString toDirectWebPath(const QString& localPath, const QString& imageRoot)
{
    const QString cleanRoot = normalizeDir(imageRoot);
    const QString cleanPath = QDir::cleanPath(localPath);
    if (!cleanPath.startsWith(cleanRoot))
        return QString();

    QString rel = cleanPath.mid(cleanRoot.size());
    while (rel.startsWith('/'))
        rel.remove(0, 1);
    return QStringLiteral("/img/") + rel;
}

void writeGuiderDiagnosticsIndex(const QString& guiderDiagnosticsRoot, const QString& imageRoot)
{
    QDir diagDir(guiderDiagnosticsRoot);
    if (!diagDir.exists())
        return;

    const QStringList batchNames = diagDir.entryList(QStringList() << "batch_*", QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    QJsonArray batches;
    for (const QString& batchName : batchNames)
    {
        const QString batchDir = diagDir.filePath(batchName);
        const QString summaryPath = QDir(batchDir).filePath("analysis/summary.json");
        QFile summaryFile(summaryPath);
        if (!summaryFile.open(QIODevice::ReadOnly))
            continue;

        const auto summaryDoc = QJsonDocument::fromJson(summaryFile.readAll());
        summaryFile.close();
        if (!summaryDoc.isObject())
            continue;

        QJsonObject batch;
        batch["batchName"] = batchName;
        batch["batchDir"] = batchDir;
        batch["summaryPath"] = summaryPath;
        batch["summaryWebPath"] = toDirectWebPath(summaryPath, imageRoot);
        batch["frameCount"] = summaryDoc.object().value("frameCount").toInt();

        QFileInfo fi(batchDir);
        batch["lastModified"] = fi.lastModified().toString(Qt::ISODate);
        batches.push_back(batch);
    }

    QJsonObject root;
    root["generatedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["guiderDiagnosticsRoot"] = guiderDiagnosticsRoot;
    root["imageRoot"] = imageRoot;
    root["batches"] = batches;

    const QString indexPath = diagDir.filePath("index.json");
    QSaveFile out(indexPath);
    if (out.open(QIODevice::WriteOnly))
    {
        out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        out.commit();
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    struct LoggerCloser
    {
        ~LoggerCloser()
        {
            Logger::Close();
        }
    } loggerCloser;

    std::cerr << "[startup] argc=" << argc << "\n";
    std::cerr.flush();

    if (argc < 2)
    {
        std::cerr << "usage: guiding_batch_analyzer <batchDir> [outDir] [frameLimit]\n";
        return 2;
    }

    const QString batchDir = QString::fromUtf8(argv[1]);
    const QString outDir = (argc >= 3)
        ? QString::fromUtf8(argv[2])
        : QDir(batchDir).filePath("analysis");
    const QString imageRoot = QDir::cleanPath(QDir::homePath() + "/images");
    const QString guiderDiagnosticsRoot = QDir::cleanPath(QFileInfo(batchDir).dir().absolutePath());
    const int frameLimit = (argc >= 4) ? std::max(1, QString::fromUtf8(argv[3]).toInt()) : -1;
    std::cerr << "[startup] batchDir=" << batchDir.toStdString()
              << " outDir=" << outDir.toStdString()
              << " frameLimit=" << frameLimit << "\n";
    std::cerr.flush();

    if (!QDir(batchDir).exists())
    {
        std::cerr << "batch dir not found: " << batchDir.toStdString() << "\n";
        return 2;
    }
    std::cerr << "[startup] batch_dir_exists\n";
    std::cerr.flush();
    QDir().mkpath(outDir);
    std::cerr << "[startup] out_dir_ready\n";
    std::cerr.flush();

    QStringList fitsFiles = QDir(batchDir).entryList(QStringList() << "frame_*.fits", QDir::Files, QDir::Name);
    if (fitsFiles.isEmpty())
    {
        std::cerr << "no frame_*.fits found in " << batchDir.toStdString() << "\n";
        return 2;
    }
    std::cerr << "[startup] fits_count=" << fitsFiles.size() << "\n";
    std::cerr.flush();

    std::cerr << "[startup] detector_construct_begin\n";
    std::cerr.flush();
    guiding::GuidingStarDetector det;
    std::cerr << "[startup] detector_construct_done\n";
    std::cerr.flush();
    guiding::StarSelectionParams sp;
    QJsonArray framesJson;
    int frameIndex = 0;

    for (const QString& name : fitsFiles)
    {
        ++frameIndex;
        if (frameLimit > 0 && frameIndex > frameLimit)
            break;

        const QString fitsPath = QDir(batchDir).filePath(name);
        std::cerr << "[progress] frame=" << frameIndex << " file=" << name.toStdString() << " step=read\n";
        std::cerr.flush();
        cv::Mat img16;
        if (Tools::readFits(fitsPath.toUtf8().constData(), img16) != 0 || img16.empty())
        {
            std::cerr << "[skip] failed to read " << fitsPath.toStdString() << "\n";
            continue;
        }

        std::vector<guiding::StarCandidate> validated;
        std::vector<guiding::StarCandidate> rejected;
        std::cerr << "[progress] frame=" << frameIndex << " file=" << name.toStdString() << " step=select\n";
        std::cerr.flush();
        auto best = det.selectGuideStar(img16, sp, fitsPath, &validated, &rejected);
        std::cerr << "[progress] frame=" << frameIndex << " file=" << name.toStdString()
                  << " step=select_done validated=" << validated.size()
                  << " rejected=" << rejected.size() << "\n";
        std::cerr.flush();

        std::cerr << "[progress] frame=" << frameIndex << " file=" << name.toStdString() << " step=bright_peaks\n";
        std::cerr.flush();
        const auto brightPeaks = detectBrightPeaksSimple(img16, 12);

        std::cerr << "[progress] frame=" << frameIndex << " file=" << name.toStdString() << " step=preview\n";
        std::cerr.flush();
        cv::Mat preview8 = makePreview8(img16);
        cv::Mat overlay;
        cv::cvtColor(preview8, overlay, cv::COLOR_GRAY2BGR);
        cv::Mat medianPreview8 = makeMedianPreview8(img16);
        cv::Mat medianOverlay;
        cv::cvtColor(medianPreview8, medianOverlay, cv::COLOR_GRAY2BGR);

        drawAllMarkers(overlay, brightPeaks, rejected, validated, best);
        drawAllMarkers(medianOverlay, brightPeaks, rejected, validated, best);

        cv::putText(overlay, "RAW", cv::Point(16, 28),
                    cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
        cv::putText(medianOverlay, "MEDIAN x3", cv::Point(16, 28),
                    cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);

        const QString stem = QFileInfo(name).completeBaseName();
        const QString rawJpgPath = QDir(batchDir).filePath(stem + ".jpg");
        const QString rawJsonPath = QDir(batchDir).filePath(stem + ".json");
        const QString outImagePath = QDir(outDir).filePath(stem + "_analysis.jpg");
        const QString outMedianImagePath = QDir(outDir).filePath(stem + "_analysis_median.jpg");
        std::cerr << "[progress] frame=" << frameIndex << " file=" << name.toStdString() << " step=write_image\n";
        std::cerr.flush();
        cv::imwrite(outImagePath.toStdString(), overlay);
        cv::imwrite(outMedianImagePath.toStdString(), medianOverlay);

        QJsonObject frameJson;
        frameJson["frame"] = stem;
        frameJson["fitsPath"] = fitsPath;
        frameJson["fitsWebPath"] = toDirectWebPath(fitsPath, imageRoot);
        frameJson["rawJpgPath"] = rawJpgPath;
        frameJson["rawJpgWebPath"] = toDirectWebPath(rawJpgPath, imageRoot);
        frameJson["rawJsonPath"] = rawJsonPath;
        frameJson["rawJsonWebPath"] = toDirectWebPath(rawJsonPath, imageRoot);
        frameJson["analysisImagePath"] = outImagePath;
        frameJson["analysisImageWebPath"] = toDirectWebPath(outImagePath, imageRoot);
        frameJson["analysisMedianImagePath"] = outMedianImagePath;
        frameJson["analysisMedianImageWebPath"] = toDirectWebPath(outMedianImagePath, imageRoot);
        frameJson["width"] = img16.cols;
        frameJson["height"] = img16.rows;
        frameJson["validatedCount"] = static_cast<int>(validated.size());
        frameJson["rejectedCount"] = static_cast<int>(rejected.size());
        frameJson["brightPeaks"] = brightPeaksToJson(brightPeaks);
        frameJson["validated"] = candidatesToJson(validated);
        frameJson["rejected"] = candidatesToJson(rejected);
        if (best.has_value())
            frameJson["primary"] = candidateToJson(*best);
        framesJson.push_back(frameJson);

        std::cout << "[frame] " << stem.toStdString()
                  << " primary=" << (best.has_value() ? "yes" : "no")
                  << " validated=" << validated.size()
                  << " rejected=" << rejected.size()
                  << " brightPeaks=" << brightPeaks.size()
                  << " out=" << outImagePath.toStdString()
                  << " median_out=" << outMedianImagePath.toStdString()
                  << "\n";
    }

    QJsonObject root;
    root["batchDir"] = batchDir;
    root["batchName"] = QFileInfo(batchDir).fileName();
    root["analysisDir"] = outDir;
    root["summaryWebPath"] = toDirectWebPath(QDir(outDir).filePath("summary.json"), imageRoot);
    root["frameCount"] = static_cast<int>(framesJson.size());
    root["frames"] = framesJson;

    const QString summaryPath = QDir(outDir).filePath("summary.json");
    std::cerr << "[progress] step=write_summary path=" << summaryPath.toStdString() << "\n";
    std::cerr.flush();
    QFile summary(summaryPath);
    if (summary.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        summary.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        summary.close();
    }

    writeGuiderDiagnosticsIndex(guiderDiagnosticsRoot, imageRoot);

    std::cout << "[done] summary=" << summaryPath.toStdString() << "\n";
    return 0;
}
