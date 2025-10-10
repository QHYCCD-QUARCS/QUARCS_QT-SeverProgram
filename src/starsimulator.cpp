#include "starsimulator.h"
#include "tools.h"
#include <QApplication>
#include <QThread>
#include <QtMath>
#include <QDir>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutexLocker>

StarSimulator::StarSimulator(QObject *parent)
    : QObject(parent)
    , m_noiseLevel(0.005) // 进一步降低默认噪声水平
    , m_atmosphericTurbulence(0.05) // 进一步降低默认大气扰动
    , m_totalStarsGenerated(0)
    , m_validStarsGenerated(0)
    , m_randomGenerator(new QRandomGenerator())
    , m_mtGenerator(std::chrono::steady_clock::now().time_since_epoch().count())
    , m_uniformDist(0.0, 1.0)
    , m_normalDist(0.0, 1.0)
    , m_poissonDist(5.0) // 泊松分布用于星点数量
    , m_brightnessDist(0.7, 0.2) // 提高亮度分布中心值和标准差
{
    // 设置默认对焦曲线
    m_focusCurve.bestPosition = 5000.0;
    m_focusCurve.minHFR = 1.0;
    m_focusCurve.curveWidth = 5000.0;
    m_focusCurve.asymmetry = 0.0;
    
    // 初始化大气扰动参数
    m_turbulenceParams.scale = 50.0;
    m_turbulenceParams.intensity = 0.05; // 进一步降低默认扰动强度
    m_turbulenceParams.timeScale = 0.1;
    
    // 初始化默认望远镜参数（典型的中等口径望远镜）
    m_telescopeParams.focalLength = 510.0;    // 510mm焦距
    m_telescopeParams.aperture = 85.0;         // 85mm口径
    m_telescopeParams.obstruction = 0;       // 0%遮挡
    m_telescopeParams.seeing = 2.0;             // 2arcsec视宁度
    m_telescopeParams.pixelSize = 3.76;         // 3.76μm像素 （1080p）
    m_telescopeParams.binning = 1.0;            // 1x1合并
    m_telescopeParams.filterTransmission = 1; // 100%透过率
    m_telescopeParams.exposureTime = 1.0;       // 1秒曝光
    
    log("星图模拟器已创建，使用默认望远镜参数");
}

StarSimulator::~StarSimulator()
{
    delete m_randomGenerator;
}

bool StarSimulator::generateStarImage(const StarImageParams &params)
{
    QMutexLocker locker(&m_mutex);
    
    try {
        log(QString("开始生成星图: %1x%2, 电调位置=%3, 输出路径=%4")
            .arg(params.imageWidth).arg(params.imageHeight)
            .arg(params.focuserCurrentPos).arg(params.outputPath));
        
        // 初始化模拟器
        initialize(params);
        
        // 生成星点数据
        generateStars(params);
        
        // 生成图像
        QImage image = generateImage(params);
        
        // 保存图像
        bool saveSuccess = false;
        if (params.outputPath.toLower().endsWith(".fits")) {
            saveSuccess = saveAsFITS(image, params.outputPath);
        } else {
            saveSuccess = saveImage(image, params.outputPath);
        }
        
        if (saveSuccess) {
            m_lastGeneratedImage = image;
            m_lastGenerationTime = QDateTime::currentDateTime();
            
            log(QString("星图生成成功: 有效星点=%1, 总星点=%2")
                .arg(m_validStarsGenerated).arg(m_totalStarsGenerated));
            
            emit imageGenerated(params.outputPath);
            return true;
        } else {
            log("图像保存失败");
            return false;
        }
        
    } catch (const std::exception &e) {
        log(QString("星图生成异常: %1").arg(e.what()));
        return false;
    }
}

bool StarSimulator::generateStarImage(int imageWidth, int imageHeight, 
                                     int focuserMinPos, int focuserMaxPos, int focuserCurrentPos,
                                     const QString &outputPath)
{
    StarImageParams params;
    params.imageWidth = imageWidth;
    params.imageHeight = imageHeight;
    params.focuserMinPos = focuserMinPos;
    params.focuserMaxPos = focuserMaxPos;
    params.focuserCurrentPos = focuserCurrentPos;
    params.outputPath = outputPath;
    
    return generateStarImage(params);
}

QList<FITSImage::Star> StarSimulator::getGeneratedStars() const
{
    return m_fitsStars;
}

QImage StarSimulator::getLastGeneratedImage() const
{
    return m_lastGeneratedImage;
}

