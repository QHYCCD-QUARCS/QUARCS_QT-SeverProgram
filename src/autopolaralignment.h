#ifndef POLARALIGNMENT_H
#define POLARALIGNMENT_H

#include <QObject>
#include <QTimer>
#include <QThread>
#include <QMutex>
#include <QQueue>
#include <QVector>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <QEventLoop>
#include <QCoreApplication>
#include <QDateTime>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "Logger.h"
#include "tools.h"
#include "myclient.h"

// 极轴校准状态枚举 - 定义校准过程中的各个状态
enum class PolarAlignmentState {
    IDLE,                   // 空闲状态 - 系统未运行校准
    INITIALIZING,           // 初始化 - 系统正在初始化校准流程
    CHECKING_POLAR_POINT,   // 检查极点位置 - 检查望远镜是否指向极点
    MOVING_DEC_AWAY,        // 移动DEC轴脱离极点 - 将DEC轴从极点位置移开
    FIRST_CAPTURE,          // 第一次拍摄 - 在第一个位置拍摄图像
    FIRST_ANALYSIS,         // 第一次分析 - 分析第一次拍摄的图像
    FIRST_RECOVERY,         // 第一次恢复 - 第一次分析失败时的恢复处理
    MOVING_RA_FIRST,        // 第一次RA轴移动 - 移动RA轴到第二个位置
    SECOND_CAPTURE,         // 第二次拍摄 - 在第二个位置拍摄图像
    SECOND_ANALYSIS,        // 第二次分析 - 分析第二次拍摄的图像
    SECOND_RECOVERY,        // 第二次恢复 - 第二次分析失败时的恢复处理
    MOVING_RA_SECOND,       // 第二次RA轴移动 - 移动RA轴到第三个位置
    THIRD_CAPTURE,          // 第三次拍摄 - 在第三个位置拍摄图像
    THIRD_ANALYSIS,         // 第三次分析 - 分析第三次拍摄的图像
    THIRD_RECOVERY,         // 第三次恢复 - 第三次分析失败时的恢复处理
    CALCULATING_DEVIATION,  // 计算偏差 - 根据三次测量计算极轴偏差
    GUIDING_ADJUSTMENT,     // 指导调整 - 指导用户调整极轴
    FINAL_VERIFICATION,     // 最终验证 - 验证调整效果
    ADJUSTING_FOR_OBSTACLE, // 调整避开遮挡物 - 向内调整避开遮挡物
    COMPLETED,              // 完成 - 校准流程完成
    FAILED,                 // 失败 - 校准流程失败
    USER_INTERVENTION       // 需要用户干预 - 需要用户手动处理
};

// 极轴校准结果结构 - 存储校准的最终结果
struct PolarAlignmentResult {
    double raDeviation;     // 赤经偏差（度）- 极轴在赤经方向的偏差角度
    double decDeviation;    // 赤纬偏差（度）- 极轴在赤纬方向的偏差角度
    double totalDeviation;  // 总偏差（度）- 极轴的总偏差角度
    bool isSuccessful;      // 是否成功 - 校准是否成功完成
    QString errorMessage;    // 错误信息 - 如果失败，记录错误原因
    QVector<SloveResults> measurements; // 测量结果 - 存储三次测量的详细数据
};

// 调整指导数据结构 - 存储adjustmentGuideData发送的信息
struct AdjustmentGuideData {
    double ra;              // 当前RA位置
    double dec;             // 当前DEC位置
    double maxRa;           // 最大RA
    double minRa;           // 最小RA
    double maxDec;          // 最大DEC
    double minDec;          // 最小DEC
    double targetRa;        // 目标RA
    double targetDec;       // 目标DEC
    double offsetRa;        // RA偏移
    double offsetDec;       // DEC偏移
    QString adjustmentRa;   // 调整指导RA
    QString adjustmentDec;  // 调整指导DEC
    QDateTime timestamp;    // 时间戳
    
    AdjustmentGuideData() : ra(0.0), dec(0.0), maxRa(0.0), minRa(0.0), 
                           maxDec(0.0), minDec(0.0), targetRa(0.0), targetDec(0.0),
                           offsetRa(0.0), offsetDec(0.0) {}
};

