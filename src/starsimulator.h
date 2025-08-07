#ifndef STARSIMULATOR_H
#define STARSIMULATOR_H

#include <QObject>
#include <QTimer>
#include <QVector>
#include <QPointF>
#include <QRandomGenerator>
#include <QDateTime>
#include <QMutex>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QImage>
#include <QPainter>
#include <QColor>
#include <cmath>
#include <random>
#include <algorithm>
#include <stellarsolver.h> // 包含FITSImage定义
#include <fitsio.h>

// 星点结构
struct SimulatedStar {
    QPointF position;           // 星点位置
    double brightness;          // 亮度
    double hfr;                 // HFR值
    double size;                // 星点大小
    QPointF velocity;           // 移动速度（像素/秒）
    double brightnessVariation; // 亮度变化幅度
    double phase;               // 亮度变化相位
    
    SimulatedStar() : brightness(0.0), hfr(0.0), size(0.0), 
                      brightnessVariation(0.0), phase(0.0) {}
};

// 对焦曲线参数
struct FocusCurve {
    double bestPosition;        // 最佳对焦位置
    double minHFR;             // 最小HFR值
    double curveWidth;          // 曲线宽度
    double asymmetry;           // 曲线不对称性
    
    FocusCurve() : bestPosition(0.0), minHFR(1.0), curveWidth(5000.0), asymmetry(0.0) {}
};

// 大气扰动参数
struct TurbulenceParams {
    double scale;               // 扰动尺度
    double intensity;           // 扰动强度
    double timeScale;           // 时间尺度
    
    TurbulenceParams() : scale(50.0), intensity(0.3), timeScale(0.1) {}
};

// 真实天文设备参数
struct TelescopeParams {
    double focalLength;     // 焦距 (mm)
    double aperture;        // 口径 (mm)
    double obstruction;     // 遮挡比 (0-1)
    double seeing;          // 视宁度 (arcsec)
    double pixelSize;       // 像素大小 (μm)
    double binning;         // 像素合并 (1x1, 2x2, etc.)
    double filterTransmission; // 滤镜透过率
    double exposureTime;    // 曝光时间 (s)
};

// 星图生成参数
struct StarImageParams {
    int imageWidth;             // 图像宽度
    int imageHeight;            // 图像高度
    int focuserMinPos;          // 电调最小位置
    int focuserMaxPos;          // 电调最大位置
    int focuserCurrentPos;      // 电调当前位置
    int minStarCount;           // 最小星点数量
    int maxStarCount;           // 最大星点数量
    double exposureTime;        // 曝光时间（秒）
    double noiseLevel;          // 噪声水平
    QString outputPath;         // 输出路径
    TelescopeParams telescope; // 望远镜参数
    
    StarImageParams() : imageWidth(1920), imageHeight(1080), 
                       focuserMinPos(0), focuserMaxPos(10000),
                       focuserCurrentPos(5000), minStarCount(5), maxStarCount(20),
                       exposureTime(1.0), noiseLevel(0.1) {}
};

class StarSimulator : public QObject
{
    Q_OBJECT

public:
    explicit StarSimulator(QObject *parent = nullptr);
    ~StarSimulator();

    // 主要接口：生成并保存星图
    bool generateStarImage(const StarImageParams &params);
    
    // 简化接口：根据基本参数生成星图
    bool generateStarImage(int imageWidth, int imageHeight, 
                          int focuserMinPos, int focuserMaxPos, int focuserCurrentPos,
                          const QString &outputPath);
    
    // 获取生成的星点列表
    QList<FITSImage::Star> getGeneratedStars() const;
    
    // 获取最后生成的图像
    QImage getLastGeneratedImage() const;
    
    // 保存最后生成的图像为FITS格式
    bool saveLastImageAsFITS(const QString &path);
    
    // 设置对焦曲线
    void setFocusCurve(const FocusCurve &curve);
    
    // 设置设备参数
    void setTelescopeParams(const TelescopeParams &params);
    TelescopeParams getTelescopeParams() const;
    
    // 设置噪声参数
    void setNoiseParameters(double noiseLevel, double atmosphericTurbulence);
    
    // 设置大气扰动参数
    void setTurbulenceParameters(double scale, double intensity, double timeScale);
    
    // 获取对焦曲线信息
    FocusCurve getFocusCurve() const;
    
    // 获取HFR理论值（用于验证计算）
    double getTheoreticalHFR(int focuserPosition) const;
    
    // 获取生成统计信息
    QString getGenerationStats() const;

signals:
    void imageGenerated(const QString &path);
    void logMessage(const QString &message);

private:
    // 初始化模拟器
    void initialize(const StarImageParams &params);
    
    // 生成星点数据
    void generateStars(const StarImageParams &params);
    
    // 计算HFR值
    double calculateHFR(int focuserPosition, const SimulatedStar &star);
    
    // 生成图像
    QImage generateImage(const StarImageParams &params);
    
    // 添加噪声到图像
    void addNoiseToImage(QImage &image, double noiseLevel);
    
    // 应用大气扰动
    void applyAtmosphericTurbulence(QImage &image);
    
    // 绘制星点
    void drawStar(QPainter &painter, const SimulatedStar &star, double exposureTime);
    
    // 保存图像
    bool saveImage(const QImage &image, const QString &path);
    
    // 保存为FITS格式
    bool saveAsFITS(const QImage &image, const QString &path);
    
    // 生成随机星点
    SimulatedStar generateRandomStar(int imageWidth, int imageHeight);
    
    // 在指定位置生成星点
    SimulatedStar generateStarAtPosition(const QPointF &position, int imageWidth, int imageHeight);
    
    // 生成星点位置网格
    QVector<QPointF> generateStarPositions(int imageWidth, int imageHeight, int starCount);
    
    // 检查星点是否有效
    bool isValidStar(const SimulatedStar &star, int imageWidth, int imageHeight);
    
    // 记录日志
    void log(const QString &message);

private:
    QMutex m_mutex;
    QList<SimulatedStar> m_stars;
    QList<FITSImage::Star> m_fitsStars;
    QImage m_lastGeneratedImage;
    QDateTime m_lastGenerationTime;
    
    // 对焦曲线参数
    FocusCurve m_focusCurve;
    
    // 噪声参数
    double m_noiseLevel;
    double m_atmosphericTurbulence;
    TurbulenceParams m_turbulenceParams;
    
    // 真实天文设备参数
    TelescopeParams m_telescopeParams;
    
    // 统计信息
    int m_totalStarsGenerated;
    int m_validStarsGenerated;
    
    // 随机数生成器
    QRandomGenerator *m_randomGenerator;
    std::mt19937 m_mtGenerator;
    std::uniform_real_distribution<double> m_uniformDist;
    std::normal_distribution<double> m_normalDist;
    std::poisson_distribution<int> m_poissonDist;
    std::normal_distribution<double> m_brightnessDist;
};

#endif // STARSIMULATOR_H 