void StarSimulator::setFocusCurve(const FocusCurve &curve)
{
    QMutexLocker locker(&m_mutex);
    m_focusCurve = curve;
    log(QString("对焦曲线已更新: 最佳位置=%1, 最小HFR=%2, 曲线宽度=%3")
        .arg(curve.bestPosition).arg(curve.minHFR).arg(curve.curveWidth));
}

void StarSimulator::setNoiseParameters(double noiseLevel, double atmosphericTurbulence)
{
    QMutexLocker locker(&m_mutex);
    m_noiseLevel = noiseLevel;
    m_atmosphericTurbulence = atmosphericTurbulence;
    log(QString("噪声参数已设置: 噪声水平=%1, 大气扰动=%2")
        .arg(noiseLevel).arg(atmosphericTurbulence));
}

void StarSimulator::setTurbulenceParameters(double scale, double intensity, double timeScale)
{
    QMutexLocker locker(&m_mutex);
    m_turbulenceParams.scale = scale;
    m_turbulenceParams.intensity = intensity;
    m_turbulenceParams.timeScale = timeScale;
    log(QString("大气扰动参数已设置: 尺度=%1, 强度=%2, 时间尺度=%3")
        .arg(scale).arg(intensity).arg(timeScale));
}

QString StarSimulator::getGenerationStats() const
{
    return QString("星图生成统计:\n"
                  "总生成星点: %1\n"
                  "有效星点: %2\n"
                  "最后生成时间: %3\n"
                  "噪声水平: %4\n"
                  "大气扰动: %5\n"
                  "扰动尺度: %6\n"
                  "扰动强度: %7")
            .arg(m_totalStarsGenerated)
            .arg(m_validStarsGenerated)
            .arg(m_lastGenerationTime.toString("yyyy-MM-dd hh:mm:ss"))
            .arg(m_noiseLevel)
            .arg(m_atmosphericTurbulence)
            .arg(m_turbulenceParams.scale)
            .arg(m_turbulenceParams.intensity);
}

void StarSimulator::initialize(const StarImageParams &params)
{
    // 清空之前的数据
    m_stars.clear();
    m_fitsStars.clear();
    
    // 更新对焦曲线的最佳位置
    m_focusCurve.bestPosition = (params.focuserMaxPos + params.focuserMinPos) / 2.0;
    
    log(QString("模拟器初始化完成: 图像大小=%1x%2, 电调范围=%3-%4")
        .arg(params.imageWidth).arg(params.imageHeight)
        .arg(params.focuserMinPos).arg(params.focuserMaxPos));
}

void StarSimulator::generateStars(const StarImageParams &params)
{
    QElapsedTimer timer;
    timer.start();
    
    // 使用泊松分布生成更真实的星点数量
    int baseStarCount = (params.minStarCount + params.maxStarCount) / 2;
    m_poissonDist.param(std::poisson_distribution<int>::param_type(baseStarCount));
    int starCount = m_poissonDist(m_mtGenerator);
    starCount = qBound(params.minStarCount, starCount, params.maxStarCount);
    
    m_totalStarsGenerated = starCount;
    m_validStarsGenerated = 0;
    
    log(QString("开始生成%1个星点").arg(starCount));
    
    // 预分配内存以提高性能
    m_stars.reserve(starCount);
    m_fitsStars.reserve(starCount);
    
    // 生成星点网格以避免重叠
    QVector<QPointF> starPositions = generateStarPositions(params.imageWidth, params.imageHeight, starCount);
    
    for (int i = 0; i < starCount; ++i) {
        SimulatedStar star = generateStarAtPosition(starPositions[i], params.imageWidth, params.imageHeight);
        
        // 计算HFR值
        star.hfr = calculateHFR(params.focuserCurrentPos, star);
        
        // 检查星点是否有效
        if (isValidStar(star, params.imageWidth, params.imageHeight)) {
            m_stars.append(star);
            m_validStarsGenerated++;
            
            // 转换为FITS格式
            FITSImage::Star fitsStar;
            fitsStar.x = star.position.x();
            fitsStar.y = star.position.y();
            fitsStar.HFR = star.hfr;
            fitsStar.peak = star.brightness;
            fitsStar.flux = star.brightness * 1000.0;
            // 移除不存在的成员变量
            // fitsStar.background = 0.1 + m_normalDist(m_mtGenerator) * 0.05;
            // fitsStar.HFR = star.hfr;
            
            // 计算椭圆参数
            double eccentricity = 0.1 + m_uniformDist(m_mtGenerator) * 0.2;
            double angle = m_uniformDist(m_mtGenerator) * 2 * M_PI;
            double fwhm = star.hfr * 2.355;
            fitsStar.a = fwhm * (1.0 + eccentricity);
            fitsStar.b = fwhm * (1.0 - eccentricity);
            fitsStar.theta = angle;
            
            m_fitsStars.append(fitsStar);
        }
    }
    
    qint64 elapsed = timer.elapsed();
    log(QString("星点生成完成: 有效星点=%1/%2, 耗时=%3ms")
        .arg(m_validStarsGenerated).arg(m_totalStarsGenerated).arg(elapsed));
}