// 极轴校准配置结构 - 定义校准过程中的各种参数
struct PolarAlignmentConfig {
    int defaultExposureTime = 1000;    // 默认曝光时间（毫秒）- 正常拍摄的曝光时间
    int recoveryExposureTime = 5000;   // 恢复曝光时间（毫秒）- 分析失败时使用的较长曝光时间
    int shortExposureTime = 1000;      // 短曝光时间（毫秒）- 快速拍摄的曝光时间
    double raRotationAngle = 15.0;     // RA轴旋转角度（度）- 每次RA轴移动的角度
    double decRotationAngle = 10.0;    // DEC轴旋转角度（度）- DEC轴从极点移开的角度
    int maxRetryAttempts = 3;          // 最大重试次数 - 分析失败时的最大重试次数
    int captureAndAnalysisTimeout = 30000; // 拍摄和分析超时时间（毫秒）
    int movementTimeout = 15000;       // 移动超时时间（毫秒）- 望远镜移动的最大等待时间
    int maxAdjustmentAttempts = 5;     // 最大调整尝试次数 - 向内调整的最大次数
    double adjustmentAngleReduction = 0.5; // 调整角度缩减比例 - 每次调整后角度减半
    double cameraWidth = 0; // 相机传感器宽度（毫米）
    double cameraHeight = 0; // 相机传感器高度（毫米）
    int focalLength = 0; // 焦距（毫米）
    double latitude = 0.0; // 观测地点纬度（度）- 正值表示北半球，负值表示南半球
    double longitude = 0.0; // 观测地点经度（度）
    double finalVerificationThreshold = 0.5; // 最终验证精度阈值（度）
};

/**
 * @brief 自动极轴校准类
 * 
 * 该类实现了完整的自动极轴校准流程，包括：
 * - 三次不同位置的图像拍摄和分析
 * - 自动计算极轴偏差
 * - 错误恢复机制
 * - 用户指导调整
 */
class PolarAlignment : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param indiServer INDI服务器客户端
     * @param dpMount 望远镜设备指针
     * @param dpMainCamera 主相机设备指针
     * @param mainWindow 主窗口指针，用于访问拍摄状态
     * @param parent 父对象指针
     */
    explicit PolarAlignment(MyClient* indiServer, INDI::BaseDevice* dpMount, INDI::BaseDevice* dpMainCamera, QObject *parent = nullptr);
    
    /**
     * @brief 析构函数
     */
    ~PolarAlignment();

    // ==================== 主要接口函数 ====================
    
    /**
     * @brief 开始极轴校准
     * @return 是否成功启动校准流程
     */
    bool startPolarAlignment();
    
    /**
     * @brief 停止极轴校准
     * 立即停止当前校准流程，清理所有状态
     */
    void stopPolarAlignment();
    
    /**
     * @brief 用户确认调整完成
     * 在指导调整循环中调用，表示用户确认调整完成
     */
    void userConfirmAdjustment();
    
    /**
     * @brief 用户取消校准
     * 在指导调整循环中调用，表示用户取消校准
     */
    void userCancelAlignment();
    
    /**
     * @brief 暂停极轴校准
     * 暂停当前校准流程，保持当前状态
     */
    void pausePolarAlignment();
    
    /**
     * @brief 恢复极轴校准
     * 从暂停状态恢复校准流程
     */
    void resumePolarAlignment();

    /**
     * @brief 设置拍摄结束状态
     * @param isEnd 是否结束
     */
    void setCaptureEnd(bool isEnd);
    
    // ==================== 状态查询函数 ====================
    
    /**
     * @brief 获取当前校准状态
     * @return 当前的状态枚举值
     */
    PolarAlignmentState getCurrentState() const;
    
    /**
     * @brief 获取当前状态消息
     * @return 当前状态消息
     */
    QString getCurrentStatusMessage() const;

    /**
     * @brief 获取当前进度百分比
     * @return 当前进度百分比
     */
    int getProgressPercentage() const;

    /**
     * @brief 检查校准是否正在运行
     * @return 是否正在运行
     */
    bool isRunning() const;
    
    /**
     * @brief 检查校准是否已完成
     * @return 是否已完成
     */
    bool isCompleted() const;
    
    /**
     * @brief 检查校准是否失败
     * @return 是否失败
     */
    bool isFailed() const;
    
    // ==================== 结果获取函数 ====================
    
    /**
     * @brief 获取校准结果
     * @return 校准结果结构体
     */
    PolarAlignmentResult getResult() const;
    
    /**
     * @brief 获取当前状态消息
     * @return 状态描述字符串
     */
    QString getStatusMessage() const;
    
    // ==================== 配置设置函数 ====================
    
    /**
     * @brief 设置校准配置参数
     * @param config 配置结构体
     */
    void setConfig(const PolarAlignmentConfig& config);
    
    // ==================== 调整指导数据管理函数 ====================
    
    /**
     * @brief 发送满足条件的调整指导数据
     * 发送所有offsetRa和offsetDec都为0的数据，以及最后一个星点的数据
     */
    void sendValidAdjustmentGuideData();
    
    /**
     * @brief 清空调整指导数据容器
     */
    void clearAdjustmentGuideData();
    
    /**
     * @brief 获取调整指导数据容器的大小
     * @return 数据条数
     */
    int getAdjustmentGuideDataCount() const;
    
    /**
     * @brief 获取当前校准配置
     * @return 配置结构体
     */
    PolarAlignmentConfig getConfig() const;

