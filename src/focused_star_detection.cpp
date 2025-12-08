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
                                                          double fluxPercentile = 10.0,
                                                          bool verbose = false) {
	if (stars.empty()) {
		if (verbose) {
			Logger::Log("[Focused][Filter] 输入星点为空，跳过噪声过滤",
			            LogLevel::DEBUG, DeviceType::MAIN);
		}
		return stars;
	}
	if (verbose) {
		Logger::Log("[Focused][Filter] Step0 输入星点数量 = " + std::to_string(stars.size()),
		            LogLevel::DEBUG, DeviceType::MAIN);
		for (size_t i = 0; i < stars.size(); ++i) {
			const auto &s = stars[i];
			std::string msg = "  原始星点 " + std::to_string(i) +
			                  " pos=(" + std::to_string(s.x) + "," + std::to_string(s.y) + ")" +
			                  " flux=" + std::to_string(s.flux) +
			                  " hfr=" + std::to_string(s.hfr) +
			                  " radius=" + std::to_string(s.radius);
			Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
		}
	}
	// 这里原本有“星点数量<=3 直接视为噪声”的规则，
	// 但在小视场/单星 ROI 场景下会误杀目标星，故取消此硬性数量阈值，
	// 改为完全依赖后续的 flux/HFR/半径 等约束进行过滤。
	// 过滤 HFR == 1
	std::vector<Tools::FocusedStar> filtered;
	filtered.reserve(stars.size());
	for (size_t i = 0; i < stars.size(); ++i) {
		const auto &s = stars[i];
		if (std::abs(s.hfr - 1.0) <= 0.01) {
			if (verbose) {
				std::string msg = "[Focused][Filter] 丢弃星点(索引 " + std::to_string(i) +
				                  ") 原因:HFR≈1.0 pos=(" + std::to_string(s.x) + "," +
				                  std::to_string(s.y) + ") hfr=" + std::to_string(s.hfr);
				Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
			}
			continue;
		}
		filtered.push_back(s);
	}
	if (filtered.empty()) {
		if (verbose) {
			Logger::Log("[Focused][Filter] 经过 HFR==1 过滤后星点为空",
			            LogLevel::DEBUG, DeviceType::MAIN);
		}
		return filtered;
	}
	// 计算 min_flux
	std::vector<double> fluxes;
	for (const auto& s : filtered) if (s.flux > 0.0) fluxes.push_back(s.flux);
	double minFlux = 0.0;
	if (!fluxes.empty()) minFlux = percentile(fluxes, fluxPercentile);
	if (verbose) {
		std::string msg = "[Focused][Filter] 根据 flux 百分位计算得到 minFlux = " +
		                  std::to_string(minFlux) + " (percentile = " +
		                  std::to_string(fluxPercentile) + ")";
		Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
	}
	// 一般约束
	std::vector<Tools::FocusedStar> filtered2;
	for (size_t i = 0; i < filtered.size(); ++i) {
		const auto &s = filtered[i];
		if (s.flux < minFlux) {
			if (verbose) {
				std::string msg = "[Focused][Filter] 丢弃星点(中间索引 " + std::to_string(i) +
				                  ") 原因:flux < minFlux flux=" + std::to_string(s.flux) +
				                  " minFlux=" + std::to_string(minFlux);
				Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
			}
			continue;
		}
		if (s.hfr < minHFR || s.hfr > maxHFR) {
			if (verbose) {
				std::string msg = "[Focused][Filter] 丢弃星点(中间索引 " + std::to_string(i) +
				                  ") 原因:HFR越界 hfr=" + std::to_string(s.hfr) +
				                  " 允许范围[" + std::to_string(minHFR) + "," +
				                  std::to_string(maxHFR) + "]";
				Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
			}
			continue;
		}
		if (s.hfr > 0.0 && s.radius > 0.0) {
			double ratio = s.hfr / s.radius;
			if (ratio > 2.5) {
				if (verbose) {
					std::string msg = "[Focused][Filter] 丢弃星点(中间索引 " + std::to_string(i) +
					                  ") 原因:HFR/半径比过大 ratio=" + std::to_string(ratio);
					Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
				}
				continue;
			}
			if (ratio < 0.1) {
				if (verbose) {
					std::string msg = "[Focused][Filter] 丢弃星点(中间索引 " + std::to_string(i) +
					                  ") 原因:HFR/半径比过小 ratio=" + std::to_string(ratio);
					Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
				}
				continue;
			}
		}
		filtered2.push_back(s);
	}
	// 这里原先还有一个“过滤后若数量仍<=3 且 HFR/半径变化很小则全部丢弃”的特判，
	// 同样会在单星 / 少星 ROI 场景下误杀目标星，已移除，仅保留上面的物理约束过滤。
	if (verbose) {
		Logger::Log("[Focused][Filter] 最终保留星点数量 = " + std::to_string(filtered2.size()),
		            LogLevel::DEBUG, DeviceType::MAIN);
		for (size_t i = 0; i < filtered2.size(); ++i) {
			const auto &s = filtered2[i];
			std::string msg = "  保留星点 " + std::to_string(i) +
			                  " pos=(" + std::to_string(s.x) + "," + std::to_string(s.y) + ")" +
			                  " flux=" + std::to_string(s.flux) +
			                  " hfr=" + std::to_string(s.hfr) +
			                  " radius=" + std::to_string(s.radius);
			Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
		}
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
	if (verbose) {
		std::string msg = "[Focused][Detect] Step2 阈值估计 kSigma=" +
		                  std::to_string(kSigma) + " thr=" + std::to_string(thr);
		Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
	}
	cv::threshold(fg, bin, thr, 1.0, cv::THRESH_BINARY);
	cv::Mat binU8;
	bin.convertTo(binU8, CV_8U, 255.0);
	cv::Mat labels, stats, centroids;
	int numLabels = cv::connectedComponentsWithStats(binU8, labels, stats, centroids, 8, CV_32S);
	if (verbose) {
		Logger::Log("[Focused][Detect] 初始连通域个数(含背景) = " + std::to_string(numLabels),
		            LogLevel::DEBUG, DeviceType::MAIN);
	}
	std::vector<Tools::FocusedStar> stars;
	for (int lab = 1; lab < numLabels; ++lab) {
		int area = stats.at<int>(lab, cv::CC_STAT_AREA);
		if (area <= 10) { // 排除超小噪点（对齐Python逻辑）
			if (verbose) {
				std::string msg = "[Focused][Detect] 丢弃连通域 label=" +
				                  std::to_string(lab) +
				                  " 原因: area <= 30 area=" + std::to_string(area);
				Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
			}
			continue;
		}
		// 若 maxArea <= 0，则不做“最大面积”限制，仅限制最小面积；
		// 若 maxArea > 0，则要求 area 落在 [minArea, maxArea] 区间内
		if (area < minArea || (maxArea > 0 && area > maxArea)) {
			if (verbose) {
				std::string msg = "[Focused][Detect] 丢弃连通域 label=" +
				                  std::to_string(lab) +
				                  " 原因: area 不在允许范围; minArea=" +
				                  std::to_string(minArea) +
				                  " maxArea=" + std::to_string(maxArea) +
				                  " area=" + std::to_string(area);
				Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
			}
			continue;
		}
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
				std::string msg = "[Focused][Detect] SNR过滤 label=" +
				                  std::to_string(lab) +
				                  " 位置(" + std::to_string(cx) + "," + std::to_string(cy) +
				                  ") SNR=" + std::to_string(snr) +
				                  "<" + std::to_string(minSNR) +
				                  " 质量=" + snrRes.second.toStdString();
				Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
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
		Logger::Log("[Focused][Detect] 合焦小星（面积+SNR过滤后）数量: " + std::to_string(totalDetected),
		            LogLevel::DEBUG, DeviceType::MAIN);
		for (int i = 0; i < totalDetected; ++i) {
			const auto &s = stars[i];
			std::string msg = "  通过小星检测星点 " + std::to_string(i) +
			                  " pos=(" + std::to_string(s.x) + "," + std::to_string(s.y) + ")" +
			                  " area=" + std::to_string(s.area) +
			                  " flux=" + std::to_string(s.flux) +
			                  " hfr=" + std::to_string(s.hfr) +
			                  " snr=" + std::to_string(s.snr) +
			                  " 质量=" + s.snrQuality.toStdString();
			Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
		}
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
	if (verbose) {
		Logger::Log("[Focused] Step 1 预处理（背景扣除+平滑）",
		            LogLevel::DEBUG, DeviceType::MAIN);
	}
	cv::Mat fg = preprocessFocused(image16, bgKsize, smoothSigma);
	// Step 2: 小星检测（全图）
	if (verbose) {
		Logger::Log("[Focused] Step 2 小星检测（全图阈值+连通域）",
		            LogLevel::DEBUG, DeviceType::MAIN);
	}
	cv::Mat bgForSnr = toFloat32Normalized(image16);
	std::vector<Tools::FocusedStar> allStars = detectSmallStarsFocused(fg, bgForSnr, kSigma, minArea, maxArea, minSNR, verbose);
	// Step 3: 噪声过滤（合焦策略）
	if (verbose) {
		Logger::Log("[Focused] Step 3 噪声过滤（合焦规则）",
		            LogLevel::DEBUG, DeviceType::MAIN);
	}
	allStars = filterNoiseFocused(allStars, 0.5, 50.0, 10.0, verbose);
	// Step 4: 去重
	if (verbose) {
		Logger::Log("[Focused] Step 4 去重", LogLevel::DEBUG, DeviceType::MAIN);
		Logger::Log("[Focused][BeforeDedup] 星点数量 = " + std::to_string(allStars.size()),
		            LogLevel::DEBUG, DeviceType::MAIN);
	}
	allStars = removeDuplicatesFocused(allStars);
	if (verbose) {
		Logger::Log("[Focused] 完成，星点数量: " + std::to_string(static_cast<int>(allStars.size())),
		            LogLevel::DEBUG, DeviceType::MAIN);
		for (size_t i = 0; i < allStars.size(); ++i) {
			const auto &s = allStars[i];
			std::string msg = "  最终星点 " + std::to_string(i) +
			                  " pos=(" + std::to_string(s.x) + "," + std::to_string(s.y) + ")" +
			                  " flux=" + std::to_string(s.flux) +
			                  " hfr=" + std::to_string(s.hfr) +
			                  " radius=" + std::to_string(s.radius) +
			                  " snr=" + std::to_string(s.snr) +
			                  " 质量=" + s.snrQuality.toStdString();
			Logger::Log(msg, LogLevel::DEBUG, DeviceType::MAIN);
		}
	}
	return allStars;
}

int Tools::DetectFocusedStarsFromFITS(const char* fileName,
                                      std::vector<Tools::FocusedStar>& outStars,
                                      bool verbose) {
	outStars.clear();
	cv::Mat img;
	int status = Tools::readFits(fileName, img);
	if (status != 0) return status;
	// 这里将 maxArea 设为 0，表示“不限制最大面积”，仅由 minArea 等参数约束
	outStars = Tools::DetectFocusedStars(img, 3.5, 3, 0, 3.0, 51, 1.0, verbose);
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