double StarSimulator::calculateHFR(int focuserPosition, const SimulatedStar &star)
{
    // 获取设备参数
    const TelescopeParams &scope = m_telescopeParams;
    
    // 1. 计算基础衍射极限 (Dawes极限)
    double dawesLimit = 116.0 / scope.aperture; // arcsec
    
    // 2. 计算像素尺度 (arcsec/pixel)
    double pixelScale = (scope.pixelSize * scope.binning * 206.265) / scope.focalLength; // arcsec/pixel
    
    // 3. 计算视宁度影响 (转换为像素)
    double seeingPixels = scope.seeing / pixelScale;
    
    // 4. 计算衍射极限 (转换为像素)
    double diffractionPixels = dawesLimit / pixelScale;
    
    // 5. 计算遮挡影响
    double obstructionFactor = 1.0 + (scope.obstruction * scope.obstruction);
    
    // 6. 计算最佳对焦时的理论HFR
    double bestHFR = qSqrt(seeingPixels * seeingPixels + 
                           diffractionPixels * diffractionPixels * obstructionFactor);
    
    // 7. 计算焦距偏离的影响
    double distance = qAbs(focuserPosition - m_focusCurve.bestPosition);
    double focusRange = m_focusCurve.curveWidth;
    double normalizedDistance = distance / focusRange;
    
    // 8. 散焦导致的额外模糊 (使用更真实的散焦模型)
    // 散焦时，星点会形成艾里盘，HFR随散焦距离线性增加
    double defocusBlur = normalizedDistance * 8.0; // 最大8像素模糊，线性关系
    
    // 9. 计算最终HFR (使用平方和根，更符合物理模型)
    double finalHFR = qSqrt(bestHFR * bestHFR + defocusBlur * defocusBlur);
    
    // 10. 添加不对称性
    if (focuserPosition > m_focusCurve.bestPosition) {
        finalHFR *= (1.0 + m_focusCurve.asymmetry);
    } else {
        finalHFR *= (1.0 - m_focusCurve.asymmetry);
    }
    
    // 11. 添加大气扰动影响
    double turbulenceEffect = m_atmosphericTurbulence * normalizedDistance * 1.5;
    finalHFR += turbulenceEffect;
    
    // 12. 添加小的随机噪声 (模拟测量误差)
    double measurementNoise = m_normalDist(m_mtGenerator) * 0.1;
    finalHFR += measurementNoise;
    
    // 13. 确保HFR在合理范围内
    finalHFR = qBound(0.5, finalHFR, 20.0);
    
    // 记录第一个星点的HFR计算详情（用于调试）
    if (m_totalStarsGenerated == 0) {
        log(QString("真实HFR计算详情:"));
        log(QString("  设备参数: 焦距=%1mm, 口径=%2mm, 像素=%3μm, 合并=%4x%4")
            .arg(scope.focalLength).arg(scope.aperture).arg(scope.pixelSize).arg(scope.binning));
        log(QString("  像素尺度: %1 arcsec/pixel").arg(pixelScale, 0, 'f', 3));
        log(QString("  视宁度: %1 arcsec (%2 pixels)").arg(scope.seeing).arg(seeingPixels, 0, 'f', 2));
        log(QString("  衍射极限: %1 arcsec (%2 pixels)").arg(dawesLimit, 0, 'f', 3).arg(diffractionPixels, 0, 'f', 2));
        log(QString("  最佳HFR: %1 pixels").arg(bestHFR, 0, 'f', 2));
        log(QString("  当前位置: %1, 最佳位置: %2, 距离: %3").arg(focuserPosition).arg(m_focusCurve.bestPosition).arg(distance));
        log(QString("  散焦模糊: %1 pixels").arg(defocusBlur, 0, 'f', 2));
        log(QString("  最终HFR: %1 pixels").arg(finalHFR, 0, 'f', 2));
    }
    
    return finalHFR;
}

