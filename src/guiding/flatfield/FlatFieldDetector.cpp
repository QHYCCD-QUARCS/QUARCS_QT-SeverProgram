#include "FlatFieldDetector.h"

#include <algorithm>

namespace guiding {
namespace flatfield {

namespace {

guiding::StarCandidate toGuidingStar(const star_detect::DetectedStar& star)
{
    guiding::StarCandidate out;
    out.x = star.x;
    out.y = star.y;
    out.snr = star.snr;
    out.hfd = star.hfd;
    out.peakADU = star.peakADU;
    out.edgeDistPx = star.edgeDistPx;
    return out;
}

void assignConverted(const std::vector<star_detect::DetectedStar>& input,
                     std::vector<guiding::StarCandidate>* output)
{
    if (!output)
        return;
    output->clear();
    output->reserve(input.size());
    for (const auto& star : input)
        output->push_back(toGuidingStar(star));
}

void assignRejected(const std::vector<star_detect::RejectedStar>& input,
                    std::vector<guiding::StarCandidate>* output)
{
    if (!output)
        return;
    output->clear();
    output->reserve(input.size());
    for (const auto& rejected : input)
        output->push_back(toGuidingStar(rejected.star));
}

star_detect::DetectParams toDetectParams(const FlatFieldDetector::Params& p)
{
    star_detect::DetectParams out;
    out.kernelSize = p.kernelSize;
    out.flatMethod = p.method;
    out.gaussianSigma = p.gaussianSigma;
    out.snrThreshold = p.snrThreshold;
    out.minHFD = p.minHFD;
    out.maxHFD = p.maxHFD;
    out.minSeparationPx = p.minSeparation;
    out.edgeMarginPx = p.edgeMarginPx;
    out.nearSaturationRatio = p.nearSaturationRatio;
    out.centroidHalfSize = p.centroidHalfSize;
    out.centroidKSigma = p.kSigma;
    return out;
}

} // namespace

std::optional<StarCandidate> FlatFieldDetector::detect(const cv::Mat& image16,
                                                       const Params& p,
                                                       std::vector<StarCandidate>* outDedupCandidates,
                                                       std::vector<StarCandidate>* outSnrCandidates,
                                                       std::vector<StarCandidate>* outCandidates,
                                                       std::vector<StarCandidate>* outRejected) const
{
    star_detect::ImageContext imageContext;
    imageContext.imageWidth = image16.cols;
    imageContext.imageHeight = image16.rows;

    const star_detect::DetectionResult detection =
        m_detector.detect(image16, imageContext, toDetectParams(p));

    assignConverted(detection.dedupCandidates, outDedupCandidates);
    assignConverted(detection.snrCandidates, outSnrCandidates);
    assignConverted(detection.validCandidates, outCandidates);
    assignRejected(detection.rejectedCandidates, outRejected);

    if (detection.validCandidates.empty())
        return std::nullopt;

    const auto it = std::max_element(detection.validCandidates.begin(), detection.validCandidates.end(),
                                     [](const star_detect::DetectedStar& a, const star_detect::DetectedStar& b) {
                                         return a.snr < b.snr;
                                     });
    return toGuidingStar(*it);
}

cv::Mat FlatFieldDetector::generateFlatField(const cv::Mat& image16, int kernelSize,
                                             const std::string& method, double sigma) const
{
    return m_detector.generateFlatField(image16, kernelSize, method, sigma);
}

cv::Mat FlatFieldDetector::generateFlatSubtracted(const cv::Mat& image16, int kernelSize,
                                                  const std::string& method, double sigma) const
{
    return m_detector.generateFlatSubtracted(image16, kernelSize, method, sigma);
}

} // namespace flatfield
} // namespace guiding
