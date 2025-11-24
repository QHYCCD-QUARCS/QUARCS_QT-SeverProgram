// 独立文件：合焦星点检测（C++实现，源自 findstars.py 合焦路径）
// 实现 Tools::DetectFocusedStars / DetectFocusedStarsFromFITS / MedianHFR
#include "tools.h"
#include <algorithm>
#include <numeric>
#include <limits>

namespace {

// 转 float32 并归一化到 [0,1]
static cv::Mat toFloat32Normalized(const cv::Mat& image) {
	cv::Mat gray;
	if (image.channels() == 1) {
		gray = image;
	} else {
		cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
	}
	cv::Mat f32;
	if (gray.depth() == CV_32F) {
		f32 = gray.clone();
	} else {
		gray.convertTo(f32, CV_32F);
	}
	double minv = 0.0, maxv = 0.0;
	cv::minMaxLoc(f32, &minv, &maxv);
	if (maxv > 0.0) {
		f32 /= static_cast<float>(maxv);
	}
	return f32;
}

// 背景扣除（高斯模糊），随后轻微平滑
static cv::Mat preprocessFocused(const cv::Mat& image16, int bgKsize, double smoothSigma) {
	cv::Mat img = toFloat32Normalized(image16);
	if (bgKsize % 2 == 0) bgKsize += 1;
	cv::Mat bg;
	cv::GaussianBlur(img, bg, cv::Size(bgKsize, bgKsize), 0.0);
	cv::Mat fg = img - bg;
	cv::threshold(fg, fg, 0.0, 0.0, cv::THRESH_TOZERO);
	if (smoothSigma > 0.0) {
		cv::GaussianBlur(fg, fg, cv::Size(), smoothSigma);
	}
	return fg;
}

static double estimateThreshold(const cv::Mat& img, double kSigma) {
	cv::Scalar mean, stddev;
	cv::meanStdDev(img, mean, stddev);
	return static_cast<double>(mean[0]) + kSigma * static_cast<double>(stddev[0]);
}

// ROI内基于前景亮度计算HFR
static double computeHFR(const cv::Mat& fgROI, const cv::Mat& maskROI, double cx, double cy) {
	CV_Assert(fgROI.type() == CV_32F);
	CV_Assert(maskROI.type() == CV_8U);
	std::vector<float> vals;
	std::vector<float> rs;
	vals.reserve(static_cast<size_t>(maskROI.total()));
	rs.reserve(static_cast<size_t>(maskROI.total()));
	for (int y = 0; y < maskROI.rows; ++y) {
		const uint8_t* m = maskROI.ptr<uint8_t>(y);
		const float* f = fgROI.ptr<float>(y);
		for (int x = 0; x < maskROI.cols; ++x) {
			if (m[x]) {
				vals.push_back(f[x]);
				double dx = static_cast<double>(x) - cx;
				double dy = static_cast<double>(y) - cy;
				rs.push_back(static_cast<float>(std::sqrt(dx * dx + dy * dy)));
			}
		}
	}
	if (vals.empty()) return 0.0;
	double totalFlux = std::accumulate(vals.begin(), vals.end(), 0.0);
	if (totalFlux <= 0.0) return 0.0;
	// 按半径排序
	std::vector<int> order(vals.size());
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(), [&](int a, int b){ return rs[a] < rs[b]; });
	double halfFlux = totalFlux * 0.5;
	double csum = 0.0;
	for (int idx : order) {
		csum += vals[idx];
		if (csum >= halfFlux) {
			return static_cast<double>(rs[idx]);
		}
	}
	return static_cast<double>(rs[order.back()]);
}