double StarSimulator::getTheoreticalHFR(int focuserPosition) const
{
    // 计算到最佳对焦位置的距离
    double distance = qAbs(focuserPosition - m_focusCurve.bestPosition);
    
    // 计算焦距偏离程度（归一化到0-1范围）
    double focusRange = m_focusCurve.curveWidth;
    double normalizedDistance = distance / focusRange;
    
    // 使用与calculateHFR相同的计算公式
    double maxHFRIncrease = 8.0; // 最大HFR增加量
    double baseHFR = m_focusCurve.minHFR + 
                     (normalizedDistance * normalizedDistance) * maxHFRIncrease;
    
    // 添加不对称性（模拟不同方向的散焦差异）
    if (focuserPosition > m_focusCurve.bestPosition) {
        baseHFR *= (1.0 + m_focusCurve.asymmetry);
    } else {
        baseHFR *= (1.0 - m_focusCurve.asymmetry);
    }
    
    // 添加大气扰动影响（与焦距偏离成正比）
    double turbulenceEffect = m_atmosphericTurbulence * normalizedDistance * 2.0;
    baseHFR += turbulenceEffect;
    
    // 确保HFR在合理范围内
    baseHFR = qBound(m_focusCurve.minHFR, baseHFR, 20.0);
    
    return baseHFR;
}

QImage StarSimulator::generateImage(const StarImageParams &params)
{
    try {
        QElapsedTimer timer;
        timer.start();
        
        // 检查参数有效性
        if (params.imageWidth <= 0 || params.imageHeight <= 0) {
            log("错误：图像尺寸无效");
            return QImage();
        }
        
        // 创建16位灰度图像
        QImage image(params.imageWidth, params.imageHeight, QImage::Format_Grayscale16);
        if (image.isNull()) {
            log("错误：无法创建图像");
            return QImage();
        }
        
        image.fill(0); // 黑色背景
        
        QPainter painter(&image);
        if (!painter.isActive()) {
            log("错误：无法创建画笔");
            return QImage();
        }
        
        painter.setRenderHint(QPainter::Antialiasing);
        
        // 绘制星点
        for (const auto &star : m_stars) {
            drawStar(painter, star, params.exposureTime);
        }
        
        painter.end();
        
        // 添加大气扰动
        if (m_atmosphericTurbulence > 0.0) {
            applyAtmosphericTurbulence(image);
        }
        
        // 添加噪声
        addNoiseToImage(image, params.noiseLevel);
        
        qint64 elapsed = timer.elapsed();
        log(QString("图像生成完成，耗时=%1ms").arg(elapsed));
        
        return image;
        
    } catch (const std::exception &e) {
        log(QString("图像生成异常: %1").arg(e.what()));
        return QImage();
    } catch (...) {
        log("图像生成发生未知异常");
        return QImage();
    }
}

void StarSimulator::addNoiseToImage(QImage &image, double noiseLevel)
{
    try {
        int width = image.width();
        int height = image.height();
        
        // 检查图像有效性
        if (image.isNull() || width <= 0 || height <= 0) {
            log("错误：图像无效，无法添加噪声");
            return;
        }
        
        // 进一步降低噪声水平，提高星点可见性
        double reducedNoiseLevel = noiseLevel * 0.01; // 降低到原来的1%
        
        // 使用更真实的噪声模型：高斯噪声 + 泊松噪声
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                QRgb pixel = image.pixel(x, y);
                int gray = qGray(pixel);
                
                // 高斯噪声（模拟电子噪声）- 进一步降低噪声水平
                double gaussianNoise = m_normalDist(m_mtGenerator) * reducedNoiseLevel * 50; // 从200降到50
                
                // 泊松噪声（模拟光子噪声）- 进一步降低光子噪声
                double photonNoise = 0.0;
                if (gray > 0) {
                    double photonCount = gray * 0.0001; // 从0.001降到0.0001
                    std::poisson_distribution<int> photonDist(photonCount);
                    int actualPhotons = photonDist(m_mtGenerator);
                    photonNoise = (actualPhotons - photonCount) * 2.0; // 从10降到2
                }
                
                // 组合噪声
                double totalNoise = gaussianNoise + photonNoise;
                gray += static_cast<int>(totalNoise);
                gray = qBound(0, gray, 65535); // 16位范围
                
                image.setPixel(x, y, qRgb(gray, gray, gray));
            }
        }
        
        log(QString("噪声添加完成: 噪声水平=%1, 降低后=%2").arg(noiseLevel).arg(reducedNoiseLevel));
    } catch (const std::exception &e) {
        log(QString("添加噪声异常: %1").arg(e.what()));
    } catch (...) {
        log("添加噪声发生未知异常");
    }
}

