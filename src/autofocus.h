#ifndef AUTOFOCUS_H
#define AUTOFOCUS_H

#include <QObject>
#include <QTimer>
#include <QVector>
#include <QPointF>
#include <QMutex>
#include <QString>
#include <QSettings>
#include <QCoreApplication>
#include <stellarsolver.h> // 包含FITSImage定义
#include "myclient.h"
#include "tools.h"
#include "starsimulator.h" // 包含星图模拟器
#include <fitsio.h>
#include <random> // 添加随机数生成器头文件
#include "websocketthread.h"

// 自动对焦状态枚举
enum class AutoFocusState {
    IDLE,                   // 空闲状态
    CHECKING_STARS,         // 检查星点
    LARGE_RANGE_SEARCH,     // 大范围找星
    COARSE_ADJUSTMENT,      // 粗调
    FINE_ADJUSTMENT,        // 精调
    SUPER_FINE_ADJUSTMENT,  // 更细致精调（基于HFR拟合）
    COLLECTING_DATA,        // 收集数据
    FITTING_DATA,           // 拟合数据
    MOVING_TO_BEST_POSITION, // 移动到最佳位置
    COMPLETED,              // 完成
    ERROR                   // 错误状态
};

// 对焦数据点结构
struct FocusDataPoint {
    int focuserPosition;    // 电调位置
    double hfr;             // 这里存HFR值
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

// 空程校准结果结构
struct BacklashCalibrationResult {
    int inwardBacklash;     // 向内空程（步数）
    int outwardBacklash;    // 向外空程（步数）
    bool isValid;           // 校准结果是否有效
    QString errorMessage;   // 错误信息
    
    BacklashCalibrationResult() : inwardBacklash(0), outwardBacklash(0), isValid(false) {}
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
                      WebSocketThread *wsThread,
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
    
    // 获取当前测试图片路径
    QString getCurrentTestImagePath() const;
    
    // 空程校准相关接口
    void startBacklashCalibration();                    // 开始空程校准
    void stopBacklashCalibration();                     // 停止空程校准
    bool isBacklashCalibrationRunning() const;          // 检查空程校准是否正在运行
    BacklashCalibrationResult getBacklashCalibrationResult() const; // 获取空程校准结果
    void setUseBacklashCompensation(bool use);          // 设置是否使用空程补偿
    bool isUsingBacklashCompensation() const;           // 检查是否使用空程补偿
    void setBacklashCompensation(int inward, int outward); // 设置空程补偿值

    // 仅从当前位置启动 super-fine 精调（跳过粗调/精调的完整流程）
    void startSuperFineFromCurrentPosition();

