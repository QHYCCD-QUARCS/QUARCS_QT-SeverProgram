#ifndef AUTOFOCUS_H
#define AUTOFOCUS_H

#include <QObject>
#include <QTimer>
#include <QVector>
#include <QPointF>
#include <QMutex>
#include <QString>
#include <stellarsolver.h> // 包含FITSImage定义
#include "myclient.h"
#include "tools.h"
#include "starsimulator.h" // 包含星图模拟器
#include <fitsio.h>
#include <random> // 添加随机数生成器头文件

// 自动对焦状态枚举
enum class AutoFocusState {
    IDLE,                   // 空闲状态
    CHECKING_STARS,         // 检查星点
    LARGE_RANGE_SEARCH,     // 大范围找星
    COARSE_ADJUSTMENT,      // 粗调
    FINE_ADJUSTMENT,        // 精调
    COLLECTING_DATA,        // 收集数据
    FITTING_DATA,           // 拟合数据
    MOVING_TO_BEST_POSITION, // 移动到最佳位置
    COMPLETED,              // 完成
    ERROR                   // 错误状态
};

// 对焦数据点结构
struct FocusDataPoint {
    int focuserPosition;    // 电调位置
    double hfr;             // HFR值
    QVector<double> measurements; // 多次测量的HFR值（用于精调）
    
    FocusDataPoint() : focuserPosition(0), hfr(0.0) {}
    FocusDataPoint(int pos, double hfrVal) : focuserPosition(pos), hfr(hfrVal) {}
};

// 拟合结果结构
struct FitResult {
    double a, b, c;        // 二次函数 y = ax^2 + bx + c 的系数
    double bestPosition;    // 最佳位置
    double minHFR;         // 最小HFR值
    
    FitResult() : a(0.0), b(0.0), c(0.0), bestPosition(0.0), minHFR(0.0) {}
};

class AutoFocus : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param indiServer INDI客户端对象
     * @param dpFocuser 电调设备对象
     * @param dpMainCamera 主相机设备对象
     * @param parent 父对象
     */
    explicit AutoFocus(MyClient *indiServer, 
                      INDI::BaseDevice *dpFocuser, 
                      INDI::BaseDevice *dpMainCamera, 
                      QObject *parent = nullptr);
    ~AutoFocus();

    // 主要接口
    void startAutoFocus();
    void stopAutoFocus();
    bool isRunning() const;
    AutoFocusState getCurrentState() const;

    // 配置参数
    void setHFRThreshold(double threshold);
    void setCoarseStepSize(int steps);
    void setFineStepSize(int steps);
    void setCoarseShotsPerPosition(int shots);
    void setFineShotsPerPosition(int shots);
    void setMaxLargeRangeShots(int shots);
    void setInitialLargeRangeStep(double percentage);
    void setMinLargeRangeStep(double percentage);
    void setDefaultExposureTime(int exposureTime);  // 设置默认曝光时间
    
    // 拍摄状态查询
    bool isCaptureEnd() const { return m_isCaptureEnd; }
    QString getLastCapturedImage() const { return m_lastCapturedImage; }
    void setCaptureEnd(bool end) { m_isCaptureEnd = end; }
    
    // 拍摄状态控制
    void setCaptureComplete(const QString& imagePath = ""); // 设置拍摄完成
    void setCaptureFailed();                               // 设置拍摄失败
    void resetCaptureStatus();                             // 重置拍摄状态
    
    // 星点选择配置
    void setTopStarCount(int count) { m_topStarCount = count; }
    int getTopStarCount() const { return m_topStarCount; }
    
    // 电调位置限制配置
    void setFocuserMinPosition(int minPos) { m_focuserMinPosition = minPos; }
    void setFocuserMaxPosition(int maxPos) { m_focuserMaxPosition = maxPos; }
    int getFocuserMinPosition() const { return m_focuserMinPosition; }
    int getFocuserMaxPosition() const { return m_focuserMaxPosition; }
    
    // 虚拟数据控制
    void setUseVirtualData(bool useVirtual);
    bool isUsingVirtualData() const;
    void setVirtualImagePath(const QString &path);
    QString getVirtualImagePath() const;
    double getVirtualBestFocusPosition() const;
    
    // 设置虚拟图像输出路径
    void setVirtualOutputPath(const QString &path);
    QString getVirtualOutputPath() const;