void StarSimulator::drawStar(QPainter &painter, const SimulatedStar &star, double exposureTime)
{
    try {
        // 计算星点大小（基于HFR）
        double starSize = qMax(1.0, qMin(star.hfr * 0.8, 5.0)); // 限制最大大小5像素
        
        // 计算亮度
        double brightness = star.brightness * exposureTime * 8.0; // 适度亮度
        brightness = qBound(0.6, brightness, 1.0);
        
        QPointF center = star.position;
        int radius = qMax(2, static_cast<int>(starSize * 2.0)); // 增加半径以容纳虚像
        
        // 检查图像边界
        QImage *image = static_cast<QImage*>(painter.device());
        if (!image) {
            log("错误：无法获取图像设备");
            return;
        }
        
        int imageWidth = image->width();
        int imageHeight = image->height();
        
        // 绘制散焦星点：中心实点 + 周围虚像
        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                double distance = qSqrt(x*x + y*y);
                if (distance <= radius) {
                    double intensity = 0.0;
                    
                    // 计算散焦程度（HFR越大，散焦越严重）
                    double defocusFactor = qMin(star.hfr / 8.0, 1.0); // 归一化散焦因子
                    
                    if (distance <= starSize * 0.5) {
                        // 中心实点：高亮度，小范围
                        intensity = brightness * (1.0 - distance / (starSize * 0.5));
                    } else if (distance <= starSize) {
                        // 实点边缘到虚像的过渡区域
                        double transitionFactor = (distance - starSize * 0.5) / (starSize * 0.5);
                        double coreIntensity = brightness * (1.0 - transitionFactor);
                        double virtualIntensity = brightness * 0.4 * (1.0 - defocusFactor) * transitionFactor;
                        intensity = coreIntensity + virtualIntensity;
                    } else {
                        // 周围虚像：低亮度，大范围，随散焦程度增加
                        double virtualRadius = starSize + defocusFactor * (radius - starSize);
                        if (distance <= virtualRadius) {
                            // 虚像亮度随距离和散焦程度衰减
                            double virtualIntensity = brightness * 0.2 * (1.0 - defocusFactor) * 
                                                    (1.0 - (distance - starSize) / (virtualRadius - starSize));
                            intensity = virtualIntensity;
                        }
                    }
                    
                    intensity = qMax(0.0, intensity);
                    
                    // 转换为像素值
                    int pixelGray = static_cast<int>(intensity * 65535);
                    pixelGray = qBound(0, pixelGray, 65535);
                    
                    int pixelX = static_cast<int>(center.x()) + x;
                    int pixelY = static_cast<int>(center.y()) + y;
                    
                    // 边界检查
                    if (pixelX >= 0 && pixelX < imageWidth &&
                        pixelY >= 0 && pixelY < imageHeight) {
                        
                        // 获取当前像素值并混合
                        QRgb currentPixel = image->pixel(pixelX, pixelY);
                        int currentGray = qGray(currentPixel);
                        
                        // 使用最大值混合，确保星点清晰
                        int newGray = qMax(currentGray, pixelGray);
                        image->setPixel(pixelX, pixelY, qRgb(newGray, newGray, newGray));
                    }
                }
            }
        }
        
        // 记录第一个星点的绘制详情（用于调试）
        if (m_totalStarsGenerated == 0) {
            double defocusFactor = qMin(star.hfr / 8.0, 1.0); // 重新计算用于日志
            log(QString("星点绘制详情: HFR=%1, 大小=%2, 亮度=%3, 半径=%4, 散焦因子=%5")
                .arg(star.hfr, 0, 'f', 2).arg(starSize, 0, 'f', 2)
                .arg(brightness, 0, 'f', 3).arg(radius).arg(defocusFactor, 0, 'f', 2));
        }
        
    } catch (const std::exception &e) {
        log(QString("绘制星点异常: %1").arg(e.what()));
    } catch (...) {
        log("绘制星点发生未知异常");
    }
}

bool StarSimulator::saveImage(const QImage &image, const QString &path)
{
    // 确保输出目录存在
    QFileInfo fileInfo(path);
    QDir dir = fileInfo.dir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    
    // 检查并删除已存在的文件
    if (fileInfo.exists()) {
        QFile existingFile(path);
        if (existingFile.remove()) {
            log(QString("已删除已存在的图像文件: %1").arg(path));
        } else {
            log(QString("警告：无法删除已存在的图像文件: %1").arg(path));
        }
    }
    
    // 保存图像
    if (image.save(path)) {
        log(QString("图像已保存: %1").arg(path));
        return true;
    } else {
        log(QString("图像保存失败: %1").arg(path));
        return false;
    }
}