    void getAutoFocusStep(); // 获取自动对焦步骤信号 - [AUTO_FOCUS_UI_ENHANCEMENT]
    void getAutoFocusData(); // 获取自动对焦数据信号 - [AUTO_FOCUS_UI_ENHANCEMENT]

signals:
    void stateChanged(AutoFocusState newState);
    void progressUpdated(int progress);
    void logMessage(const QString &message);
    void autoFocusCompleted(bool success, double bestPosition, double minHFR);
    void errorOccurred(const QString &error);
    void captureStatusChanged(bool isComplete, const QString &imagePath); // 拍摄状态变化信号
    void roiInfoChanged(const QRect &roi); // ROI信息变化信号
    void focusSeriesReset(const QString &stage);                 // 重置当前序列（"coarse"/"fine"）
    void focusDataPointReady(int position, double hfr, const QString &stage); // 新的数据点
    void focusFitUpdated(double a, double b, double c, double bestPosition, double minHFR); // 二次拟合结果
    void startPositionUpdateTimer(); // 启动位置更新定时器
    void focusBestMovePlanned(int bestPosition, const QString &stage); // 计划移动到的最佳位置
    void autofocusFailed(); // 自动对焦失败信号
    void starDetectionResult(bool detected, double hfr); // 星点识别结果信号
    void autoFocusModeChanged(const QString &mode, double hfr); // 自动对焦模式变化信号
    void focuserPositionChanged(int currentPosition); // 电调位置变化信号**xiugai
    void autoFocusStepChanged(int step, const QString &stepDescription); // 自动对焦步骤变化信号 - [AUTO_FOCUS_UI_ENHANCEMENT]


private slots:
    void onTimerTimeout();
    void onMoveCheckTimerTimeout();         // 电调移动检查定时器回调
    void onCaptureCheckTimerTimeout();      // 拍摄完成检查定时器回调
    void forceStopAllWaiting();             // 强制停止所有等待状态

private:
    // 公共初始化逻辑：设备检查 + 成员状态重置 + 行程范围与当前位置读取
    bool initializeAutoFocusCommon();
    // 硬件设备对象
    MyClient *m_indiServer;          // INDI客户端对象
    INDI::BaseDevice *m_dpFocuser;   // 电调设备对象
    INDI::BaseDevice *m_dpMainCamera; // 主相机设备对象
    WebSocketThread *m_wsThread;// 网络线程
    // 状态管理
    AutoFocusState m_currentState;
    QTimer *m_timer;
    QTimer *m_moveCheckTimer;         // 电调移动检查定时器
    QTimer *m_captureCheckTimer;     // 拍摄完成检查定时器
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
    QVector<FocusDataPoint> m_focusData;    // 所有数据（粗调+精调）
    QVector<FocusDataPoint> m_fineFocusData; // 仅精调数据
    FitResult m_lastFitResult;              // 最后一次拟合结果
    double m_lastHFR;                     // 最近一次由Python得到的HFR
    // 粗调/精调阶段基于 SNR 的最佳位置记录
    double m_coarseBestSNR;               // 粗调阶段最佳 SNR（mean_peak_snr）
    double m_fineBestSNR;                 // 精调阶段最佳 SNR（mean_peak_snr）
    // 扫描序列
    QVector<int> m_coarseScanPositions;    // 粗调扫描位置序列
    int m_coarseScanIndex;                 // 粗调扫描索引
    QVector<int> m_fineScanPositions;      // 精调扫描位置序列
    int m_fineScanIndex;                   // 精调扫描索引
    int m_coarseStepSpan;                  // 粗调步进（= (max-min)/10）
    int m_fineStepSpan;                    // 精调步进（= 粗调步进/10）
    int m_coarseBestPosition;              // 粗调期望位置
    double m_coarseBestHFR;
    int m_fineBestPosition;                // 精调阶段 SNR 最佳位置（super-fine 中心）
    bool m_coarseHasValidSNR;              // 粗调阶段是否存在至少一个 SNR>0 的位置
    
// === 精调方向与反转逻辑（新增） ===
int  m_fineDirection;       // +1: 向大的方向；-1: 向小的方向
int  m_fineIncreaseCount;   // 连续"HFR变大"的计数
bool m_fineReversed;        // 是否已经发生过一次改向
int  m_fineCenter;          // 精调中心（粗调最优位置）
               // 粗调最小HFR

    // 更细致精调（super-fine）扫描数据
    QVector<int> m_superFineScanPositions;   // super-fine 扫描位置序列
    int m_superFineScanIndex;                // super-fine 扫描索引
    int m_superFineStepSpan;                 // super-fine 步进
    QVector<FocusDataPoint> m_superFineFocusData; // 仅 super-fine 数据（用于最终拟合）

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
    bool m_moveCheckResult;                 // 移动检查结果
    bool m_moveCheckPending;                // 是否正在等待移动检查完成
    int m_moveCheckTimeout;                 // 移动检查超时时间（秒）
    int m_moveCheckStuckTimeout;            // 移动检查卡住超时时间（秒）
    int m_moveCheckTargetPosition;         // 移动检查目标位置
    int m_moveCheckTolerance;               // 移动检查容差
    
    // 拍摄相关
    bool m_isCaptureEnd;                    // 拍摄是否结束
    QString m_lastCapturedImage;            // 最后拍摄的图像路径
    int m_defaultExposureTime;              // 默认曝光时间（毫秒）
    bool m_captureCheckPending;             // 是否正在等待拍摄检查完成
    bool m_captureCheckResult;              // 拍摄检查结果
    int m_captureCheckTimeout;              // 拍摄检查超时时间（毫秒）
    
    // 星点选择相关
    int m_topStarCount;                     // 选择置信度最高的星点数量
    
    // 测试文件循环相关
    int m_testFileCounter;                  // 测试文件计数器 (1-11)
    
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
    bool m_hasLastPosition;                 // 是否已有上次位置记录的标志
    