// 计算SNR与质量标签
static std::pair<double, QString> computeSNR(const cv::Mat& fgROI,
                                             const cv::Mat& maskROI,
                                             const cv::Mat& bgROI,
                                             double cx, double cy) {
	CV_Assert(fgROI.type() == CV_32F);
	CV_Assert(maskROI.type() == CV_8U);
	CV_Assert(bgROI.empty() || bgROI.type() == CV_32F);
	std::vector<cv::Point> pts;
	pts.reserve(static_cast<size_t>(maskROI.total()));
	for (int y = 0; y < maskROI.rows; ++y) {
		const uint8_t* m = maskROI.ptr<uint8_t>(y);
		for (int x = 0; x < maskROI.cols; ++x) {
			if (m[x]) pts.emplace_back(x, y);
		}
	}
	if (pts.empty()) return {0.0, QStringLiteral("无信号")};
	std::vector<double> dists;
	dists.reserve(pts.size());
	for (const auto& p : pts) {
		double dx = static_cast<double>(p.x) - cx;
		double dy = static_cast<double>(p.y) - cy;
		dists.push_back(std::sqrt(dx * dx + dy * dy));
	}
	double maxr = *std::max_element(dists.begin(), dists.end());
	if (!(maxr > 0.0)) return {0.0, QStringLiteral("无信号")};
	// 中心区域掩膜
	std::vector<float> centerValues;
	centerValues.reserve(pts.size());
	for (size_t i = 0; i < pts.size(); ++i) {
		if (dists[i] < maxr * 0.3) {
			const cv::Point& p = pts[i];
			float v = bgROI.empty() ? fgROI.at<float>(p) : bgROI.at<float>(p);
			centerValues.push_back(v);
		}
	}
	if (centerValues.empty()) {
		for (size_t i = 0; i < pts.size(); ++i) {
			if (dists[i] < maxr * 0.5) {
				const cv::Point& p = pts[i];
				float v = bgROI.empty() ? fgROI.at<float>(p) : bgROI.at<float>(p);
				centerValues.push_back(v);
			}
		}
	}
	double peak = 0.0;
	if (!centerValues.empty()) {
		peak = static_cast<double>(*std::max_element(centerValues.begin(), centerValues.end()));
	}
	// 背景值：非mask区域
	std::vector<float> bgValues;
	for (int y = 0; y < maskROI.rows; ++y) {
		const uint8_t* m = maskROI.ptr<uint8_t>(y);
		for (int x = 0; x < maskROI.cols; ++x) {
			if (!m[x]) {
				bgValues.push_back(bgROI.empty() ? fgROI.at<float>(y, x)
				                                 : bgROI.at<float>(y, x));
			}
		}
	}
	if (bgValues.empty()) return {0.0, QStringLiteral("无背景")};
	double mean = 0.0;
	for (float v : bgValues) mean += v;
	mean /= static_cast<double>(bgValues.size());
	double var = 0.0;
	for (float v : bgValues) {
		double d = static_cast<double>(v) - mean;
		var += d * d;
	}
	var /= static_cast<double>(bgValues.size());
	double stddev = std::sqrt(std::max(var, 0.0));
	if (!(stddev > 0.0)) return {0.0, QStringLiteral("背景标准差为0")};
	double snr = (peak - mean) / stddev;
	QString quality;
	if (snr >= 100.0) quality = QStringLiteral("极佳");
	else if (snr >= 30.0) quality = QStringLiteral("良好");
	else if (snr >= 10.0) quality = QStringLiteral("正常");
	else if (snr >= 5.0)  quality = QStringLiteral("较差");
	else quality = QStringLiteral("很差");
	return { snr, quality };
}

static double percentile(std::vector<double> vals, double p) {
	if (vals.empty()) return 0.0;
	if (p <= 0.0) return *std::min_element(vals.begin(), vals.end());
	if (p >= 100.0) return *std::max_element(vals.begin(), vals.end());
	std::sort(vals.begin(), vals.end());
	double rank = (p / 100.0) * (static_cast<double>(vals.size()) - 1.0);
	size_t lo = static_cast<size_t>(std::floor(rank));
	size_t hi = static_cast<size_t>(std::ceil(rank));
	if (lo == hi) return vals[lo];
	double w = rank - static_cast<double>(lo);
	return vals[lo] * (1.0 - w) + vals[hi] * w;
}