SimulatedStar StarSimulator::generateRandomStar(int imageWidth, int imageHeight)
{
    SimulatedStar star;
    
    // 随机位置（避免边缘）
    double margin = 50.0;
    star.position.setX(margin + m_uniformDist(m_mtGenerator) * (imageWidth - 2 * margin));
    star.position.setY(margin + m_uniformDist(m_mtGenerator) * (imageHeight - 2 * margin));
    
    // 随机亮度
    star.brightness = 0.3 + m_uniformDist(m_mtGenerator) * 0.7;
    
    // 随机大小
    star.size = 1.0 + m_uniformDist(m_mtGenerator) * 3.0;
    
    // 随机移动速度（模拟地球自转）
    double baseSpeed = 0.1; // 像素/秒
    double speedVariation = 0.05;
    star.velocity.setX(baseSpeed + m_normalDist(m_mtGenerator) * speedVariation);
    star.velocity.setY(baseSpeed + m_normalDist(m_mtGenerator) * speedVariation);
    
    // 亮度变化参数
    star.brightnessVariation = 0.1 + m_uniformDist(m_mtGenerator) * 0.2;
    star.phase = m_uniformDist(m_mtGenerator) * 2 * M_PI;
    
    return star;
}

bool StarSimulator::isValidStar(const SimulatedStar &star, int imageWidth, int imageHeight)
{
    // 放宽HFR范围检查
    if (star.hfr < 0.5 || star.hfr > 20.0) {
        return false;
    }
    
    // 放宽亮度检查
    if (star.brightness < 0.3) {
        return false;
    }
    
    // 检查位置是否在图像范围内
    if (star.position.x() < 0 || star.position.x() >= imageWidth ||
        star.position.y() < 0 || star.position.y() >= imageHeight) {
        return false;
    }
    
    return true;
}

void StarSimulator::log(const QString &message)
{
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString logMessage = QString("[StarSimulator] %1: %2").arg(timestamp).arg(message);
    
    qDebug() << logMessage;
    // emit logMessage(logMessage);
} 

SimulatedStar StarSimulator::generateStarAtPosition(const QPointF &position, int imageWidth, int imageHeight)
{
    SimulatedStar star;
    star.position = position;
    
    // 使用正态分布生成更真实的亮度 - 大幅提高亮度范围
    star.brightness = m_brightnessDist(m_mtGenerator);
    star.brightness = qBound(0.6, star.brightness, 1.0); // 大幅提高最小亮度从0.4到0.6
    
    // 随机大小（基于亮度）
    star.size = 1.0 + star.brightness * 4.0 + m_uniformDist(m_mtGenerator) * 3.0; // 增加大小范围
    
    // 随机移动速度（模拟地球自转）
    double baseSpeed = 0.1; // 像素/秒
    double speedVariation = 0.05;
    star.velocity.setX(baseSpeed + m_normalDist(m_mtGenerator) * speedVariation);
    star.velocity.setY(baseSpeed + m_normalDist(m_mtGenerator) * speedVariation);
    
    // 亮度变化参数
    star.brightnessVariation = 0.1 + m_uniformDist(m_mtGenerator) * 0.2;
    star.phase = m_uniformDist(m_mtGenerator) * 2 * M_PI;
    
    return star;
}

QVector<QPointF> StarSimulator::generateStarPositions(int imageWidth, int imageHeight, int starCount)
{
    QVector<QPointF> positions;
    positions.reserve(starCount);
    
    // 使用网格分布避免星点重叠
    int gridSize = qCeil(qSqrt(starCount * 2)); // 增加网格密度
    double cellWidth = static_cast<double>(imageWidth) / gridSize;
    double cellHeight = static_cast<double>(imageHeight) / gridSize;
    
    // 边缘留白
    double margin = 50.0;
    double usableWidth = imageWidth - 2 * margin;
    double usableHeight = imageHeight - 2 * margin;
    
    QVector<bool> usedCells(gridSize * gridSize, false);
    
    for (int i = 0; i < starCount; ++i) {
        int attempts = 0;
        QPointF position;
        
        do {
            // 随机选择网格单元
            int gridX = m_randomGenerator->bounded(gridSize);
            int gridY = m_randomGenerator->bounded(gridSize);
            int cellIndex = gridY * gridSize + gridX;
            
            if (!usedCells[cellIndex]) {
                // 在网格单元内随机位置
                double x = margin + gridX * cellWidth + m_uniformDist(m_mtGenerator) * cellWidth * 0.8;
                double y = margin + gridY * cellHeight + m_uniformDist(m_mtGenerator) * cellHeight * 0.8;
                
                position = QPointF(x, y);
                usedCells[cellIndex] = true;
                break;
            }
            
            attempts++;
        } while (attempts < 100); // 防止无限循环
        
        if (attempts >= 100) {
            // 如果无法找到合适位置，使用随机位置
            position.setX(margin + m_uniformDist(m_mtGenerator) * usableWidth);
            position.setY(margin + m_uniformDist(m_mtGenerator) * usableHeight);
        }
        
        positions.append(position);
    }
    
    return positions;
} 