signals:
    /**
     * @brief 状态改变信号
     * @param newState 新的状态
     * @param message 状态消息
     * @param percentage 进度百分比
     */
    void stateChanged(PolarAlignmentState newState, QString message, int percentage);
    
    // /**
    //  * @brief 状态更新信号
    //  * @param message 状态消息
    //  */
    // void statusUpdated(QString message);
    
    // /**
    //  * @brief 进度更新信号
    //  * @param percentage 进度百分比
    //  */
    // void progressUpdated(int percentage);
    
    /**
     * @brief 调整指导数据
     * @param ra 当前RA
     * @param dec 当前DEC
     * @param maxRa 最大RA
     * @param minRa 最小RA
     * @param maxDec 最大DEC
     * @param minDec 最小DEC
     * @param targetRa 目标RA
     * @param targetDec 目标DEC
     * @param adjustmentRa 调整指导RA
     * @param adjustmentDec 调整指导DEC
     */
    void adjustmentGuideData(double ra, double dec, double maxRa, double minRa, double maxDec, double minDec,double targetRa, double targetDec,double offsetRa, double offsetDec,QString adjustmentRa,QString adjustmentDec);

    /**
     * @brief 结果就绪信号
     * @param result 校准结果
     */
    void resultReady(PolarAlignmentResult result);
    
    /**
     * @brief 错误发生信号
     * @param error 错误信息
     */
    void errorOccurred(QString error);

private slots:
    /**
     * @brief 状态定时器超时处理
     * 处理状态转换和流程控制
     */
    void onStateTimerTimeout();
    
    /**
     * @brief 拍摄和分析定时器超时处理
     * 处理图像拍摄和分析超时
     */
    void onCaptureAndAnalysisTimerTimeout();
    
    /**
     * @brief 移动定时器超时处理
     * 处理望远镜移动超时
     */
    void onMovementTimerTimeout();