    // 新增的优化参数
    bool m_devicesValid;                    // 设备有效性缓存
    qint64 m_lastDeviceCheck;               // 上次设备检查时间
    int m_retryCount;                       // 当前重试次数
    int m_maxRetryCount;                    // 最大重试次数
    int m_imageWidth;                       // 图像宽度
    int m_imageHeight;                      // 图像高度
    
    // 空程补偿相关
    bool m_useBacklashCompensation;         // 是否使用空程补偿
    int m_backlashCompensation;             // 空程补偿值（步数）

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
    bool detectHFRByPython(double &hfr);            // 通过旧的 Python 脚本识星并返回HFR
    bool detectMedianHFRByPython(double &hfr);      // 通过 calculatestars.py 计算 median_HFR（super-fine 使用）
    bool detectSNRByPython(double &snr);            // 通过Python脚本计算 avg_top50_snr（粗调/精调）
    bool loadFocuserRangeFromIni(const QString &iniPath = QString()); // 从ini读取电调范围

    
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
    bool beginMoveTo(int targetPosition, const QString& reason);
    bool captureAndDetectOnce();
    void checkAndReduceStepSize();                  // 检查并减少步长
    
    // 粗调流程
    void startCoarseAdjustment();
    void processCoarseAdjustment();
    
    // 精调流程
    void startFineAdjustment();
    void processFineAdjustment();
    
    // 更细致精调流程
    void startSuperFineAdjustment();
    void processSuperFineAdjustment();
    
    // 数据收集辅助方法
    void performCoarseDataCollection();
    void performFineDataCollection();
    
    // 数据收集和处理
    void collectFocusData();
    FitResult fitFocusData();                       // 拟合对焦数据
    FitResult findBestPositionByInterpolation(const QVector<FocusDataPoint>& data); // 基于给定数据的插值法
    void moveToBestPosition(double position);       // 移动到最佳位置
    
    // 拟合算法辅助方法
    QVector<FocusDataPoint> removeOutliers(const QVector<FocusDataPoint>& data); // 去除异常值
    QVector<FocusDataPoint> removeOutliersByResidual(const QVector<FocusDataPoint>& data); // 基于残差的异常值检测
    QVector<FocusDataPoint> removeOutliersByIQR(const QVector<FocusDataPoint>& data); // 基于IQR的异常值检测
    QVector<FocusDataPoint> removeOutliersByPosition(const QVector<FocusDataPoint>& data); // 基于位置的异常值检测
    bool solveLinearSystem(double matrix[3][3], double constants[3], double solution[3]); // 求解线性方程组
    FitResult findBestPositionByInterpolation();    // 插值法找最佳位置
    double calculateRSquared(const QVector<FocusDataPoint>& data, const FitResult& fit, double offset); // 计算R²
    
    // 改进的拟合方法
    FitResult performStandardLeastSquares(const QVector<FocusDataPoint>& data); // 标准最小二乘法
    FitResult performWeightedLeastSquares(const QVector<FocusDataPoint>& data); // 加权最小二乘法
    FitResult performSimplifiedQuadraticFit(const QVector<FocusDataPoint>& data); // 简化二次拟合
    FitResult performRobustFitting(const QVector<FocusDataPoint>& data); // 鲁棒拟合
    double getDataMinPosition(const QVector<FocusDataPoint>& data); // 获取数据最小位置
    double getDataMaxPosition(const QVector<FocusDataPoint>& data); // 获取数据最大位置
    
    // 最终验证方法
    void performFinalFocusVerification(); // 最终对焦验证
    
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
    void updateAutoFocusStep(int step, const QString &description); // 更新自动对焦步骤状态 - [AUTO_FOCUS_UI_ENHANCEMENT]
    
    // 新增的优化方法
    bool validateDevices();                                  // 验证设备有效性（带缓存）
    void executeFocuserMove(int targetPosition, const QString& moveReason); // 统一的电调移动执行
    bool isPositionReached(int currentPos, int targetPos, int tolerance = 5); // 统一的位置检查
    void handleErrorWithRetry(const QString &error, bool canRetry = false); // 带重试的错误处理
};

#endif // AUTOFOCUS_H