static std::vector<Tools::FocusedStar> filterNoiseFocused(std::vector<Tools::FocusedStar> stars,
                                                          double minHFR = 0.5,
                                                          double maxHFR = 50.0,
                                                          double fluxPercentile = 10.0) {
	if (stars.empty()) return stars;
	// 合焦：若数量很少（<=3），很可能是噪声
	if (static_cast<int>(stars.size()) <= 3) return {};
	// 过滤 HFR == 1
	std::vector<Tools::FocusedStar> filtered;
	filtered.reserve(stars.size());
	for (const auto& s : stars) {
		if (std::abs(s.hfr - 1.0) > 0.01) filtered.push_back(s);
	}
	if (filtered.empty()) return filtered;
	// 计算 min_flux
	std::vector<double> fluxes;
	for (const auto& s : filtered) if (s.flux > 0.0) fluxes.push_back(s.flux);
	double minFlux = 0.0;
	if (!fluxes.empty()) minFlux = percentile(fluxes, fluxPercentile);
	// 一般约束
	std::vector<Tools::FocusedStar> filtered2;
	for (const auto& s : filtered) {
		if (s.flux < minFlux) continue;
		if (s.hfr < minHFR || s.hfr > maxHFR) continue;
		if (s.hfr > 0.0 && s.radius > 0.0) {
			double ratio = s.hfr / s.radius;
			if (ratio > 2.5) continue;
			if (ratio < 0.1) continue;
		}
		filtered2.push_back(s);
	}
	// 特判：过滤后若数量仍<=3，且HFR/半径几乎相同，视作噪声
	if (static_cast<int>(filtered2.size()) <= 3 && !filtered2.empty()) {
		double minH = std::numeric_limits<double>::max(), maxH = -1.0;
		double minR = std::numeric_limits<double>::max(), maxR = -1.0;
		for (const auto& s : filtered2) {
			minH = std::min(minH, s.hfr); maxH = std::max(maxH, s.hfr);
			minR = std::min(minR, s.radius); maxR = std::max(maxR, s.radius);
		}
		if ((maxH - minH) < 0.1 && (maxR - minR) < 0.1) return {};
	}
	return filtered2;
}

static std::vector<Tools::FocusedStar> removeDuplicatesFocused(const std::vector<Tools::FocusedStar>& stars) {
	if (stars.empty()) return stars;
	// 计算距离阈值（基于avg半径与avg HFR）
	std::vector<double> radii, hfrs;
	radii.reserve(stars.size()); hfrs.reserve(stars.size());
	for (const auto& s : stars) {
		if (s.radius > 0.0) radii.push_back(s.radius);
		if (s.hfr > 0.0) hfrs.push_back(s.hfr);
	}
	double avgR = radii.empty() ? 0.0 : std::accumulate(radii.begin(), radii.end(), 0.0) / static_cast<double>(radii.size());
	double avgH = hfrs.empty() ? 0.0 : std::accumulate(hfrs.begin(), hfrs.end(), 0.0) / static_cast<double>(hfrs.size());
	double minDist = std::max(3.0, std::max(avgR, avgH) * 2.0);
	// 按 flux 降序
	std::vector<Tools::FocusedStar> sorted = stars;
	std::sort(sorted.begin(), sorted.end(), [](const Tools::FocusedStar& a, const Tools::FocusedStar& b){
		return a.flux > b.flux;
	});
	std::vector<Tools::FocusedStar> uniq;
	for (const auto& s : sorted) {
		bool dup = false;
		for (const auto& u : uniq) {
			double dx = s.x - u.x;
			double dy = s.y - u.y;
			if (std::sqrt(dx * dx + dy * dy) < minDist) { dup = true; break; }
		}
		if (!dup) uniq.push_back(s);
	}
	return uniq;
}