private:
    // ==================== 状态管理函数 ====================
    
    /**
     * @brief 设置新的校准状态
     * @param newState 新的状态
     */
    void setState(PolarAlignmentState newState);
    
    /**
     * @brief 处理当前状态
     * 根据当前状态执行相应的处理逻辑
     */
    void processCurrentState();
    
    // ==================== 主要流程函数 ====================
    
    // 极点检查结果结构体
    struct PolarPointCheckResult {
        bool success;        // 检查是否成功
        bool isNearPole;     // 是否在极点附近
        QString errorMessage; // 错误信息（如果失败）
    };
    
    /**
     * @brief 检查极点位置
     * @return 极点检查结果
     */
    PolarPointCheckResult checkPolarPoint();
    
    /**
     * @brief 移动DEC轴脱离极点
     * @return 是否成功移动
     */
    bool moveDecAxisAway();
    
    /**
     * @brief 拍摄并分析图像
     * @param attempt 尝试次数（1为正常，2为恢复）
     * @return 是否成功
     */
    bool captureAndAnalyze(int attempt = 1);
    
    /**
     * @brief 移动RA轴
     * @return 是否成功移动
     */
    bool moveRAAxis();
    
    /**
     * @brief 计算极轴偏差
     * @return 是否成功计算
     */
    bool calculateDeviation();
    
    /**
     * @brief 指导用户调整
     * @return 是否完成调整
     */
    bool guideUserAdjustment();
    
    /**
     * @brief 执行最终验证
     * @return 是否验证成功
     */
    bool performFinalVerification();
    
    /**
     * @brief 生成调整指导信息
     * @param adjustmentRa 调整指导RA
     * @param adjustmentDec 调整指导DEC
     * @return 调整指导字符串
     */
    QString generateAdjustmentGuide(QString &adjustmentRa, QString &adjustmentDec);
    
    /**
     * @brief 调整避开遮挡物
     * @return 是否调整成功
     */
    bool adjustForObstacle();
    
    /**
     * @brief 检查是否为初次拍摄失败
     * @return 是否为初次拍摄
     */
    bool isFirstCaptureFailure();
    
    /**
     * @brief 处理移动后失败
     * @return 是否处理成功
     */
    bool handlePostMovementFailure();
    
    // ==================== 设备操作函数 ====================
    
    /**
     * @brief 拍摄图像
     * @param exposureTime 曝光时间（毫秒）
     * @return 是否成功
     */
    bool captureImage(int exposureTime);
    
    /**
     * @brief 解析图像
     * @param imageFile 图像文件路径
     * @param result 解析结果
     * @return 是否成功
     */
    bool solveImage(const QString& imageFile);
    
    /**
     * @brief 移动望远镜到相对位置
     * @param ra RA偏移量（度）
     * @param dec DEC偏移量（度）
     * @return 是否成功
     */
    bool moveTelescope(double ra, double dec);

    /**
     * @brief 移动望远镜到绝对位置
     * @param ra RA位置（度）
     * @param dec DEC位置（度）
     * @return 是否成功
     */
    bool moveTelescopeToAbsolutePosition(double ra, double dec);
    
    /**
     * @brief 等待拍摄完成
     * @return 是否成功完成
     */
    bool waitForCaptureComplete();

    /**
     * @brief 获取拍摄结束状态
    
    /**
     * @brief 等待解析完成
     * @return 是否成功完成
     */
    bool waitForSolveComplete();
    
    /**
     * @brief 等待移动完成
     * @return 是否成功完成
     */
    bool waitForMovementComplete();
    
    // ==================== 辅助函数 ====================
    
    /**
     * @brief 检查分析结果是否成功
     * @param result 分析结果
     * @return 是否成功
     */
    bool isAnalysisSuccessful(const SloveResults& result);
    
    /**
     * @brief 处理分析失败
     * @param attempt 当前尝试次数
     */
    void handleAnalysisFailure(int attempt);
    
    /**
     * @brief 处理严重失败
     * 当多次尝试都失败时调用
     */
    void handleCriticalFailure();
    
    
    // ==================== 数学计算函数 ====================
    
    // 坐标转换结构体
    struct CartesianCoordinates {
        double x, y, z;
    };
    
    struct SphericalCoordinates {
        double ra, dec;
    };
    
    /**
     * @brief 将赤道坐标转换为笛卡尔坐标
     * @param ra 赤经（度）
     * @param dec 赤纬（度）
     * @param radius 半径（默认1.0）
     * @return 笛卡尔坐标
     */
    CartesianCoordinates equatorialToCartesian(double ra, double dec, double radius = 1.0);
    
    /**
     * @brief 将笛卡尔坐标转换为赤道坐标
     * @param cart 笛卡尔坐标
     * @return 赤道坐标
     */
    SphericalCoordinates cartesianToEquatorial(const CartesianCoordinates& cart);
    
    /**
     * @brief 计算向量叉积
     * @param v1 第一个向量
     * @param v2 第二个向量
     * @return 叉积结果
     */
    CartesianCoordinates crossProduct(const CartesianCoordinates& v1, const CartesianCoordinates& v2);
    
    /**
     * @brief 计算向量长度
     * @param v 向量
     * @return 向量长度
     */
    double vectorLength(const CartesianCoordinates& v);
    
    /**
     * @brief 归一化向量
     * @param v 向量
     * @return 归一化后的向量
     */
    CartesianCoordinates normalizeVector(const CartesianCoordinates& v);
    
    /**
     * @brief 计算角度差（考虑循环性）
     * @param angle1 第一个角度
     * @param angle2 第二个角度
     * @return 角度差
     */
    double calculateAngleDifference(double angle1, double angle2);
    
    /**
     * @brief 正确的三点极轴校准算法
     * @param pos1 第一个测量点
     * @param pos2 第二个测量点
     * @param pos3 第三个测量点
     * @param azimuthDeviation 方位角偏差（输出）
     * @param altitudeDeviation 高度角偏差（输出）
     * @return 是否计算成功
     */
    bool calculatePolarDeviationCorrect(const SloveResults& pos1, const SloveResults& pos2, const SloveResults& pos3,
                                       double& azimuthDeviation, double& altitudeDeviation);
    
    /**
     * @brief 计算总偏差
     * @param raDev RA偏差
     * @param decDev DEC偏差
     * @return 总偏差角度
     */
    double calculateTotalDeviation(double raDev, double decDev);
    
    // ==================== 地理位置相关函数 ====================
    
    /**
     * @brief 根据地理位置计算期望的极点位置
     * @param expectedRA 输出参数：期望的RA值
     * @param expectedDEC 输出参数：期望的DEC值
     * @return 是否成功计算
     */
    bool calculateExpectedPolarPosition(double& expectedRA, double& expectedDEC);
    
    /**
     * @brief 根据当前时间和地理位置计算精确的极点坐标
     * @param expectedRA 输出参数：期望的RA值
     * @param expectedDEC 输出参数：期望的DEC值
     * @return 是否成功计算
     */
    bool calculatePrecisePolarPosition(double& expectedRA, double& expectedDEC);
    
    /**
     * @brief 判断当前是否在北半球
     * @return 是否在北半球
     */
    bool isNorthernHemisphere() const;
    
    // ==================== 天文计算辅助函数 ====================
    
    /**
     * @brief 计算儒略日
     * @param year 年
     * @param month 月
     * @param day 日
     * @param hour 时
     * @param minute 分
     * @param second 秒
     * @return 儒略日
     */
    double calculateJulianDay(int year, int month, int day, int hour, int minute, int second);
    
    /**
     * @brief 计算格林尼治恒星时
     * @param jd 儒略日
     * @return 格林尼治恒星时（度）
     */
    double calculateGreenwichSiderealTime(double jd);
    
    // ==================== 成员变量 ====================
    
    // 设备指针
    MyClient* indiServer;           // INDI服务器客户端
    INDI::BaseDevice* dpMount;      // 望远镜设备指针
    INDI::BaseDevice* dpMainCamera; // 主相机设备指针
    
    PolarAlignmentState currentState;    // 当前校准状态
    PolarAlignmentConfig config;         // 校准配置参数
    PolarAlignmentResult result;         // 校准结果
    
    // 测量数据
    QVector<SloveResults> measurements; // 测量结果数组
    int currentMeasurementIndex;         // 当前测量索引
    int currentRetryAttempt;             // 当前重试次数
    int currentAdjustmentAttempt;        // 当前调整尝试次数
    double currentRAAngle;              // 当前RA角度
    double currentDECAngle;             // 当前DEC角度
    
    // 定时器
    QTimer stateTimer;      // 状态处理定时器
    QTimer captureAndAnalysisTimer; // 拍摄和分析超时定时器
    QTimer movementTimer;   // 移动超时定时器
    
    // 线程安全
    mutable QMutex stateMutex;  // 状态互斥锁
    mutable QMutex resultMutex; // 结果互斥锁
    
    // 状态变量
    bool isRunningFlag;         // 是否正在运行
    bool isPausedFlag;          // 是否已暂停
    bool userAdjustmentConfirmed; // 用户是否确认调整完成
    QString currentStatusMessage; // 当前状态消息
    int progressPercentage;      // 进度百分比
    

    
    // 临时数据
    QString currentImageFile;    // 当前图像文件名
    SloveResults currentAnalysisResult; // 当前分析结果
    double currentRAPosition;    // 当前RA位置
    double currentDECPosition;   // 当前DEC位置
    
    // 当前解析结果数据
    SloveResults currentSolveResult;    // 当前解析成功后的完整数据
    
    // 拍摄和解析状态
    bool isCaptureEnd;           // 拍摄是否结束
    bool isSolveEnd;             // 解析是否结束
    QString lastCapturedImage;   // 最后拍摄的图像文件
    
    // 失败计数
    int captureFailureCount;     // 拍摄失败计数
    int solveFailureCount;       // 解析失败计数

    // 调整指导数据容器
    QVector<AdjustmentGuideData> adjustmentGuideDataHistory; // 调整指导数据历史记录

    // 缓存目标位置，避免频繁重新计算
    double cachedTargetRA;        // 缓存的目标RA位置
    double cachedTargetDEC;       // 缓存的目标DEC位置
    bool isTargetPositionCached;  // 目标位置是否已缓存

    // 测试图片
    int testimage;
};

#endif // POLARALIGNMENT_H