void StarSimulator::applyAtmosphericTurbulence(QImage &image)
{
    int width = image.width();
    int height = image.height();
    
    // 创建扰动偏移图
    QImage turbulenceMap(width, height, QImage::Format_Grayscale16);
    turbulenceMap.fill(0);
    
    // 生成扰动模式
    for (int y = 0; y < height; y += m_turbulenceParams.scale) {
        for (int x = 0; x < width; x += m_turbulenceParams.scale) {
            // 生成随机扰动向量
            double angle = m_uniformDist(m_mtGenerator) * 2 * M_PI;
            double magnitude = m_normalDist(m_mtGenerator) * m_turbulenceParams.intensity;
            
            int offsetX = static_cast<int>(qCos(angle) * magnitude * m_turbulenceParams.scale);
            int offsetY = static_cast<int>(qSin(angle) * magnitude * m_turbulenceParams.scale);
            
            // 应用扰动到周围区域
            for (int dy = -m_turbulenceParams.scale/2; dy <= m_turbulenceParams.scale/2; ++dy) {
                for (int dx = -m_turbulenceParams.scale/2; dx <= m_turbulenceParams.scale/2; ++dx) {
                    int nx = x + dx;
                    int ny = y + dy;
                    
                    if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                        // 计算距离权重
                        double distance = qSqrt(dx*dx + dy*dy);
                        double weight = qMax(0.0, 1.0 - distance / (m_turbulenceParams.scale/2));
                        
                        int currentOffsetX = static_cast<int>(offsetX * weight);
                        int currentOffsetY = static_cast<int>(offsetY * weight);
                        
                        // 设置扰动偏移（16位范围）
                        int pixelX = qBound(0, nx + currentOffsetX, width - 1);
                        int pixelY = qBound(0, ny + currentOffsetY, height - 1);
                        
                        // 将偏移值编码到RGB通道中
                        int encodedX = qBound(0, pixelX, 65535);
                        int encodedY = qBound(0, pixelY, 65535);
                        turbulenceMap.setPixel(nx, ny, qRgb(encodedX, encodedY, 0));
                    }
                }
            }
        }
    }
    
    // 应用扰动到原图像
    QImage result = image.copy();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            QRgb turbulencePixel = turbulenceMap.pixel(x, y);
            int offsetX = qRed(turbulencePixel);
            int offsetY = qGreen(turbulencePixel);
            
            // 获取扰动后的像素值
            int sourceX = qBound(0, x + offsetX, width - 1);
            int sourceY = qBound(0, y + offsetY, height - 1);
            
            QRgb sourcePixel = image.pixel(sourceX, sourceY);
            result.setPixel(x, y, sourcePixel);
        }
    }
    
    image = result;
} 