// 小星检测（合焦）
static std::vector<Tools::FocusedStar> detectSmallStarsFocused(const cv::Mat& fg,
                                                               const cv::Mat& bgImage,
                                                               double kSigma,
                                                               int minArea,
                                                               int maxArea,
                                                               double minSNR,
                                                               bool verbose) {
	cv::Mat bin;
	double thr = estimateThreshold(fg, kSigma);
	cv::threshold(fg, bin, thr, 1.0, cv::THRESH_BINARY);
	cv::Mat binU8;
	bin.convertTo(binU8, CV_8U, 255.0);
	cv::Mat labels, stats, centroids;
	int numLabels = cv::connectedComponentsWithStats(binU8, labels, stats, centroids, 8, CV_32S);
	std::vector<Tools::FocusedStar> stars;
	for (int lab = 1; lab < numLabels; ++lab) {
		int area = stats.at<int>(lab, cv::CC_STAT_AREA);
		if (area <= 30) continue; // 排除超小噪点（对齐Python逻辑）
		if (area < minArea || area > maxArea) continue;
		double cx = centroids.at<double>(lab, 0);
		double cy = centroids.at<double>(lab, 1);
		int x = stats.at<int>(lab, cv::CC_STAT_LEFT);
		int y = stats.at<int>(lab, cv::CC_STAT_TOP);
		int w = stats.at<int>(lab, cv::CC_STAT_WIDTH);
		int h = stats.at<int>(lab, cv::CC_STAT_HEIGHT);
		cv::Rect roi(x, y, w, h);
		cv::Mat roiFG = fg(roi);
		cv::Mat roiLabels = labels(roi);
		cv::Mat roiMask = (roiLabels == lab);
		cv::Mat roiMaskU8; roiMask.convertTo(roiMaskU8, CV_8U, 255.0);
		// flux
		double flux = 0.0;
		for (int yy = 0; yy < roiFG.rows; ++yy) {
			const float* f = roiFG.ptr<float>(yy);
			const uint8_t* m = roiMaskU8.ptr<uint8_t>(yy);
			for (int xx = 0; xx < roiFG.cols; ++xx) if (m[xx]) flux += static_cast<double>(f[xx]);
		}
		double cxROI = cx - static_cast<double>(x);
		double cyROI = cy - static_cast<double>(y);
		double hfr = computeHFR(roiFG, roiMaskU8, cxROI, cyROI);
		double radius = std::sqrt(static_cast<double>(area) / CV_PI);
		cv::Mat roiBG;
		if (!bgImage.empty()) roiBG = bgImage(roi);
		auto snrRes = computeSNR(roiFG, roiMaskU8, roiBG, cxROI, cyROI);
		double snr = snrRes.first;
		if (snr < minSNR) {
			if (verbose) {
				qDebug() << "SNR过滤"
				         << "位置(" << cx << "," << cy << ")"
				         << "SNR=" << snr << "<" << minSNR
				         << "质量=" << snrRes.second;
			}
			continue;
		}
		Tools::FocusedStar s{};
		s.x = cx; s.y = cy; s.flux = flux; s.hfr = hfr; s.radius = radius; s.area = area;
		s.snr = snr; s.snrQuality = snrRes.second;
		stars.push_back(s);
	}
	if (verbose) {
		int totalDetected = static_cast<int>(stars.size());
		qDebug() << "合焦小星（SNR过滤后）数量:" << totalDetected;
	}
	return stars;
}

} // namespace

// ------------------------ Focused star detection public APIs ------------------------
std::vector<Tools::FocusedStar> Tools::DetectFocusedStars(const cv::Mat& image16,
                                                          double kSigma,
                                                          int minArea,
                                                          int maxArea,
                                                          double minSNR,
                                                          int bgKsize,
                                                          double smoothSigma,
                                                          bool verbose) {
	if (image16.empty()) return {};
	// Step 1: 前景图（背景扣除）
	if (verbose) qDebug() << "[Focused] Step 1 预处理（背景扣除+平滑）";
	cv::Mat fg = preprocessFocused(image16, bgKsize, smoothSigma);
	// Step 2: 小星检测（全图）
	if (verbose) qDebug() << "[Focused] Step 2 小星检测（全图阈值+连通域）";
	cv::Mat bgForSnr = toFloat32Normalized(image16);
	std::vector<Tools::FocusedStar> allStars = detectSmallStarsFocused(fg, bgForSnr, kSigma, minArea, maxArea, minSNR, verbose);
	// Step 3: 噪声过滤（合焦策略）
	if (verbose) qDebug() << "[Focused] Step 3 噪声过滤（合焦规则）";
	allStars = filterNoiseFocused(allStars);
	// Step 4: 去重
	if (verbose) qDebug() << "[Focused] Step 4 去重";
	allStars = removeDuplicatesFocused(allStars);
	if (verbose) qDebug() << "[Focused] 完成，星点数量:" << static_cast<int>(allStars.size());
	return allStars;
}

int Tools::DetectFocusedStarsFromFITS(const char* fileName,
                                      std::vector<Tools::FocusedStar>& outStars,
                                      bool verbose) {
	outStars.clear();
	cv::Mat img;
	int status = Tools::readFits(fileName, img);
	if (status != 0) return status;
	outStars = Tools::DetectFocusedStars(img, 3.5, 3, 200, 3.0, 51, 1.0, verbose);
	return 0;
}

double Tools::MedianHFR(const std::vector<Tools::FocusedStar>& stars) {
	if (stars.empty()) return 0.0;
	std::vector<double> hfrs; hfrs.reserve(stars.size());
	for (const auto& s : stars) if (s.hfr > 0.0) hfrs.push_back(s.hfr);
	if (hfrs.empty()) return 0.0;
	std::sort(hfrs.begin(), hfrs.end());
	size_t n = hfrs.size();
	if (n % 2 == 1) return hfrs[n / 2];
	return 0.5 * (hfrs[n / 2 - 1] + hfrs[n / 2]);
}