signals:
    void stateChanged(AutoFocusState newState);
    void progressUpdated(int progress);
    void logMessage(const QString &message);
    void autoFocusCompleted(bool success, double bestPosition, double minHFR);
    void errorOccurred(const QString &error);
    void captureStatusChanged(bool isComplete, const QString &imagePath); // 拍摄状态变化信号
    void roiInfoChanged(const QRect &roi); // ROI信息变化信号

private slots:
    void onTimerTimeout();

private:
    // 硬件设备对象
    MyClient *m_indiServer;          // INDI客户端对象
    INDI::BaseDevice *m_dpFocuser;   // 电调设备对象
    INDI::BaseDevice *m_dpMainCamera; // 主相机设备对象
    
    // 状态管理
    AutoFocusState m_currentState;
    QTimer *m_timer;
    QMutex m_mutex;
    bool m_isRunning;

    // 配置参数
    double m_hfrThreshold;          // HFR阈值
    int m_coarseStepSize;           // 粗调步数
    int m_fineStepSize;             // 精调步数
    int m_coarseShotsPerPosition;   // 粗调每个位置拍摄次数
    int m_fineShotsPerPosition;     // 精调每个位置拍摄次数
    int m_maxLargeRangeShots;       // 大范围找星最大拍摄次数
    double m_initialLargeRangeStep; // 初始大范围步长百分比
    double m_minLargeRangeStep;     // 最小大范围步长百分比

    // 数据收集
    QVector<FocusDataPoint> m_focusData;
    FitResult m_lastFitResult;              // 最后一次拟合结果
    int m_currentLargeRangeShots;
    double m_currentLargeRangeStep;
    int m_currentPosition;
    int m_dataCollectionCount;
    int m_searchDirection;                  // 大范围找星搜索方向 (1=向外, -1=向内)
    int m_initialTargetPosition;            // 大范围找星初始目标位置
    
    // 非阻塞等待相关
    bool m_waitingForMove;                  // 是否正在等待电调移动完成
    int m_moveWaitStartTime;                // 移动等待开始时间
    int m_moveWaitCount;                    // 移动等待计数
    int m_moveLastPosition;                 // 移动等待期间的最后位置
    
    // 拍摄相关
    bool m_isCaptureEnd;                    // 拍摄是否结束
    QString m_lastCapturedImage;            // 最后拍摄的图像路径
    int m_defaultExposureTime;              // 默认曝光时间（毫秒）
    
    // 星点选择相关
    int m_topStarCount;                     // 选择置信度最高的星点数量
    
    // ROI拍摄相关
    bool m_useROI;                          // 是否使用ROI拍摄
    QRect m_currentROI;                     // 当前ROI区域
    int m_roiSize;                          // ROI大小（正方形）
    QPointF m_roiCenter;                    // ROI中心位置
    
    // 电调位置限制
    int m_focuserMinPosition;               // 电调最小位置
    int m_focuserMaxPosition;               // 电调最大位置
    
    // 虚拟数据相关
    bool m_useVirtualData;                  // 是否使用虚拟数据
    StarSimulator *m_starSimulator;         // 星图模拟器
    QString m_virtualImagePath;             // 虚拟图像路径
    int m_virtualImageCounter;              // 虚拟图像计数器
    
    // 随机数生成器（用于虚拟数据）
    std::mt19937 m_mtGenerator;             // Mersenne Twister随机数生成器
    std::uniform_real_distribution<double> m_uniformDist; // 均匀分布
    
    // 电调移动状态
    bool m_isFocuserMoving;                 // 电调是否正在移动
    int m_targetFocuserPosition;            // 目标电调位置
    int m_moveStartTime;                    // 移动开始时间
    int m_moveTimeout;                      // 移动超时时间（毫秒）
    int m_lastPosition;                     // 上次位置记录
    
    // 新增的优化参数
    bool m_devicesValid;                    // 设备有效性缓存
    qint64 m_lastDeviceCheck;               // 上次设备检查时间
    int m_retryCount;                       // 当前重试次数
    int m_maxRetryCount;                    // 最大重试次数
    int m_imageWidth;                       // 图像宽度
    int m_imageHeight;                      // 图像高度

    // 核心流程方法
    void processCurrentState();
    void processCheckingStars();                    // 处理检查星点状态
    void processFittingData();                      // 处理数据拟合状态
    void processMovingToBestPosition();             // 处理移动到最佳位置状态
    
    // 拍摄功能
    bool captureFullImage();                         // 拍摄一张全图
    bool captureROIImage();                          // 拍摄ROI图像
    bool captureImage(int exposureTime, bool useROI = false); // 拍摄图像（带曝光时间和ROI参数）
    bool waitForCaptureComplete(int timeoutMs = 30000); // 等待拍摄完成
    bool detectStarsInImage();                      // 检测图像中的星点
    double calculateHFR();                          // 计算HFR值
    
    // 星点选择方法
    double selectTopStarsAndCalculateHFR();         // 选择置信度最高的星点并计算平均HFR
    QList<FITSImage::Star> selectTopStarsByConfidence(const QList<FITSImage::Star>& stars); // 根据置信度选择最佳星点
    double calculateStarConfidence(const FITSImage::Star& star); // 计算星点置信度
    
    // 虚拟数据相关方法
    bool generateVirtualImage(int exposureTime, bool useROI = false); // 生成虚拟图像
    QString getNextVirtualImagePath();              // 获取下一个虚拟图像路径
    void initializeVirtualData();                   // 初始化虚拟数据
    
    // ROI相关方法
    void setROISize(int size);                      // 设置ROI大小
    int getROISize() const { return m_roiSize; }   // 获取ROI大小
    void updateROICenter(const QPointF& starPosition); // 根据星点位置更新ROI中心
    QRect calculateROI(const QPointF& center, int size); // 计算ROI区域
    bool isROIValid(const QRect& roi) const;       // 检查ROI是否有效
    
    // 电调控制方法
    void moveFocuser(int steps);                    // 移动电调
    int getCurrentFocuserPosition();                // 获取当前电调位置
    void setFocuserPosition(int position);          // 设置电调位置
    bool isFocuserConnected();                      // 检查电调连接状态
    bool isFocuserPositionValid(int position) const; // 检查电调位置是否有效
    int getFocuserTotalRange() const;                // 获取电调总行程
    
    // 电调移动状态查询
    bool isFocuserMoving() const { return m_isFocuserMoving; }
    int getTargetFocuserPosition() const { return m_targetFocuserPosition; }
    void setMoveTimeout(int timeout) { m_moveTimeout = timeout; }
    int getMoveTimeout() const { return m_moveTimeout; }
    void setPositionChangeTimeout(int timeout) { m_moveTimeout = timeout; } // 设置位置变化超时时间
    int getPositionChangeTimeout() const { return m_moveTimeout; } // 获取位置变化超时时间
    
    // 大范围找星流程
    void startLargeRangeSearch();
    void processLargeRangeSearch();
    void checkAndReduceStepSize();                  // 检查并减少步长
    
    // 粗调流程
    void startCoarseAdjustment();
    void processCoarseAdjustment();
    
    // 精调流程
    void startFineAdjustment();
    void processFineAdjustment();
    
    // 数据收集辅助方法
    void performCoarseDataCollection();
    void performFineDataCollection();
    
    // 数据收集和处理
    void collectFocusData();
    FitResult fitFocusData();                       // 拟合对焦数据
    void moveToBestPosition(double position);       // 移动到最佳位置
    
    // 拟合算法辅助方法
    QVector<FocusDataPoint> removeOutliers(const QVector<FocusDataPoint>& data); // 去除异常值
    bool solveLinearSystem(double matrix[3][3], double constants[3], double solution[3]); // 求解线性方程组
    FitResult findBestPositionByInterpolation();    // 插值法找最佳位置
    double calculateRSquared(const QVector<FocusDataPoint>& data, const FitResult& fit, double offset); // 计算R²
    
    // 电调移动控制
    void startFocuserMove(int targetPosition);       // 开始电调移动
    bool checkFocuserMoveComplete();                 // 检查电调移动是否完成
    void stopFocuserMove();                          // 停止电调移动
    bool waitForFocuserMoveComplete(int targetPosition, int tolerance = 5, int timeoutSeconds = 60, int stuckTimeoutSeconds = 10); // 统一等待电调移动完成
    void initializeFocuserMoveParameters();          // 初始化电调移动判断参数
    
    // 辅助方法
    void changeState(AutoFocusState newState);
    void log(const QString &message);
    void updateProgress(int progress);
    void handleError(const QString &error);
    void completeAutoFocus(bool success);
    
    // 新增的优化方法
    bool validateDevices();                                  // 验证设备有效性（带缓存）
    void executeFocuserMove(int targetPosition, const QString& moveReason); // 统一的电调移动执行
    bool isPositionReached(int currentPos, int targetPos, int tolerance = 5); // 统一的位置检查
    void handleErrorWithRetry(const QString &error, bool canRetry = false); // 带重试的错误处理
};

#endif // AUTOFOCUS_H