bool StarSimulator::saveAsFITS(const QImage &image, const QString &path)
{
    try {
        // 检查图像是否有效
        if (image.isNull()) {
            log("错误：图像为空");
            return false;
        }
        
        // 确保输出目录存在
        QFileInfo fileInfo(path);
        QDir dir = fileInfo.dir();
        if (!dir.exists()) {
            if (!dir.mkpath(".")) {
                log(QString("错误：无法创建目录: %1").arg(dir.absolutePath()));
                return false;
            }
        }
        
        // 检查并删除已存在的文件
        if (fileInfo.exists()) {
            QFile existingFile(path);
            if (existingFile.remove()) {
                log(QString("已删除已存在的FITS文件: %1").arg(path));
            } else {
                log(QString("警告：无法删除已存在的FITS文件: %1").arg(path));
            }
        }
        
        // 检查图像格式
        if (image.format() != QImage::Format_Grayscale16) {
            log("警告：图像不是16位格式，转换为16位");
            QImage convertedImage = image.convertToFormat(QImage::Format_Grayscale16);
            if (convertedImage.isNull()) {
                log("错误：图像格式转换失败");
                return false;
            }
            return saveAsFITS(convertedImage, path);
        }
        
        // 创建FITS文件头信息（确保每行80字符）
        QStringList headerLines;
        headerLines << QString("SIMPLE  =                    T / Standard FITS format").leftJustified(80);
        headerLines << QString("BITPIX  =                   16 / 16-bit integers").leftJustified(80);
        headerLines << QString("NAXIS   =                    2 / Number of axes").leftJustified(80);
        headerLines << QString("NAXIS1  = %1 / Width of image").arg(image.width(), 8, 10, QChar(' ')).leftJustified(80);
        headerLines << QString("NAXIS2  = %1 / Height of image").arg(image.height(), 8, 10, QChar(' ')).leftJustified(80);
        headerLines << QString("BZERO   =                    0 / Offset for scaling").leftJustified(80);
        headerLines << QString("BSCALE  =                    1 / Scale factor").leftJustified(80);
        headerLines << QString("OBJECT  = 'Simulated Star Field' / Object name").leftJustified(80);
        headerLines << QString("TELESCOP= 'QUARCS Simulator' / Telescope name").leftJustified(80);
        headerLines << QString("INSTRUME= 'Star Simulator' / Instrument name").leftJustified(80);
        headerLines << QString("OBSERVER= 'QUARCS System' / Observer name").leftJustified(80);
        headerLines << QString("DATE-OBS= '%1' / Date of observation").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd")).leftJustified(80);
        headerLines << QString("EXPTIME =                  1.0 / Exposure time in seconds").leftJustified(80);
        headerLines << QString("GAIN    =                    1 / Camera gain").leftJustified(80);
        headerLines << QString("OFFSET  =                    0 / Camera offset").leftJustified(80);
        headerLines << QString("HISTORY Generated by QUARCS Star Simulator").leftJustified(80);
        headerLines << QString("END").leftJustified(80);
        
        // 计算需要的头块数（每块2880字节）
        int headerSize = headerLines.size() * 80;
        int headerBlocks = (headerSize + 2879) / 2880;
        int totalHeaderSize = headerBlocks * 2880;
        
        // 创建完整的头数据
        QByteArray headerData;
        for (const QString &line : headerLines) {
            headerData.append(line.toUtf8());
        }
        
        // 填充到完整的块
        while (headerData.size() < totalHeaderSize) {
            headerData.append(' ');
        }
        
        // 写入FITS文件
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            log(QString("错误：无法创建FITS文件: %1").arg(path));
            return false;
        }
        
        // 写入头数据
        if (file.write(headerData) != totalHeaderSize) {
            log(QString("错误：写入FITS头失败"));
            file.close();
            return false;
        }
        
        // 准备图像数据
        QByteArray imageData;
        imageData.reserve(image.width() * image.height() * 2);
        
        // 写入图像数据（大端序）
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                QRgb pixel = image.pixel(x, y);
                int gray = qGray(pixel);
                
                // 确保值在16位范围内
                gray = qBound(0, gray, 65535);
                
                // 写入16位数据（大端序）
                quint16 value = static_cast<quint16>(gray);
                imageData.append((value >> 8) & 0xFF);  // 高字节
                imageData.append(value & 0xFF);          // 低字节
            }
        }
        
        // 计算数据块数
        int dataSize = imageData.size();
        int dataBlocks = (dataSize + 2879) / 2880;
        int totalDataSize = dataBlocks * 2880;
        
        // 填充数据到完整的块
        while (imageData.size() < totalDataSize) {
            imageData.append('\0');
        }
        
        // 写入图像数据
        if (file.write(imageData) != totalDataSize) {
            log(QString("错误：写入图像数据失败"));
            file.close();
            return false;
        }
        
        file.close();
        
        // 验证文件是否成功创建
        QFileInfo savedFile(path);
        if (savedFile.exists() && savedFile.size() > 0) {
            log(QString("FITS文件已保存: %1 (大小: %2 字节, 头块: %3, 数据块: %4)")
                .arg(path).arg(savedFile.size()).arg(headerBlocks).arg(dataBlocks));
            return true;
        } else {
            log(QString("错误：FITS文件保存验证失败"));
            return false;
        }
        
    } catch (const std::exception &e) {
        log(QString("FITS保存异常: %1").arg(e.what()));
        return false;
    } catch (...) {
        log("FITS保存发生未知异常");
        return false;
    }
} 

bool StarSimulator::saveLastImageAsFITS(const QString &path)
{
    QMutexLocker locker(&m_mutex);
    
    if (m_lastGeneratedImage.isNull()) {
        log("没有可保存的图像");
        return false;
    }
    
    return saveAsFITS(m_lastGeneratedImage, path);
} 

FocusCurve StarSimulator::getFocusCurve() const
{
    return m_focusCurve;
} 

void StarSimulator::setTelescopeParams(const TelescopeParams &params)
{
    m_telescopeParams = params;
    log(QString("望远镜参数已设置: 焦距=%1mm, 口径=%2mm, 视宁度=%3arcsec, 像素=%4μm")
        .arg(params.focalLength).arg(params.aperture).arg(params.seeing).arg(params.pixelSize));
}

TelescopeParams StarSimulator::getTelescopeParams() const
{
    return m_telescopeParams;
} 