// flatfield_batch_test.cpp
// 批量测试平场法 vs PHD2 星点检测，使用真实导星FITS文件
//
// 用法：flatfield_batch_test <fits_dir>
// 例如：flatfield_batch_test /home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/

#include "../guiding/GuidingStarDetector.h"
#include "../guiding/flatfield/FlatFieldDetector.h"

#include <fitsio.h>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>

#include <QCoreApplication>
#include <QString>

#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>

namespace fs = std::filesystem;

static cv::Mat readFits16(const std::string& path)
{
    fitsfile* fptr = nullptr;
    int status = 0;
    int bittype = 0;
    long naxes = 0;
    long naxis[2] = {0, 0};

    fits_open_file(&fptr, path.c_str(), READONLY, &status);
    if (status) {
        fits_report_error(stderr, status);
        return cv::Mat();
    }
    fits_get_img_type(fptr, &bittype, &status);
    fits_get_img_dim(fptr, &naxes, &status);
    fits_get_img_size(fptr, naxes, naxis, &status);

    cv::Mat img;
    if (naxes >= 2 && (bittype == USHORT_IMG || bittype == SHORT_IMG))
    {
        img = cv::Mat(cv::Size(naxis[0], naxis[1]), CV_16UC1);
        long fpixel = 1;
        fits_read_img(fptr, TUSHORT, fpixel, naxis[0] * naxis[1],
                      0, img.ptr<uint16_t>(), &status);
    }
    else if (naxes >= 2 && bittype == ULONG_IMG)
    {
        // 32位转16位
        img = cv::Mat(cv::Size(naxis[0], naxis[1]), CV_32SC1);
        long fpixel = 1;
        fits_read_img(fptr, TLONG, fpixel, naxis[0] * naxis[1],
                      0, img.ptr<int32_t>(), &status);
        img.convertTo(img, CV_16UC1, 1.0);
    }
    else
    {
        std::cerr << "Unsupported FITS bittype: " << bittype << " naxes: " << naxes << std::endl;
    }

    fits_close_file(fptr, &status);
    return img;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    if (argc < 2)
    {
        std::cout << "Usage: flatfield_batch_test <fits_dir>\n";
        std::cout << "  Tests all .fits files in <fits_dir> with both PHD2 and FlatField detectors.\n";
        return 1;
    }

    std::string fitsDir(argv[1]);

    // 收集所有 .fits 文件
    std::vector<std::string> fitsFiles;
    for (const auto& entry : fs::directory_iterator(fitsDir))
    {
        std::string ext = entry.path().extension().string();
        if (ext == ".fits" || ext == ".FITS")
        {
            fitsFiles.push_back(entry.path().string());
        }
    }
    std::sort(fitsFiles.begin(), fitsFiles.end());

    std::cout << "Found " << fitsFiles.size() << " FITS files in: " << fitsDir << "\n";
    std::cout << std::string(80, '=') << "\n";

    // 检测器参数
    guiding::StarSelectionParams phd2Params;
    phd2Params.minSNR = 5.0;

    guiding::flatfield::FlatFieldDetector::Params ffParams;
    ffParams.kernelSize = 64;
    ffParams.method = "uniform";
    ffParams.snrThreshold = 5.0;
    ffParams.minHFD = 1.5;
    ffParams.maxHFD = 12.0;
    ffParams.minSeparation = 5;
    ffParams.edgeMarginPx = 40.0;

    guiding::GuidingStarDetector phd2Detector;
    guiding::flatfield::FlatFieldDetector ffDetector;

    // 统计
    int totalFiles = 0;
    int phd2Found = 0, ffFound = 0;
    double phd2TotalMs = 0.0, ffTotalMs = 0.0;

    std::cout << std::left
              << std::setw(45) << "File"
              << std::setw(12) << "PHD2_stars"
              << std::setw(12) << "FF_stars"
              << std::setw(12) << "PHD2_ms"
              << std::setw(12) << "FF_ms"
              << "\n";
    std::cout << std::string(80, '-') << "\n";

    for (const auto& fitsPath : fitsFiles)
    {
        totalFiles++;
        cv::Mat img16 = readFits16(fitsPath);
        if (img16.empty())
        {
            std::string fname = fs::path(fitsPath).filename().string();
            std::cout << std::left << std::setw(45) << fname
                      << " SKIP (read failed)" << "\n";
            continue;
        }

        // 3x3 中值滤波
        cv::Mat imgFiltered;
        cv::medianBlur(img16, imgFiltered, 3);

        // PHD2 检测
        std::vector<guiding::StarCandidate> phd2Candidates, phd2Rejected;
        auto t1 = std::chrono::high_resolution_clock::now();
        auto phd2Result = phd2Detector.selectGuideStar(imgFiltered, phd2Params,
                                                        QString::fromStdString(fitsPath),
                                                        &phd2Candidates, &phd2Rejected);
        auto t2 = std::chrono::high_resolution_clock::now();
        double phd2Ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        phd2TotalMs += phd2Ms;

        // 平场法检测
        std::vector<guiding::StarCandidate> ffCandidates, ffRejected;
        auto t3 = std::chrono::high_resolution_clock::now();
        auto ffResult = ffDetector.detect(imgFiltered, ffParams, &ffCandidates, &ffRejected);
        auto t4 = std::chrono::high_resolution_clock::now();
        double ffMs = std::chrono::duration<double, std::milli>(t4 - t3).count();
        ffTotalMs += ffMs;

        int phd2Count = phd2Candidates ? phd2Candidates->size() : 0;
        int ffCount = ffCandidates ? ffCandidates->size() : 0;

        if (phd2Result.has_value()) phd2Found++;
        if (ffResult.has_value()) ffFound++;

        std::string fname = fs::path(fitsPath).filename().string();

        std::cout << std::left << std::setw(45) << fname
                  << std::setw(12) << (phd2Result.has_value() ? phd2Count : 0)
                  << std::setw(12) << (ffResult.has_value() ? ffCount : 0)
                  << std::fixed << std::setprecision(1)
                  << std::setw(12) << phd2Ms
                  << std::setw(12) << ffMs
                  << "\n";

        // 如果检测到星点，输出最佳星点信息
        if (phd2Result.has_value())
        {
            std::cout << "  PHD2 best: x=" << std::fixed << std::setprecision(2)
                      << phd2Result->x << " y=" << phd2Result->y
                      << " SNR=" << std::setprecision(1) << phd2Result->snr
                      << " HFD=" << phd2Result->hfd << "\n";
        }
        if (ffResult.has_value())
        {
            std::cout << "  FF   best: x=" << std::fixed << std::setprecision(2)
                      << ffResult->x << " y=" << ffResult->y
                      << " SNR=" << std::setprecision(1) << ffResult->snr
                      << " HFD=" << ffResult->hfd << "\n";
        }
    }

    std::cout << std::string(80, '=') << "\n";
    std::cout << "\n=== Summary ===" << "\n";
    std::cout << "Total files: " << totalFiles << "\n";
    std::cout << "PHD2 detected: " << phd2Found << "/" << totalFiles << "\n";
    std::cout << "FlatField detected: " << ffFound << "/" << totalFiles << "\n";
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "PHD2 avg time: " << (totalFiles > 0 ? phd2TotalMs / totalFiles : 0) << " ms" << "\n";
    std::cout << "FlatField avg time: " << (totalFiles > 0 ? ffTotalMs / totalFiles : 0) << " ms" << "\n";
    std::cout << "PHD2 total time: " << phd2TotalMs << " ms" << "\n";
    std::cout << "FlatField total time: " << ffTotalMs << " ms" << "\n";

    return 0;
}
