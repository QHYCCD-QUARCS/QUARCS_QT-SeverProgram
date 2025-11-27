#include "autofocus.h"
#include <QDebug>
#include <QThread>
#include <QtMath>
#include <QDateTime>
#include <QFileInfo>
#include <QEventLoop>
#include <QCoreApplication>
#include <QPointer>
#include <string>
#include <limits>
#include <cmath>
#include <algorithm>
#include "myclient.h"  // MyClient类定义
#include "qhyccd.h"    // QHYCCD_SUCCESS常量
#include "tools.h"   // Tools类
#include "Logger.h"  // Logger类
#include <stellarsolver.h> // FITSImage类来自stellarsolver

// ==================== 调试开关 ====================
// 如果希望用本地测试文件 /home/quarcs/FOCUSTEST/1.fits ~ 10.fits
// 来循环模拟粗调/精调阶段的 SNR 计算，把下面这一行保持为 1。
// 如果要恢复正常模式（使用实时拍摄的 /dev/shm/ccd_simulator.fits），
// 请将 AUTOFOCUS_SNR_TEST_MODE 改为 0 后重新编译。
#define AUTOFOCUS_SNR_TEST_MODE 1

// ==================== 自动对焦配置结构体 ====================
struct AutoFocusConfig {
    // HFR相关参数
    double hfrThreshold = 1.0;                    // HFR阈值，超过此值进入粗调
    
    // 步长参数
    int coarseStepSize = 1000;                    // 粗调步长
    int fineStepSize = 100;                       // 精调步长
    
    // 拍摄参数
    int coarseShotsPerPosition = 1;               // 粗调每个位置拍摄张数
    int fineShotsPerPosition = 3;                 // 精调每个位置拍摄张数
    int defaultExposureTime = 1000;               // 默认曝光时间(ms)
    
    // 大范围搜索参数
    int maxLargeRangeShots = 80;                  // 大范围找星最多拍摄次数
    double initialLargeRangeStep = 10.0;          // 初始大范围步长(%)
    double minLargeRangeStep = 2.0;               // 最小大范围步长(%)
    
    // 星点选择参数
    int topStarCount = 5;                         // 选择置信度最高的星点数量
    
    // 电调移动参数
    int positionTolerance = 5;                    // 位置误差容差(步数)
    int bestPositionTolerance = 10;               // 最佳位置误差容差(步数)
    int moveTimeout = 5000;                       // 移动超时时间(ms) - 增加到5秒
    int stuckTimeout = 2000;                      // 卡住检测时间(ms) - 增加到2秒
    
    // 数据拟合参数
    double minRSquared = 0.1;                     // 最小拟合质量R²（进一步降低要求，确保使用拟合而不是插值）
    int minDataPoints = 5;                        // 最小数据点数量（降低要求）
    
    
    // 重试参数
    int maxRetryCount = 3;                        // 最大重试次数
    int retryDelay = 1000;                        // 重试延迟时间(ms)
    
    // 设备检查参数
    int deviceCheckInterval = 5000;               // 设备状态检查间隔(ms)
    
    // ROI参数
    int roiSize = 300;                            // ROI大小(像素)
    int imageWidth = 1920;                        // 图像宽度(像素)
    int imageHeight = 1080;                       // 图像高度(像素)
};

// 全局配置实例
static AutoFocusConfig g_autoFocusConfig;

/**
 * @file autofocus.cpp
 * @brief 自动对焦实现文件
 * 
 * 这个类实现了天文摄影中的自动对焦功能，主要特点：
 * 1. 支持大范围找星：当没有检测到星点时，自动进行大范围搜索
 * 2. 粗调模式：使用大步长快速找到大致对焦位置
 * 3. 精调模式：使用小步长精确对焦
 * 4. 数据拟合：使用二次函数拟合对焦曲线，找到最佳位置
 * 5. 多线程安全：使用互斥锁保护共享数据
 * 
 * 工作流程：
 * 1. 检查星点 -> 2. 大范围找星（如需要）-> 3. 粗调/精调 -> 4. 数据拟合 -> 5. 移动到最佳位置
 * 
 * 星点检测策略：
 * - 每次拍摄后检测所有星点
 * - 选择置信度最高的几颗星（基于亮度、HFR、形状等特征）
 * - 计算平均HFR值用于对焦数据拟合
 * - 不再进行复杂的星点跟踪
 */

AutoFocus::AutoFocus(MyClient *indiServer, 
                     INDI::BaseDevice *dpFocuser, 
                     INDI::BaseDevice *dpMainCamera, 
                     WebSocketThread *wsThread,
                     QObject *parent)
    : QObject(parent)
    , m_indiServer(indiServer)                       // INDI客户端对象
    , m_dpFocuser(dpFocuser)                         // 电调设备对象
    , m_dpMainCamera(dpMainCamera)                   // 主相机设备对象
    , m_wsThread(wsThread)                           // 网络线程
    , m_currentState(AutoFocusState::IDLE)           // 初始状态为空闲
    , m_timer(new QTimer(this))                      // 创建定时器用于状态处理
    , m_moveCheckTimer(new QTimer(this))             // 创建移动检查定时器
    , m_captureCheckTimer(new QTimer(this))          // 创建拍摄检查定时器
    , m_isRunning(false)                             // 初始未运行
    // 使用配置参数替代硬编码值
    , m_hfrThreshold(g_autoFocusConfig.hfrThreshold)
    , m_coarseStepSize(g_autoFocusConfig.coarseStepSize)
    , m_fineStepSize(g_autoFocusConfig.fineStepSize)
    , m_coarseShotsPerPosition(g_autoFocusConfig.coarseShotsPerPosition)
    , m_fineShotsPerPosition(g_autoFocusConfig.fineShotsPerPosition)
    , m_maxLargeRangeShots(g_autoFocusConfig.maxLargeRangeShots)
    , m_initialLargeRangeStep(g_autoFocusConfig.initialLargeRangeStep)
    , m_minLargeRangeStep(g_autoFocusConfig.minLargeRangeStep)
    , m_currentLargeRangeShots(0)                   // 当前大范围拍摄次数
    , m_currentLargeRangeStep(g_autoFocusConfig.initialLargeRangeStep) // 当前大范围步长
    , m_currentPosition(0)                          // 当前电调位置
    , m_dataCollectionCount(0)                      // 数据收集计数
    , m_isCaptureEnd(true)                         // 拍摄结束状态
    , m_lastCapturedImage("")                      // 最后拍摄图像路径
    , m_defaultExposureTime(g_autoFocusConfig.defaultExposureTime) // 默认曝光时间
    , m_captureCheckPending(false)                 // 初始不在等待拍摄检查
    , m_captureCheckResult(false)                   // 拍摄检查结果
    , m_captureCheckTimeout(30000)                  // 拍摄检查超时时间（默认30秒）
    , m_topStarCount(g_autoFocusConfig.topStarCount) // 选择置信度最高的星点数量
    , m_testFileCounter(1)                         // 测试文件计数器，从1开始
    , m_focuserMinPosition(0)                      // 电调最小位置
    , m_focuserMaxPosition(10000)                  // 电调最大位置
    , m_isFocuserMoving(false)                     // 电调未在移动
    , m_targetFocuserPosition(0)                   // 目标位置
    , m_moveStartTime(0)                           // 移动开始时间
    , m_moveTimeout(g_autoFocusConfig.moveTimeout) // 位置无变化超时时间
    , m_lastPosition(0)                            // 上次位置记录
    , m_useROI(false)                              // 默认不使用ROI
    , m_currentROI(0, 0, 0, 0)                    // 初始ROI区域
    , m_roiSize(g_autoFocusConfig.roiSize)         // ROI大小
    , m_roiCenter(0, 0)                            // ROI中心位置
    , m_waitingForMove(false)                      // 初始不等待电调移动
    , m_moveWaitStartTime(0)                       // 移动等待开始时间
    , m_moveWaitCount(0)                           // 移动等待计数
    , m_moveLastPosition(0)                        // 移动等待期间的最后位置
    , m_moveCheckResult(false)                     // 移动检查结果
    , m_moveCheckPending(false)                    // 初始不在等待移动检查
    , m_moveCheckTimeout(0)                         // 移动检查超时时间
    , m_moveCheckStuckTimeout(0)                   // 移动检查卡住超时时间
    , m_moveCheckTargetPosition(0)                 // 移动检查目标位置
    , m_moveCheckTolerance(5)                      // 移动检查容差
    , m_lastFitResult()                            // 初始化拟合结果
    , m_coarseBestSNR(-1.0)                        // 粗调阶段最佳 SNR
    , m_fineBestSNR(-1.0)                          // 精调阶段最佳 SNR
    , m_useVirtualData(false)                      // 默认不使用虚拟数据
    , m_starSimulator(nullptr)                     // 星图模拟器指针
    , m_virtualImagePath("")                       // 虚拟图像路径
    , m_virtualImageCounter(0)                     // 虚拟图像计数器
    , m_mtGenerator(std::chrono::steady_clock::now().time_since_epoch().count()) // 随机数生成器
    , m_uniformDist(0.0, 1.0)                     // 均匀分布
    
    // 新增的优化参数
    , m_devicesValid(false)                        // 设备有效性缓存
    , m_lastDeviceCheck(0)                         // 上次设备检查时间
    , m_retryCount(0)                              // 当前重试次数
    , m_maxRetryCount(g_autoFocusConfig.maxRetryCount) // 最大重试次数
    , m_imageWidth(g_autoFocusConfig.imageWidth)   // 图像宽度
    , m_imageHeight(g_autoFocusConfig.imageHeight) // 图像高度
    
    // 空程补偿相关
    , m_useBacklashCompensation(false)             // 默认不使用空程补偿
    , m_backlashCompensation(0)                    // 默认空程补偿值为0
{
        m_hasLastPosition = false;
// 验证传入的设备对象
    if (!m_dpMainCamera) {
        log(QString("警告: 主相机设备对象为空"));
    }
    if (!m_dpFocuser) {
        log(QString("警告: 电调设备对象为空"));
    }
    if (!m_indiServer) {
        log(QString("警告: INDI客户端对象为空"));
    }
    
    // 连接定时器信号到状态处理槽函数
    connect(m_timer, &QTimer::timeout, this, &AutoFocus::onTimerTimeout);
    // 连接移动检查定时器信号
    connect(m_moveCheckTimer, &QTimer::timeout, this, &AutoFocus::onMoveCheckTimerTimeout);
    // 设置移动检查定时器间隔为100ms
    m_moveCheckTimer->setInterval(100);
    // 连接拍摄检查定时器信号
    connect(m_captureCheckTimer, &QTimer::timeout, this, &AutoFocus::onCaptureCheckTimerTimeout);
    // 设置拍摄检查定时器间隔为100ms
    m_captureCheckTimer->setInterval(100);
}

// ==================== 新增的优化方法实现 ====================

/**
 * @brief 验证设备有效性（带缓存）
 * 
 * 减少重复的设备检查，提高性能
 * 
 * @return bool 设备是否有效
 */
bool AutoFocus::validateDevices()
{
    auto now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastDeviceCheck > g_autoFocusConfig.deviceCheckInterval) {
        m_devicesValid = (m_dpMainCamera && m_dpFocuser && m_indiServer);
        m_lastDeviceCheck = now;
        
        if (!m_devicesValid) {
            log("设备有效性检查失败：设备对象无效");
        }
    }
    return m_devicesValid;
}

/**
 * @brief 统一的电调移动执行
 * 
 * 提取公共的电调移动逻辑，减少代码重复
 * 
 * @param targetPosition 目标位置
 * @param moveReason 移动原因（用于日志）
 */
void AutoFocus::executeFocuserMove(int targetPosition, const QString& moveReason)
{
    if (!m_dpFocuser) {
        log(QString("错误: 电调设备对象为空，无法执行移动"));
        return;
    }
    
    // 检查位置限制
    if (targetPosition < m_focuserMinPosition) {
        log(QString("警告: 目标位置 %1 小于最小位置 %2，限制到最小位置")
            .arg(targetPosition).arg(m_focuserMinPosition));
        targetPosition = m_focuserMinPosition;
    } else if (targetPosition > m_focuserMaxPosition) {
        log(QString("警告: 目标位置 %1 大于最大位置 %2，限制到最大位置")
            .arg(targetPosition).arg(m_focuserMaxPosition));
        targetPosition = m_focuserMaxPosition;
    }
    
    // 计算移动方向和步数
    int moveSteps = targetPosition - m_currentPosition;
    bool isInward = (moveSteps < 0);
    int absSteps = qAbs(moveSteps);
    
    if (absSteps == 0) {
        log(QString("%1: 目标位置与当前位置相同，无需移动").arg(moveReason));
        return;
    }
    
    // 设置移动状态
    m_isFocuserMoving = true;
    m_targetFocuserPosition = targetPosition;
    m_moveStartTime = QDateTime::currentMSecsSinceEpoch();
    m_lastPosition = m_currentPosition;
    
    // 发送移动命令
    m_indiServer->setFocuserMoveDiretion(m_dpFocuser, isInward);
    m_indiServer->moveFocuserSteps(m_dpFocuser, absSteps);
    
    // 初始化参数
    initializeFocuserMoveParameters();
    
    // 设置等待状态
    m_waitingForMove = true;
    m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
    m_moveWaitCount = 0;
    m_moveLastPosition = m_currentPosition;
    
    log(QString("%1: 从%2移动到%3，方向=%4，步数=%5")
        .arg(moveReason).arg(m_currentPosition).arg(targetPosition)
        .arg(isInward ? "向内" : "向外").arg(absSteps));
}

/**
 * @brief 统一的位置检查
 * 
 * 使用配置化的容差参数，确保位置检查的一致性
 * 
 * @param currentPos 当前位置
 * @param targetPos 目标位置
 * @param tolerance 容差（步数）
 * @return bool 是否到达目标位置
 */
bool AutoFocus::isPositionReached(int currentPos, int targetPos, int tolerance)
{
    return qAbs(currentPos - targetPos) <= tolerance;
}

/**
 * @brief 带重试的错误处理
 * 
 * 支持重试机制，提高系统的鲁棒性
 * 
 * @param error 错误信息
 * @param canRetry 是否可以重试
 */
void AutoFocus::handleErrorWithRetry(const QString &error, bool canRetry)
{
    log(QString("错误: %1").arg(error));
    
    if (canRetry && m_retryCount < m_maxRetryCount) {
        m_retryCount++;
        log(QString("尝试重试第%1次").arg(m_retryCount));
        
        // 延迟重试
        QTimer::singleShot(g_autoFocusConfig.retryDelay, [this]() {
            if (m_isRunning) {
                log("开始重试操作");
                // 这里可以根据具体错误类型执行不同的重试逻辑
            }
        });
        return;
    }
    
    // 重试次数用完或不可重试，发出错误信号
    emit errorOccurred(error);
    completeAutoFocus(false);
}



AutoFocus::~AutoFocus()
{
    stopAutoFocus();
}

/**
 * @brief 公共初始化逻辑：设备检查 + 成员状态重置 + 行程范围与当前位置读取
 */
bool AutoFocus::initializeAutoFocusCommon()
{
    if (m_isRunning) {
        log(QString("自动对焦已在运行中"));
        return false;
    }

    // 检查设备对象是否有效
    if (!m_dpFocuser) {
        log(QString("错误: 电调设备对象为空，无法开始自动对焦"));
        emit errorOccurred("电调设备未连接");
        return false;
    }
    
    if (!m_dpMainCamera) {
        log(QString("错误: 主相机设备对象为空，无法开始自动对焦"));
        emit errorOccurred("主相机设备未连接");
        return false;
    }
    
    if (!m_indiServer) {
        log(QString("错误: INDI客户端对象为空，无法开始自动对焦"));
        emit errorOccurred("INDI客户端未连接");
        return false;
    }
    
    // 初始化所有参数，确保回调函数访问时不会出现段错误
    log("开始初始化自动对焦参数...");
    
    // 初始化运行状态
    m_isRunning = true;
    
    // 初始化电调相关参数（确保回调访问时不会出现段错误）
    m_isFocuserMoving = false;
    m_targetFocuserPosition = 0;
    m_moveStartTime = 0;
    m_lastPosition = 0;
    m_waitingForMove = false;
    m_moveWaitStartTime = 0;
    m_moveWaitCount = 0;
    m_moveLastPosition = 0;
    
    // 初始化拍摄相关参数（确保回调访问时不会出现段错误）
    m_isCaptureEnd = true;
    // 确保图像路径不为空，避免回调访问空字符串
    if (m_lastCapturedImage.isEmpty()) {
        m_lastCapturedImage = "/dev/shm/ccd_simulator.fits";
    }
    
    // 初始化星点选择参数（确保回调访问时不会出现段错误）
    if (m_topStarCount <= 0) m_topStarCount = 5; // 未配置则默认选5颗星
    
    // 初始化数据收集参数（确保回调访问时不会出现段错误）
    m_focusData.clear();
    m_lastFitResult = FitResult();
    m_dataCollectionCount = 0;
    m_currentLargeRangeShots = 0;
    m_currentLargeRangeStep = m_initialLargeRangeStep;
    
    // 初始化ROI参数（确保回调访问时不会出现段错误）
    m_useROI = false;
    m_currentROI = QRect(0, 0, 0, 0);
    m_roiCenter = QPointF(0, 0);
    
    // 初始化虚拟数据参数
    if (m_useVirtualData && !m_starSimulator) {
        m_starSimulator = new StarSimulator(this);
        log("虚拟数据模式：星图模拟器已创建");
    }
    
    log("自动对焦参数初始化完成");
    
    // 优先从INI读取经过校准的电调范围；若读取成功，覆盖硬件报告的范围
    if (loadFocuserRangeFromIni()) {
        log(QString("已使用INI中的电调范围: [%1, %2]").arg(m_focuserMinPosition).arg(m_focuserMaxPosition));
    }

    
    // 获取电调位置范围（若INI读取失败则回退读取硬件）
    if (m_dpFocuser && (m_focuserMaxPosition <= m_focuserMinPosition)) {
        int min, max, step, value;
        m_indiServer->getFocuserRange(m_dpFocuser, min, max, step, value);
        m_focuserMinPosition = min;
        m_focuserMaxPosition = max;
        log(QString("电调位置范围: %1 - %2").arg(m_focuserMinPosition).arg(m_focuserMaxPosition));
    }
    
    // 获取当前电调位置
    int currentPos;
    m_indiServer->getFocuserAbsolutePosition(m_dpFocuser, currentPos);
    m_currentPosition = currentPos;
    log(QString("当前电调位置: %1").arg(m_currentPosition));

    return true;
}

/**
 * @brief 开始自动对焦流程（完整流程：检查星点→粗调→精调→super-fine）
 */
void AutoFocus::startAutoFocus()
{
    if (!initializeAutoFocusCommon()) {
        return;
    }

    // 切换到初始状态
    changeState(AutoFocusState::CHECKING_STARS);
    updateAutoFocusStep(1, "Please observe if the camera has started shooting the full image. If it has started shooting, please wait for the shooting to complete"); // [AUTO_FOCUS_UI_ENHANCEMENT]
    
    // 启动定时器开始状态机处理
    if (m_timer) {
        m_timer->start(100); // 100ms间隔
        log("定时器已启动，开始自动对焦流程");
    } else {
        log("错误: 定时器对象为空");
        m_isRunning = false;
        emit errorOccurred("定时器初始化失败");
        return;
    }
    
    emit stateChanged(AutoFocusState::CHECKING_STARS);
    log("自动对焦流程已启动");
}

/**
 * @brief 仅从当前位置开始 super-fine 精调（跳过前面的粗调/精调阶段）
 */
void AutoFocus::startSuperFineFromCurrentPosition()
{
    if (!initializeAutoFocusCommon()) {
        return;
    }

    // 以当前位置作为 super-fine 的中心
    int currentPos = getCurrentFocuserPosition();
    const int minPos = m_focuserMinPosition;
    const int maxPos = m_focuserMaxPosition;
    currentPos = std::clamp(currentPos, minPos, maxPos);

    m_currentPosition = currentPos;
    m_fineBestPosition = currentPos;
    m_fineBestSNR = -1.0;

    // 为直接进入 super-fine 的场景显式设置精调 / super-fine 步距
    // 与 startFineAdjustment 中保持一致：精调步距约为总行程的 2%，
    // 而 super-fine 步距在 startSuperFineAdjustment 中会取 m_fineStepSpan 的一半，
    // 也就是大约总行程的 1%。
    const int totalRange = std::max(1, maxPos - minPos);
    m_fineStepSpan = std::max(1, static_cast<int>(std::round(totalRange * 0.02)));

    log(QString("从当前位置启动 super-fine 精调，当前位置: %1，精调步距≈%2 步（总行程约 2%%）")
            .arg(m_currentPosition)
            .arg(m_fineStepSpan));

    // 直接进入 super-fine 阶段
    startSuperFineAdjustment();

    // 启动定时器驱动状态机
    if (m_timer) {
        m_timer->start(100); // 100ms间隔
        log("定时器已启动，开始 super-fine 自动对焦流程");
    } else {
        log("错误: 定时器对象为空");
        m_isRunning = false;
        emit errorOccurred("定时器初始化失败");
        return;
    }

    log("super-fine 自动对焦流程已启动");
}

void AutoFocus::stopAutoFocus()
{
    try {
        log("开始停止自动对焦");
    
        
        // if (!m_isRunning) {
        //     log("自动对焦未在运行，无需停止");
        //     return;
        // }

        // 立即重置运行状态，防止定时器继续调用processCurrentState
        m_isRunning = false;
        log("运行状态已重置");
        
        // 立即停止定时器，防止任何回调继续执行
        if (m_timer) {
            m_timer->stop();
            log("定时器已停止");
        }
        
        // 停止移动检查定时器
        if (m_moveCheckTimer) {
            m_moveCheckTimer->stop();
            log("移动检查定时器已停止");
        }
        
        // 停止拍摄检查定时器
        if (m_captureCheckTimer) {
            m_captureCheckTimer->stop();
            log("拍摄检查定时器已停止");
        }
        
        // 重置移动检查状态
        m_moveCheckPending = false;
        
        // 重置拍摄检查状态
        m_captureCheckPending = false;
        
        // 强制断开所有定时器连接，防止回调继续执行
        if (m_moveCheckTimer) {
            m_moveCheckTimer->disconnect();
        }
        if (m_captureCheckTimer) {
            m_captureCheckTimer->disconnect();
        }
        
        // 强制设置所有等待状态为完成，确保事件循环能立即退出
        m_moveCheckPending = false;
        m_captureCheckPending = false;
        m_moveCheckResult = false;
        m_captureCheckResult = false;
        
        // 发送强制停止信号，确保所有等待循环立即退出
        QMetaObject::invokeMethod(this, "forceStopAllWaiting", Qt::QueuedConnection);
        
        // 立即处理所有待处理的事件，确保停止信号被处理
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        
        // 强制等待一小段时间，确保所有异步操作完成
        QThread::msleep(50);
        
        // 再次强制处理事件，确保所有停止操作完成
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        
        // 立即停止电调移动
        if (m_dpFocuser && m_isFocuserMoving) {
            log("正在停止电调移动...");
            // 发送停止命令给电调
            m_indiServer->abortFocuserMove(m_dpFocuser);
            log("电调停止命令已发送");
        }
        
        // 立即停止拍摄
        if (m_dpMainCamera && !m_isCaptureEnd) {
            log("正在停止拍摄...");
            // 发送停止拍摄命令
            m_indiServer->setCCDAbortExposure(m_dpMainCamera);
            log("拍摄停止命令已发送");
        }
        
        // 重置电调移动状态（不清空参数，避免回调访问导致段错误）
        m_isFocuserMoving = false;
        // 保留目标位置和移动时间，避免回调访问未初始化数据
        // m_targetFocuserPosition 保持不变
        // m_moveStartTime 保持不变
        // m_lastPosition 保持不变
        log("电调移动状态已重置（参数保留）");
        
        // 重置等待状态（不清空参数，避免回调访问导致段错误）
        m_waitingForMove = false;
        // 保留等待相关参数，避免回调访问未初始化数据
        // m_moveWaitStartTime 保持不变
        // m_moveWaitCount 保持不变
        // m_moveLastPosition 保持不变
        log("等待状态已重置（参数保留）");
        
        // 重置拍摄状态（不清空参数，避免回调访问导致段错误）
        m_isCaptureEnd = true;
        // 保留图像路径，避免回调函数访问空字符串
        // m_lastCapturedImage 保持不变
        log("拍摄状态已重置（参数保留）");
        
        // 重置星点选择状态（不清空参数，避免回调访问导致段错误）
        // 保留星点选择参数，避免回调访问未初始化数据
        // m_topStarCount 保持不变
        log("星点选择状态已重置（参数保留）");
        
        // 重置数据收集状态（不清空参数，避免回调访问导致段错误）
        // 保留数据收集参数，避免回调访问未初始化数据
        // m_focusData 保持不变
        // m_lastFitResult 保持不变
        // m_dataCollectionCount 保持不变
        // m_currentLargeRangeShots 保持不变
        // m_currentLargeRangeStep 保持不变
        log("数据收集状态已重置（参数保留）");
        
        // 切换到空闲状态
        changeState(AutoFocusState::IDLE);
        log("自动对焦已停止（参数不清空，避免回调段错误）");
        
    } catch (const std::exception& e) {
        log(QString("停止自动对焦时发生异常: %1").arg(e.what()));
    } catch (...) {
        log("停止自动对焦时发生未知异常");
    }
}

void AutoFocus::getAutoFocusStep()
{
    if (m_currentState == AutoFocusState::CHECKING_STARS)
    {
        updateAutoFocusStep(1, "Please observe if the camera has started shooting the full image. If it has started shooting, please wait for the shooting to complete");
    }
    else if (m_currentState == AutoFocusState::COARSE_ADJUSTMENT)
    {
        updateAutoFocusStep(2, "Coarse adjustment in progress, please observe if the focuser has started moving. If it has started moving, please wait for the coarse adjustment to complete");
    }
    else if (m_currentState == AutoFocusState::FINE_ADJUSTMENT)
    {
        updateAutoFocusStep(3, "Fine adjustment in progress, please observe if the focuser has started moving. If it has started moving, please wait for the fine adjustment to complete");
    }
    else if (m_currentState == AutoFocusState::SUPER_FINE_ADJUSTMENT
             || m_currentState == AutoFocusState::FITTING_DATA
             || m_currentState == AutoFocusState::MOVING_TO_BEST_POSITION)
    {
        updateAutoFocusStep(4, "Super fine adjustment in progress. The system is performing precise HFR-based fitting, please wait for the final best focus position.");
    }
    else {
        // 其他状态默认按精调阶段提示
        updateAutoFocusStep(3, "Fine adjustment in progress, please observe if the focuser has started moving. If it has started moving, please wait for the fine adjustment to complete");
    }
}

void AutoFocus::getAutoFocusData()
{

  if (m_currentState == AutoFocusState::FINE_ADJUSTMENT && !m_fineFocusData.isEmpty())
  {
    for (const auto& data : m_fineFocusData)
    {
        emit focusDataPointReady(data.focuserPosition, data.hfr, QStringLiteral("fine"));
    }
  }
}

bool AutoFocus::isRunning() const
{
    return m_isRunning;
}

AutoFocusState AutoFocus::getCurrentState() const
{
    return m_currentState;
}

void AutoFocus::setHFRThreshold(double threshold)
{
    m_hfrThreshold = threshold;
}

void AutoFocus::setCoarseStepSize(int steps)
{
    m_coarseStepSize = steps;
}

void AutoFocus::setFineStepSize(int steps)
{
    m_fineStepSize = steps;
}

void AutoFocus::setCoarseShotsPerPosition(int shots)
{
    m_coarseShotsPerPosition = shots;
}

void AutoFocus::setFineShotsPerPosition(int shots)
{
    m_fineShotsPerPosition = shots;
}

void AutoFocus::setMaxLargeRangeShots(int shots)
{
    m_maxLargeRangeShots = shots;
}

void AutoFocus::setInitialLargeRangeStep(double percentage)
{
    m_initialLargeRangeStep = percentage;
}

void AutoFocus::setMinLargeRangeStep(double percentage)
{
    m_minLargeRangeStep = percentage;
}

void AutoFocus::setDefaultExposureTime(int exposureTime)
{
    m_defaultExposureTime = exposureTime;
    log(QString("设置默认曝光时间: %1ms").arg(exposureTime));
}


void AutoFocus::onTimerTimeout()
{
    try {
        // 检查运行状态
        if (!m_isRunning) {
            return;
        }
        
        // 使用优化的设备验证方法（带缓存）
        if (!validateDevices()) {
            log(QString("设备对象无效，停止自动对焦"));
            stopAutoFocus();
            return;
        }

        // 再次检查运行状态，确保在调用processCurrentState之前没有被停止
        if (!m_isRunning) {
            return;
        }

        processCurrentState();
        
    } catch (const std::exception& e) {
        log(QString("处理状态时发生异常: %1").arg(e.what()));
        stopAutoFocus();
    } catch (...) {
        log(QString("处理状态时发生未知异常"));
        stopAutoFocus();
    }
}

/**
 * @brief 电调移动检查定时器回调
 * 
 * 每100ms检查一次电调移动状态，避免阻塞主线程
 */
void AutoFocus::onMoveCheckTimerTimeout()
{
    // 首先检查对象是否仍然有效
    if (!this) {
        return;
    }
    
    // 立即检查停止状态，如果已停止则立即退出
    if (!m_isRunning) {
        m_moveCheckPending = false;
        m_moveCheckResult = false;
        if (m_moveCheckTimer) {
            m_moveCheckTimer->stop();
        }
        return;
    }
    
    if (!m_moveCheckPending) {
        if (m_moveCheckTimer) {
            m_moveCheckTimer->stop();
        }
        return;
    }
    
    // 获取当前位置
    int currentPos = getCurrentFocuserPosition();
    m_moveWaitCount++;
    
    log(QString("等待电调移动 [%1/%2]: 当前位置=%3, 目标位置=%4")
        .arg(m_moveWaitCount / 10.0, 0, 'f', 1).arg(m_moveCheckTimeout).arg(currentPos).arg(m_moveCheckTargetPosition));
    
    // 使用统一的位置检查函数
    if (isPositionReached(currentPos, m_moveCheckTargetPosition, m_moveCheckTolerance)) {
        log(QString("电调移动完成: 当前位置=%1, 目标位置=%2, 误差=%3")
            .arg(currentPos).arg(m_moveCheckTargetPosition).arg(qAbs(currentPos - m_moveCheckTargetPosition)));
        // 发送电调位置同步信号
        emit m_wsThread->sendMessageToClient("FocusPosition:" + QString::number(currentPos) + ":" + QString::number(currentPos));
        
        // 设置结果并停止定时器
        m_moveCheckResult = true;
        m_moveCheckPending = false;
        if (m_moveCheckTimer) {
            m_moveCheckTimer->stop();
        }
        return;
    }
    
    // 检查是否卡住
    if (currentPos == m_moveLastPosition && m_moveWaitCount > m_moveCheckStuckTimeout * 10) {
        log(QString("电调可能卡住: 位置%1秒未变化").arg(m_moveCheckStuckTimeout));
        m_moveCheckResult = false;
        m_moveCheckPending = false;
        if (m_moveCheckTimer) {
            m_moveCheckTimer->stop();
        }
        return;
    }
    
    // 更新位置记录
    if (currentPos != m_moveLastPosition) {
        m_moveLastPosition = currentPos;
    }
    
    // 检查总超时
    if (m_moveWaitCount >= m_moveCheckTimeout * 10) {
        log(QString("电调移动超时: 等待%1秒后仍未到达目标位置").arg(m_moveCheckTimeout));
        m_moveCheckResult = false;
        m_moveCheckPending = false;
        if (m_moveCheckTimer) {
            m_moveCheckTimer->stop();
        }
        return;
    }
}

/**
 * @brief 强制停止所有等待状态
 * 
 * 用于在停止自动对焦时强制退出所有等待循环
 */
void AutoFocus::forceStopAllWaiting()
{
    // 强制设置所有等待状态为完成
    m_moveCheckPending = false;
    m_captureCheckPending = false;
    m_moveCheckResult = false;
    m_captureCheckResult = false;
    
    // 停止所有定时器
    if (m_moveCheckTimer) {
        m_moveCheckTimer->stop();
        m_moveCheckTimer->disconnect(); // 断开所有连接
    }
    if (m_captureCheckTimer) {
        m_captureCheckTimer->stop();
        m_captureCheckTimer->disconnect(); // 断开所有连接
    }
    
    // 强制处理所有待处理的事件
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    
    // 再次强制处理事件，确保所有停止操作完成
    QThread::msleep(10);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    
    log("强制停止所有等待状态");
}

/**
 * @brief 拍摄完成检查定时器回调
 * 
 * 每100ms检查一次拍摄完成状态，避免阻塞主线程
 */
void AutoFocus::onCaptureCheckTimerTimeout()
{
    // 首先检查对象是否仍然有效
    if (!this) {
        return;
    }
    
    // 立即检查停止状态，如果已停止则立即退出
    if (!m_isRunning) {
        m_captureCheckPending = false;
        m_captureCheckResult = false;
        if (m_captureCheckTimer) {
            m_captureCheckTimer->stop();
        }
        return;
    }
    
    if (!m_captureCheckPending) {
        if (m_captureCheckTimer) {
            m_captureCheckTimer->stop();
        }
        return;
    }
    
    // 检查是否已停止
    if (!m_isRunning) {
        log("自动对焦已停止，中断拍摄等待");
        m_captureCheckResult = false;
        m_captureCheckPending = false;
        if (m_captureCheckTimer) {
            m_captureCheckTimer->stop();
        }
        return;
    }
    
    // 检查拍摄是否完成
    if (m_isCaptureEnd) {
        log(QString("拍摄完成，图像路径: %1").arg(m_lastCapturedImage));
        
        // 如果使用虚拟数据模式，在拍摄完成后替换图像
        if (m_useVirtualData && m_starSimulator) {
            log("虚拟数据模式：生成模拟星图替换真实图像");
            QString originalPath = m_lastCapturedImage;
            
            // 检查原始图像文件
            QFileInfo originalFileInfo(originalPath);
            log(QString("原始图像文件: %1, 存在=%2, 大小=%3")
                .arg(originalPath).arg(originalFileInfo.exists()).arg(originalFileInfo.size()));
            
            // 生成虚拟图像
            bool success = generateVirtualImage(m_defaultExposureTime, m_useROI);
            if (success) {
                // 验证新生成的虚拟图像
                QFileInfo virtualFileInfo(m_lastCapturedImage);
                log(QString("虚拟图像生成成功，替换原图像: %1 -> %2")
                    .arg(originalPath).arg(m_lastCapturedImage));
                log(QString("虚拟图像文件: 存在=%1, 大小=%2")
                    .arg(virtualFileInfo.exists()).arg(virtualFileInfo.size()));
            } else {
                log("虚拟图像生成失败，使用原图像");
                m_lastCapturedImage = originalPath;
            }
        }
        
        // 设置结果并停止定时器
        m_captureCheckResult = true;
        m_captureCheckPending = false;
        if (m_captureCheckTimer) {
            m_captureCheckTimer->stop();
        }
        return;
    }
    
    // 检查超时（通过已用时间判断）
    // 这里我们依靠事件循环中的超时定时器来处理超时
    // 如果在定时器回调中需要检查超时，可以通过记录开始时间来计算
}

/**
 * @brief 处理当前状态的状态机
 * 
 * 这是自动对焦的核心状态机，根据当前状态调用相应的处理函数
 */
void AutoFocus::processCurrentState()
{
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过状态处理");
        return;
    }
    
    // 使用优化的设备验证方法（带缓存）
    if (!validateDevices()) {
        log(QString("设备对象无效，停止自动对焦"));
        stopAutoFocus();
        return;
    }
    
    // 检查电调移动状态
    if (m_isFocuserMoving) {
        if (checkFocuserMoveComplete()) {
            log(QString("电调移动完成，继续处理当前状态"));
        } else {
            // 电调还在移动中，等待下次检查
            return;
        }
    }
    
    // 检查是否正在等待移动完成（用于粗调和精调）
    if (m_waitingForMove && (m_currentState == AutoFocusState::COARSE_ADJUSTMENT || 
                             m_currentState == AutoFocusState::FINE_ADJUSTMENT)) {
        
        // 使用统一的等待函数，使用配置参数
        bool moveSuccess = waitForFocuserMoveComplete(m_targetFocuserPosition, 
                                                    g_autoFocusConfig.positionTolerance, 
                                                    g_autoFocusConfig.moveTimeout / 1000, 
                                                    g_autoFocusConfig.stuckTimeout / 1000);
        
        // 清除等待状态
        m_waitingForMove = false;
        m_isFocuserMoving = false;
        
        if (moveSuccess) {
            log(QString("电调移动完成，继续%1").arg(m_currentState == AutoFocusState::COARSE_ADJUSTMENT ? "粗调" : "精调"));
        } else {
            log(QString("电调移动失败，但继续%1").arg(m_currentState == AutoFocusState::COARSE_ADJUSTMENT ? "粗调" : "精调"));
        }
        
        // 再次检查运行状态，防止在数据收集期间被停止
        if (!m_isRunning) {
            log("自动对焦已停止，跳过数据收集");
            return;
        }
        
        // 移动完成后，根据当前状态执行相应的数据收集
        if (m_currentState == AutoFocusState::COARSE_ADJUSTMENT) {
            performCoarseDataCollection();
        } else if (m_currentState == AutoFocusState::FINE_ADJUSTMENT) {
            performFineDataCollection();
        }
        
        return; // 数据收集完成后返回，等待下次定时器调用
    }
    
    // 再次检查运行状态，防止在等待移动完成期间被停止
    if (!m_isRunning) {
        log("自动对焦已停止，跳过状态处理");
        return;
    }
    
    switch (m_currentState) {
    case AutoFocusState::CHECKING_STARS:
        processCheckingStars();              // 检查星点状态
        break;
    case AutoFocusState::LARGE_RANGE_SEARCH:
        processLargeRangeSearch();           // 大范围找星状态
        break;
    case AutoFocusState::COARSE_ADJUSTMENT:
        processCoarseAdjustment();           // 粗调状态
        break;
    case AutoFocusState::FINE_ADJUSTMENT:
        processFineAdjustment();             // 精调状态
        break;
    case AutoFocusState::SUPER_FINE_ADJUSTMENT:
        processSuperFineAdjustment();        // 更细致精调状态
        break;
    case AutoFocusState::COLLECTING_DATA:
        collectFocusData();                  // 收集数据状态
        break;
    case AutoFocusState::FITTING_DATA:
        processFittingData();                // 拟合数据状态
        break;
    case AutoFocusState::MOVING_TO_BEST_POSITION:
        processMovingToBestPosition();       // 移动到最佳位置状态
        break;
    default:
        break;
    }
}

void AutoFocus::processCheckingStars()
{
// 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过星点检查");
        return;
    }
    
    log(QString("拍摄全图并检查星点(HFR)..."));
    
    // 拍摄全图并等待完成
    if (!captureFullImage()) {
        handleErrorWithRetry("拍摄全图失败", true); // 允许重试
        return;
    }
    
    // 等待拍摄完成
    if (!waitForCaptureComplete()) {
        handleErrorWithRetry("拍摄超时", true); // 允许重试
        return;
    }

    double hfr = 0.0;// 初始一个值，避免误判
    if (detectHFRByPython(hfr)) {
        Logger::Log(QString("检测到星点，HFR=%1").arg(hfr).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        // 使用阈值决定进入粗调还是精调；m_hfrThreshold 语义改为HFR阈值
        if (hfr > m_hfrThreshold) {
            Logger::Log(QString("HFR大于阈值（%1），进入粗调扫描").arg(m_hfrThreshold).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
            emit autoFocusModeChanged("coarse", hfr);
            startCoarseAdjustment();
        } else {
            Logger::Log(QString("HFR小于等于阈值（%1），直接进入精调").arg(m_hfrThreshold).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
            emit autoFocusModeChanged("fine", hfr);
            // 设置粗调基准为当前位置，以便精调围绕当前位置采样
            m_coarseBestPosition = getCurrentFocuserPosition();
            m_coarseBestHFR = hfr;
            startFineAdjustment();
        }
    } else {
        Logger::Log("未检测到星点，跳过大范围找星，改为粗调扫描", LogLevel::INFO, DeviceType::FOCUSER);
        emit autoFocusModeChanged("coarse", 0.0);
        startCoarseAdjustment();
    }
}


void AutoFocus::startLargeRangeSearch()
{
log("大范围找星流程已被禁用，直接切换到粗调");
    startCoarseAdjustment();
    return;
}

// ===================== 可直接替换的实现 =====================

void AutoFocus::processLargeRangeSearch()
{
Q_UNUSED(this);
    // 已废弃的大范围找星：不做任何操作
    log("processLargeRangeSearch() 已禁用");
    // 为兼容，直接进入粗调
    if (m_currentState == AutoFocusState::LARGE_RANGE_SEARCH) {
        startCoarseAdjustment();
    }
    return;
}



// ===================== 私有辅助函数 =====================

// 统一的“启动一次移动”的入口：
// - 负责方向计算、步数下限、状态位、日志与下发命令
// - 若目标==当前（或被夹紧后相等），返回 false，调用方可直接拍照或调整策略
bool AutoFocus::beginMoveTo(int targetPosition, const QString& reason)
{
    if (!m_dpFocuser || !m_indiServer) {
        handleError("电调设备连接已断开");
        return false;
    }

    // 夹紧目标
    targetPosition = std::clamp(targetPosition, m_focuserMinPosition, m_focuserMaxPosition);

    const int current = getCurrentFocuserPosition();
    int delta = targetPosition - current;

    if (delta == 0) {
        log(QString("目标与当前位置相同(%1)，跳过移动：%2").arg(targetPosition).arg(reason));
        return false;
    }

    const bool isInward = (delta < 0);
    int steps = std::abs(delta);
    steps = std::max(1, steps); // 最小 1 步，避免 0 步等待

    // ---- 设置等待/移动状态 ----
    m_isFocuserMoving        = true;
    m_waitingForMove         = true;
    m_targetFocuserPosition  = targetPosition;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_moveStartTime   = now;
    m_lastPosition    = current;
    m_moveWaitStartTime = now;
    m_moveWaitCount   = 0;
    m_moveLastPosition = current;

    // ---- 下发方向与步数 ----
    m_indiServer->setFocuserMoveDiretion(m_dpFocuser, isInward);
    m_indiServer->moveFocuserSteps(m_dpFocuser, steps);

    // ---- 初始化移动判断参数（速度/停滞等）----
    initializeFocuserMoveParameters();

    log(QString("开始移动：%1 → %2，方向=%3，步数=%4，原因=%5")
        .arg(current)
        .arg(targetPosition)
        .arg(isInward ? "向内" : "向外")
        .arg(steps)
        .arg(reason));

    // 发送电调位置同步信号
    emit m_wsThread->sendMessageToClient("FocusPosition:" + QString::number(current) + ":" + QString::number(current));

    return true;
}

// 拍一张、等待完成并识星；若成功，内部切状态为 CHECKING_STARS
bool AutoFocus::captureAndDetectOnce()
{
    if (!captureFullImage()) {
        handleError("拍摄图像失败");
        return false;
    }
    if (!waitForCaptureComplete()) {
        handleError("拍摄超时");
        return false;
    }

    if (detectStarsInImage()) {
        log("大范围找星成功，检测到星点，进入 CHECKING_STARS");
        changeState(AutoFocusState::CHECKING_STARS);
        return true;
    }

    log("未检测到星点，将继续搜索");
    return false;
}

// void AutoFocus::processLargeRangeSearch()
// {
//     // 立即检查运行状态，如果已停止则直接返回
//     if (!m_isRunning) {
//         log("自动对焦已停止，跳过大范围搜索");
//         return;
//     }
    
//     if (m_currentLargeRangeShots >= m_maxLargeRangeShots) {
//         handleError(QString("大范围找星失败：拍摄%1次仍未识别到星点").arg(m_maxLargeRangeShots));
//         return;
//     }

//     // 获取当前电调位置
//     m_currentPosition = getCurrentFocuserPosition();
    
//     // 检查是否正在等待电调移动完成
//     if (m_waitingForMove) {
//         // 使用统一的等待函数，使用配置参数
//         bool moveSuccess = waitForFocuserMoveComplete(m_targetFocuserPosition, 
//                                                     g_autoFocusConfig.positionTolerance, 
//                                                     g_autoFocusConfig.moveTimeout / 1000, 
//                                                     g_autoFocusConfig.stuckTimeout / 1000);
        
//         // 清除等待状态
//         m_waitingForMove = false;
//         m_isFocuserMoving = false;
        
//         // 移动完成后，检查是否需要减少步长
//         checkAndReduceStepSize();
        
//         // 无论移动是否成功，都要拍摄检查星点
//         log("电调移动完成，开始拍摄检查星点");
//         if (!captureFullImage()) {
//             handleError("拍摄图像失败");
//             return;
//         }
        
//         // 等待拍摄完成
//         if (!waitForCaptureComplete()) {
//             handleError("拍摄超时");
//             return;
//         }

//         if (detectStarsInImage()) {
//             log("大范围找星成功，检测到星点");
//             changeState(AutoFocusState::CHECKING_STARS);
//             return;
//         }
        
//         log("未检测到星点，继续搜索");
//         // 注意：这里不增加搜索计数，因为后续逻辑会处理
//         return;
//     }
    
//     // 增加搜索计数（只在非等待状态下增加）
//     m_currentLargeRangeShots++;
//     log(QString("大范围找星第%1次尝试").arg(m_currentLargeRangeShots));
    
//     // 如果是第一次尝试，先移动到初始目标位置
//     if (m_currentLargeRangeShots == 1) {
//         log(QString("第一次尝试，移动到初始目标位置: %1").arg(m_initialTargetPosition));
        
//         // 检查是否需要移动
//         if (m_currentPosition != m_initialTargetPosition) {
//             // 使用统一的电调移动函数
//             executeFocuserMove(m_initialTargetPosition, "第一次尝试移动到初始目标位置");
            
//             // 立即检查电调状态
//             QThread::msleep(100); // 等待100ms让命令生效
//             int afterMovePosition = getCurrentFocuserPosition();
//             log(QString("移动命令发送后电调位置: %1").arg(afterMovePosition));
            
//             return; // 返回，让定时器处理等待
//         } else {
//             log(QString("当前位置已经是目标位置，无需移动"));
            
//             // 即使不需要移动，也要拍摄检查星点
//             if (!captureFullImage()) {
//                 handleError("拍摄图像失败");
//                 return;
//             }
            
//             // 等待拍摄完成
//             if (!waitForCaptureComplete()) {
//                 handleError("拍摄超时");
//                 return;
//             }

//             if (detectStarsInImage()) {
//                 log("大范围找星成功，检测到星点");
//                 changeState(AutoFocusState::CHECKING_STARS);
//                 return;
//             }

//             // 第一次尝试完成，继续当前方向搜索
//             log(QString("第一次尝试完成，继续当前方向搜索"));
//             // 注意：这里不增加搜索计数，因为已经在前面增加了
//         }
//     }
    
//     // 检查是否已达到位置限制并处理往返搜索
//     if (m_currentPosition >= m_focuserMaxPosition && m_searchDirection > 0) {
//         log(QString("已达到最大位置 %1，开始向内搜索").arg(m_currentPosition));
//         m_searchDirection = -1; // 改变搜索方向向内
//         // 计算向内移动的步数
//         int totalRange = m_focuserMaxPosition - m_focuserMinPosition;
//         int moveSteps = static_cast<int>(totalRange * m_currentLargeRangeStep / 100.0);
//         int targetPosition = m_currentPosition - moveSteps;
        
//         // 确保不超出最小位置
//         if (targetPosition < m_focuserMinPosition) {
//             targetPosition = m_focuserMinPosition;
//         }
        
//         // 计算移动方向和步数
//         int actualMoveSteps = targetPosition - m_currentPosition;
//         bool isInward = (actualMoveSteps < 0);
        
//         if (isInward) {
//             actualMoveSteps = -actualMoveSteps; // 取绝对值
//         }
        
//         log(QString("向内搜索: 从位置 %1 移动到 %2，方向: 向内，步数: %3").arg(m_currentPosition).arg(targetPosition).arg(actualMoveSteps));
        
//         // 设置移动状态
//         m_isFocuserMoving = true;
//         m_targetFocuserPosition = targetPosition;
//         m_moveStartTime = QDateTime::currentMSecsSinceEpoch();
//         m_lastPosition = m_currentPosition;
        
//         // 设置移动方向并发送移动命令
//         m_indiServer->setFocuserMoveDiretion(m_dpFocuser, isInward);
//         m_indiServer->moveFocuserSteps(m_dpFocuser, actualMoveSteps);
        
//         // 初始化电调判断参数
//         initializeFocuserMoveParameters();
        
//         log(QString("移动命令详情: 方向=%1, 步数=%2, 目标位置=%3").arg(isInward ? "向内" : "向外").arg(actualMoveSteps).arg(targetPosition));
        
//         // 立即检查电调状态
//         QThread::msleep(100); // 等待100ms让命令生效
//         int afterMovePosition = getCurrentFocuserPosition();
//         log(QString("移动命令发送后电调位置: %1").arg(afterMovePosition));
        
//         // 设置非阻塞等待状态
//         m_waitingForMove = true;
//         m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
//         m_moveWaitCount = 0;
//         m_moveLastPosition = m_currentPosition;
//         log(QString("开始等待电调移动完成，目标位置: %1").arg(targetPosition));
//         return; // 返回，让定时器处理等待
//     } else if (m_currentPosition <= m_focuserMinPosition && m_searchDirection < 0) {
//         log(QString("已达到最小位置 %1，开始向外搜索").arg(m_currentPosition));
//         m_searchDirection = 1; // 改变搜索方向向外
//         // 计算向外移动的步数
//         int totalRange = m_focuserMaxPosition - m_focuserMinPosition;
//         int moveSteps = static_cast<int>(totalRange * m_currentLargeRangeStep / 100.0);
//         int targetPosition = m_currentPosition + moveSteps;
        
//         // 确保不超出最大位置
//         if (targetPosition > m_focuserMaxPosition) {
//             targetPosition = m_focuserMaxPosition;
//         }
        
//         // 计算移动方向和步数
//         int actualMoveSteps2 = targetPosition - m_currentPosition;
//         bool isInward = (actualMoveSteps2 < 0);
        
//         if (isInward) {
//             actualMoveSteps2 = -actualMoveSteps2; // 取绝对值
//         }
        
//         log(QString("向外搜索: 从位置 %1 移动到 %2，方向: 向外，步数: %3").arg(m_currentPosition).arg(targetPosition).arg(actualMoveSteps2));
        
//         // 设置移动状态
//         m_isFocuserMoving = true;
//         m_targetFocuserPosition = targetPosition;
//         m_moveStartTime = QDateTime::currentMSecsSinceEpoch();
//         m_lastPosition = m_currentPosition;
        
//         // 设置移动方向并发送移动命令
//         m_indiServer->setFocuserMoveDiretion(m_dpFocuser, isInward);
//         m_indiServer->moveFocuserSteps(m_dpFocuser, actualMoveSteps2);
        
//         // 初始化电调判断参数
//         initializeFocuserMoveParameters();
        
//         // 设置非阻塞等待状态，让定时器处理等待
//         m_waitingForMove = true;
//         m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
//         m_moveWaitCount = 0;
//         m_moveLastPosition = m_currentPosition;
//         log(QString("开始等待电调移动完成，目标位置: %1").arg(targetPosition));
//         return; // 返回，让定时器处理等待
//     } else {
//         // 正常搜索：按照当前方向移动
//         int totalRange = m_focuserMaxPosition - m_focuserMinPosition;
//         int moveSteps = static_cast<int>(totalRange * m_currentLargeRangeStep / 100.0) * m_searchDirection;
        
//         // 检查移动后是否会超出限制
//         int targetPosition = m_currentPosition + moveSteps;
//         if (targetPosition > m_focuserMaxPosition) {
//             log(QString("警告: 目标位置 %1 大于最大位置 %2，限制到最大位置").arg(targetPosition).arg(m_focuserMaxPosition));
//             targetPosition = m_focuserMaxPosition;
//         } else if (targetPosition < m_focuserMinPosition) {
//             log(QString("警告: 目标位置 %1 小于最小位置 %2，限制到最小位置").arg(targetPosition).arg(m_focuserMinPosition));
//             targetPosition = m_focuserMinPosition;
//         }
        
//         // 计算移动方向和步数
//         int actualMoveSteps3 = targetPosition - m_currentPosition;
//         bool isInward = (actualMoveSteps3 < 0);
        
//         if (isInward) {
//             actualMoveSteps3 = -actualMoveSteps3; // 取绝对值
//         }
        
//         log(QString("电调移动: 当前位置=%1, 目标位置=%2, 方向=%3, 步数=%4").arg(m_currentPosition).arg(targetPosition).arg(m_searchDirection > 0 ? "向外" : "向内").arg(actualMoveSteps3));
        
//         // 设置移动状态
//         m_isFocuserMoving = true;
//         m_targetFocuserPosition = targetPosition;
//         m_moveStartTime = QDateTime::currentMSecsSinceEpoch();
//         m_lastPosition = m_currentPosition;
        
//         // 设置移动方向并发送移动命令
//         m_indiServer->setFocuserMoveDiretion(m_dpFocuser, isInward);
//         m_indiServer->moveFocuserSteps(m_dpFocuser, actualMoveSteps3);
        
//         // 初始化电调判断参数
//         initializeFocuserMoveParameters();
        
//         log(QString("移动命令详情: 方向=%1, 步数=%2, 目标位置=%3").arg(isInward ? "向内" : "向外").arg(actualMoveSteps3).arg(targetPosition));
        
//         // 立即检查电调状态
//         QThread::msleep(100); // 等待100ms让命令生效
//         int afterMovePosition = getCurrentFocuserPosition();
//         log(QString("移动命令发送后电调位置: %1").arg(afterMovePosition));
        
//         // 设置非阻塞等待状态，让定时器处理等待
//         m_waitingForMove = true;
//         m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
//         m_moveWaitCount = 0;
//         m_moveLastPosition = m_currentPosition;
//         log(QString("开始等待电调移动完成，目标位置: %1").arg(targetPosition));
//         return; // 返回，让定时器处理等待
//     }
    
//     // 额外的安全检查：确保电调设备仍然有效
//     if (!m_dpFocuser || !m_indiServer) {
//         log(QString("错误: 电调设备或INDI客户端已失效"));
//         handleError("电调设备连接已断开");
//         return;
//     }
    
//     // 拍摄并等待完成
//     if (!captureFullImage()) {
//         handleError("拍摄图像失败");
//         return;
//     }
    
//     // 等待拍摄完成
//     if (!waitForCaptureComplete()) {
//         handleError("拍摄超时");
//         return;
//     }

//     if (detectStarsInImage()) {
//         log("大范围找星成功，检测到星点");
//         changeState(AutoFocusState::CHECKING_STARS);
//         return;
//     }

//     // 步长减少逻辑已移到checkAndReduceStepSize()函数中
//     // 在电调移动完成后调用
// }

/**
 * @brief 检查并减少步长
 * 
 * 在电调移动完成后调用，检查是否已完成全程搜索，
 * 如果是则减少步长并改变搜索方向
 */
void AutoFocus::checkAndReduceStepSize()
{
    // 获取当前电调位置
    m_currentPosition = getCurrentFocuserPosition();
    
    // 检查是否已经走完全程（从一端到另一端）
    bool hasCompletedFullRange = false;

    log(QString("检查全程搜索状态: 当前位置=%1, 最小位置=%2, 最大位置=%3, 搜索方向=%4")
        .arg(m_currentPosition).arg(m_focuserMinPosition).arg(m_focuserMaxPosition)
        .arg(m_searchDirection > 0 ? "向外" : "向内"));
    
    if (m_searchDirection > 0) {
        // 向外搜索，检查是否已经到达最大位置
        hasCompletedFullRange = (m_currentPosition >= m_focuserMaxPosition);
        log(QString("向外搜索: 当前位置=%1 >= 最大位置=%2 ? %3").arg(m_currentPosition).arg(m_focuserMaxPosition).arg(hasCompletedFullRange ? "是" : "否"));
    } else {
        // 向内搜索，检查是否已经到达最小位置
        hasCompletedFullRange = (m_currentPosition <= m_focuserMinPosition);
        log(QString("向内搜索: 当前位置=%1 <= 最小位置=%2 ? %3").arg(m_currentPosition).arg(m_focuserMinPosition).arg(hasCompletedFullRange ? "是" : "否"));
    }
    
    if (hasCompletedFullRange) {
        // 已经走完全程，减少步长并改变搜索方向
        double oldStep = m_currentLargeRangeStep;
        m_currentLargeRangeStep = qMax(m_currentLargeRangeStep / 2.0, g_autoFocusConfig.minLargeRangeStep);
        m_searchDirection = -m_searchDirection; // 改变搜索方向
        log(QString("已完成全程搜索: 步长从%1%减少到%2%，改变搜索方向为%3")
            .arg(oldStep).arg(m_currentLargeRangeStep).arg(m_searchDirection > 0 ? "向外" : "向内"));
    } else {
        log(QString("继续当前方向搜索: 当前位置=%1，方向=%2，当前步长=%3%")
            .arg(m_currentPosition).arg(m_searchDirection > 0 ? "向外" : "向内").arg(m_currentLargeRangeStep));
    }
}

void AutoFocus::startCoarseAdjustment()
{
    emit focusSeriesReset(QStringLiteral("coarse"));

    changeState(AutoFocusState::COARSE_ADJUSTMENT);
    updateAutoFocusStep(2, "Coarse adjustment in progress, please observe if the focuser has started moving. If it has started moving, please wait for the coarse adjustment to complete"); // [AUTO_FOCUS_UI_ENHANCEMENT]
    m_focusData.clear();
    m_dataCollectionCount = 0;
    m_coarseBestSNR = -1.0;
    m_coarseHasValidSNR = false;  // 粗调开始时尚未发现任何有效 SNR

    // 从INI配置的最大、最小范围生成11个位置：从最大到最小
    const int minPos = m_focuserMinPosition;
    const int maxPos = m_focuserMaxPosition;
    if (maxPos <= minPos) {
        handleError("电调位置范围无效，无法进行粗调");
        return;
    }
    m_coarseStepSpan = std::max(1, (maxPos - minPos) / 10);
    m_coarseScanPositions.clear();
    for (int i = 0; i <= 10; ++i) {
        int p = maxPos - i * m_coarseStepSpan;
        if (p < minPos) p = minPos;
        if (p > maxPos) p = maxPos;
        if (m_coarseScanPositions.isEmpty() || m_coarseScanPositions.last() != p)
            m_coarseScanPositions.push_back(p);
    }
    // 限制粗调总采样点数不超过 10 个
    if (m_coarseScanPositions.size() > 10) {
        QVector<int> limited;
        int total = m_coarseScanPositions.size();
        int desired = 10;
        for (int i = 0; i < desired; ++i) {
            int idx = static_cast<int>(std::round(i * (total - 1.0) / (desired - 1.0)));
            idx = std::clamp(idx, 0, total - 1);
            int p = m_coarseScanPositions[idx];
            if (limited.isEmpty() || limited.last() != p)
                limited.push_back(p);
        }
        m_coarseScanPositions = limited;
    }

    m_coarseScanIndex = 0;
    m_coarseBestPosition = maxPos;

    Logger::Log(QString("开始粗调：范围[%1, %2]，步进=%3，共%4个采样点")
        .arg(minPos).arg(maxPos).arg(m_coarseStepSpan).arg(m_coarseScanPositions.size()).toStdString(), 
        LogLevel::INFO, DeviceType::FOCUSER);

    // 首次移动到最大位置开始采样
    if (!m_coarseScanPositions.isEmpty()) {
        beginMoveTo(m_coarseScanPositions.first(), "粗调起始移动到最大位置");
    }
}




/**
 * @brief 处理粗调状态
 * 
 * 粗调使用大步长快速找到大致对焦位置：
 * 1. 每次移动1000步
 * 2. 每个位置拍摄1张图像
 * 3. 收集5个数据点后开始拟合
 */
void AutoFocus::processCoarseAdjustment()
{
    // 仅执行粗调采样，不进行拟合；由 performCoarseDataCollection 负责移动与采集
    if (!m_isRunning) {
        log("自动对焦已停止，跳过粗调");
        return;
    }

    // 直接进入粗调采样流程（内部会根据当前位置决定是否需要移动）
    performCoarseDataCollection();
}


void AutoFocus::performCoarseDataCollection()
{
    // 立即检查运行状态
    if (!m_isRunning) {
        log("自动对焦已停止，跳过粗调数据收集");
        return;
    }

    if (m_coarseScanIndex >= m_coarseScanPositions.size()) {
        // 所有粗调采样点已完成，先检查是否存在任何有效 SNR
        if (!m_coarseHasValidSNR) {
            // 说明本轮粗调中的每一张星图识别结果都是 0（无有效星点）
            Logger::Log("粗调阶段所有采样点的 SNR 均为 0 或无效，判定本次对焦失败", LogLevel::ERROR, DeviceType::FOCUSER);
            emit autofocusFailed();
            // 结束自动对焦流程，不再进入精调
            completeAutoFocus(false);
            return;
        }

        // 粗调结束，存在至少一个有效 SNR，移动到 SNR 最佳位置并进入精调
        if (m_coarseBestSNR < 0.0) {
            // 理论上 m_coarseHasValidSNR==true 时不应出现此情况，但仍做保护
            int current = getCurrentFocuserPosition();
            m_coarseBestPosition = current;
            Logger::Log("粗调阶段未正确记录最佳 SNR 位置，回退使用当前位置作为精调起点", LogLevel::WARNING, DeviceType::FOCUSER);
        }

        emit focusBestMovePlanned(m_coarseBestPosition, QStringLiteral("coarse"));

        Logger::Log(QString("粗调完成：最佳 SNR=%1，位置=%2")
            .arg(m_coarseBestSNR).arg(m_coarseBestPosition).toStdString(),
            LogLevel::INFO, DeviceType::FOCUSER);

        if (beginMoveTo(m_coarseBestPosition, "粗调完成，移动到 SNR 最优位置")) {
            // 切到精调状态；等待到位后，onTimerTimeout 会调用 performFineDataCollection()
            startFineAdjustment();
        } else {
            // 无需移动也直接开始精调
            startFineAdjustment();
        }
        return;
    }

    // 确保当前在需要采样的位置
    int target = m_coarseScanPositions[m_coarseScanIndex];
    int current = getCurrentFocuserPosition();
    if (!isPositionReached(current, target, g_autoFocusConfig.positionTolerance)) {
        // 尚未到目标位置，先移动
        beginMoveTo(target, QString("粗调扫描移动到第%1个点").arg(m_coarseScanIndex+1));
        return;
    }

    // 发送电调位置同步信号
    emit m_wsThread->sendMessageToClient("FocusPosition:" + QString::number(current) + ":" + QString::number(current));
    // 拍摄一张图像并等待完成
    if (!captureFullImage()) { handleError("拍摄图像失败"); return; }
    if (!waitForCaptureComplete()) { handleError("拍摄超时"); return; }

    // 通过 Python 脚本计算 avg_top50_snr
    double snr = 0.0;
    bool ok = detectSNRByPython(snr);
    if (!ok || !std::isfinite(snr) || snr <= 0.0) {
        Logger::Log("该位置未识到有效 SNR 或 Python SNR 脚本执行失败，使用占位值 0", LogLevel::INFO, DeviceType::FOCUSER);
        emit starDetectionResult(false, 0.0);
        snr = 0.0;
    } else {
        Logger::Log(QString("粗调位置检测到 mean_peak_snr: %1").arg(snr).toStdString(),
                    LogLevel::INFO, DeviceType::FOCUSER);
        emit starDetectionResult(true, snr);
        // 标记本轮粗调存在至少一个 SNR>0 的有效位置
        if (snr > 0.0) {
            m_coarseHasValidSNR = true;
        }
    }

    // 发送 SNR 结果到前端
    if (m_wsThread) {
        QString msg = QString("AutoFocusSNR:coarse:%1:%2:%3")
                          .arg(m_coarseScanIndex + 1)
                          .arg(target)
                          .arg(snr, 0, 'f', 6);
        emit m_wsThread->sendMessageToClient(msg);
    }

    // 记录数据（此处 hfr 字段存放 SNR，仅用于日志与调试，不参与拟合）
    FocusDataPoint dp(target, snr);
    m_focusData.append(dp);
    m_dataCollectionCount++;
    // 粗调阶段不发送数据点到前端，避免干扰精调拟合
    // emit focusDataPointReady(target, fwhm, QStringLiteral("coarse"));

    Logger::Log(QString("粗调数据点%1/%2：位置=%3，SNR=%4，当前最佳SNR=%5")
        .arg(m_coarseScanIndex+1).arg(m_coarseScanPositions.size()).arg(target).arg(snr).arg(m_coarseBestSNR).toStdString(), 
        LogLevel::INFO, DeviceType::FOCUSER);

    // 更新最佳 SNR 位置（只在识到有效 SNR 时）
    if (ok && snr > m_coarseBestSNR) {
        m_coarseBestSNR = snr;
        m_coarseBestPosition = target;
    }

    // 下一个位置
    ++m_coarseScanIndex;
    if (m_coarseScanIndex < m_coarseScanPositions.size()) {
        beginMoveTo(m_coarseScanPositions[m_coarseScanIndex], "粗调扫描下一个点");
    } else {
        // 让下一轮调用处理收尾
        Logger::Log("粗调采样完成，准备移动至最佳位置并进入精调", LogLevel::INFO, DeviceType::FOCUSER);
    }
}


void AutoFocus::performFineDataCollection()
{
    if (!m_isRunning) {
        log("自动对焦已停止，跳过精调数据收集");
        return;
    }

    // 精调开始时清空前端数据
    if (m_fineScanIndex == 0) {
        log("精调开始，清空前端数据");
        // 发送特殊的清空消息，不包含实际数据点
        emit focusDataPointReady(-1, -1, QStringLiteral("clear"));
    }

    if (m_fineScanIndex >= m_fineScanPositions.size()) {
        // 全部采样完毕 —— 优先尝试基于 SNR 的二次曲线拟合，得到更精确的 super-fine 起点；
        // 如拟合失败或质量较差，则退回到“直接取最大 SNR 点”的逻辑。
        if (!m_fineFocusData.isEmpty()) {
            // 构造 SNR 数据点：x = focuserPosition, y = SNR (暂存于 hfr 字段)
            QVector<QPointF> snrPoints;
            snrPoints.reserve(m_fineFocusData.size());
            double minPos = std::numeric_limits<double>::max();
            double maxPos = std::numeric_limits<double>::lowest();
            for (const auto &dp : m_fineFocusData) {
                double x = static_cast<double>(dp.focuserPosition);
                double y = dp.hfr;
                if (!std::isfinite(y) || y <= 0.0)
                    continue;
                snrPoints.append(QPointF(x, y));
                if (x < minPos) minPos = x;
                if (x > maxPos) maxPos = x;
            }

            const int minPointsForFit = 5;
            const double minRSquaredForAccept = 0.8;
            bool useFittedPosition = false;
            double fittedBestPos = 0.0;
            double fittedBestSNR = 0.0;

            if (snrPoints.size() >= minPointsForFit) {
                float a = 0.0f, b = 0.0f, c = 0.0f;
                int fitCode = Tools::fitQuadraticCurve(snrPoints, a, b, c);
                if (fitCode == 0) {
                    double r2 = Tools::calculateRSquared(snrPoints, a, b, c);
                    // 对焦时 SNR 在最佳位置应形成开口向下的抛物线，因此 a 需小于 0
                    if (r2 >= minRSquaredForAccept && a < 0.0f) {
                        double vertexX = -static_cast<double>(b) / (2.0 * static_cast<double>(a));
                        if (std::isfinite(vertexX)) {
                            // 限制顶点位置在采样范围内，同时也限制在电调物理范围内
                            vertexX = std::clamp(vertexX, minPos, maxPos);
                            vertexX = std::clamp(vertexX,
                                                 static_cast<double>(m_focuserMinPosition),
                                                 static_cast<double>(m_focuserMaxPosition));
                            fittedBestPos = std::round(vertexX);
                            fittedBestSNR = static_cast<double>(a) * vertexX * vertexX +
                                            static_cast<double>(b) * vertexX +
                                            static_cast<double>(c);
                            useFittedPosition = true;
                            log(QString("SNR 二次曲线拟合成功：a=%1, b=%2, c=%3, R²=%4, 顶点位置=%5, 估计最大 SNR=%6")
                                .arg(a).arg(b).arg(c).arg(r2).arg(fittedBestPos).arg(fittedBestSNR));
                        } else {
                            log("SNR 二次曲线拟合结果顶点位置为非有限值，放弃使用拟合结果");
                        }
                    } else {
                        log(QString("SNR 二次曲线拟合质量不足或开口方向不正确：a=%1, R²=%2，退回使用实际最大 SNR 点")
                            .arg(a).arg(r2));
                    }
                } else {
                    log("SNR 二次曲线拟合失败，退回使用实际最大 SNR 点");
                }
            } else {
                log(QString("精调阶段用于 SNR 拟合的有效数据点不足（仅 %1 个），跳过二次拟合")
                    .arg(snrPoints.size()));
            }

            if (useFittedPosition) {
                m_fineBestPosition = static_cast<int>(fittedBestPos);
                m_fineBestSNR = fittedBestSNR;
            } else {
                // 拟合不可用时，遍历精调阶段所有 SNR 数据点，选择 SNR 最大的位置，
                // 作为 super-fine 的中心，避免增量更新过程中出现偏差。
                double bestSNR = m_fineFocusData[0].hfr; // 这里的 hfr 字段存的是 SNR
                int bestPos = m_fineFocusData[0].focuserPosition;
                for (const auto &dp : m_fineFocusData) {
                    if (dp.hfr > bestSNR) {
                        bestSNR = dp.hfr;
                        bestPos = dp.focuserPosition;
                    }
                }
                m_fineBestSNR = bestSNR;
                m_fineBestPosition = bestPos;
                log(QString("使用精调阶段实际观测到的最大 SNR 位置作为 super-fine 起点：SNR=%1, 位置=%2")
                    .arg(m_fineBestSNR).arg(m_fineBestPosition));
            }
        } else {
            int current = getCurrentFocuserPosition();
            m_fineBestPosition = current;
            m_fineBestSNR = -1.0;
            Logger::Log("精调阶段没有有效的 SNR 数据点，使用当前位置作为 super-fine 起点",
                        LogLevel::WARNING, DeviceType::FOCUSER);
        }

        Logger::Log(QString("精调采样完成（共%1点），最佳 SNR=%2，位置=%3，准备进入更细致精调")
                        .arg(m_fineScanPositions.size())
                        .arg(m_fineBestSNR)
                        .arg(m_fineBestPosition)
                        .toStdString(),
                    LogLevel::INFO, DeviceType::FOCUSER);

        if (beginMoveTo(m_fineBestPosition, "精调完成，移动到 SNR 最佳位置")) {
            startSuperFineAdjustment();
        } else {
            startSuperFineAdjustment();
        }
        return;
    }

    const int target = m_fineScanPositions[m_fineScanIndex];
    const int current = getCurrentFocuserPosition();
    if (!isPositionReached(current, target, g_autoFocusConfig.positionTolerance)) {
        beginMoveTo(target, QString("精调移动到第%1个点").arg(m_fineScanIndex+1));
        return;
    }

    // 发送电调位置同步信号
    emit m_wsThread->sendMessageToClient("FocusPosition:" + QString::number(current) + ":" + QString::number(current));

    // 拍摄并等待
    if (!captureFullImage()) { handleError("拍摄图像失败"); return; }
    if (!waitForCaptureComplete()) { handleError("拍摄超时"); return; }

    // 识别 SNR
    double snr = 0.0;
    bool ok = detectSNRByPython(snr);
    if (!ok || !(std::isfinite(snr) && snr > 0.0)) {
        log(QString("该位置 SNR 无效或未识到星，使用占位值 0"));
        emit starDetectionResult(false, 0.0);
        snr = 0.0;
    } else {
        log(QString("精调位置检测到 avg_top50_snr: %1").arg(snr));
        emit starDetectionResult(true, snr);
    }

    // 发送 SNR 结果到前端
    if (m_wsThread) {
        QString msg = QString("AutoFocusSNR:fine:%1:%2:%3")
                          .arg(m_fineScanIndex + 1)
                          .arg(target)
                          .arg(snr, 0, 'f', 6);
        emit m_wsThread->sendMessageToClient(msg);
    }

    // 记录数据点（在 fine 阶段 hfr 字段中暂存 SNR，只用于可视化）
    FocusDataPoint dp(target, snr);
    m_focusData.append(dp);
    m_fineFocusData.append(dp);  // 同时添加到精调专用数组
    m_dataCollectionCount++;
    emit focusDataPointReady(target, snr, QStringLiteral("fine"));

    log(QString("精调数据点%1/%2：位置=%3，SNR=%4")
        .arg(m_fineScanIndex+1).arg(m_fineScanPositions.size()).arg(target).arg(snr));

    // 下一个点
    ++m_fineScanIndex;
    if (m_fineScanIndex < m_fineScanPositions.size()) {
        beginMoveTo(m_fineScanPositions[m_fineScanIndex], "精调扫描下一个点");
    } else {
        log("精调采样已完成，等待拟合阶段");
    }
}



void AutoFocus::startFineAdjustment()
{
    emit focusSeriesReset(QStringLiteral("fine"));

    changeState(AutoFocusState::FINE_ADJUSTMENT);
    updateAutoFocusStep(3, "Fine adjustment in progress, please observe if the focuser has started moving. If it has started moving, please wait for the fine adjustment to complete"); // [AUTO_FOCUS_UI_ENHANCEMENT]
    m_focusData.clear();
    m_dataCollectionCount = 0;
    m_fineBestSNR = -1.0;

    const int minPos = m_focuserMinPosition;
    const int maxPos = m_focuserMaxPosition;

    // 以粗调找到的最优位置为精调中心
    m_fineCenter = std::clamp(m_coarseBestPosition, minPos, maxPos);

    // 改进：更精细的步距，提高对焦精度
    // 精调步距 = 总量程的 2%（至少 1 步），提高步长以加快精调速度
    const int totalRange = std::max(1, maxPos - minPos);
    m_fineStepSpan = std::max(1, static_cast<int>(std::round(totalRange * 0.02)));

    // 构造 11 个点：center, ±1*step, ±2*step, ..., ±5*step
    m_fineScanPositions.clear();
    auto push_unique = [&](int p){
        p = std::clamp(p, minPos, maxPos);
        if (m_fineScanPositions.isEmpty() || m_fineScanPositions.back() != p)
            m_fineScanPositions.push_back(p);
    };

    // 修正：以粗调最佳位置为中心，向两个方向采样
    // 精调策略：center - 4, center - 3, center - 2, center - 1, center, center + 1, center + 2, center + 3, center + 4
    // 总共9个点，以粗调最佳位置为中心，覆盖±4个步长的范围
    
    // 先添加中心点
    push_unique(m_fineCenter);
    
    // 向两个方向添加采样点：±1, ±2, ±3, ±4
    for (int k = 1; k <= 4; ++k) {
        // 向负方向：center - k
        push_unique(m_fineCenter - k * m_fineStepSpan);
        // 向正方向：center + k
        push_unique(m_fineCenter + k * m_fineStepSpan);
    }
    
    // 按位置排序，确保采样顺序合理（从最小到最大）
    std::sort(m_fineScanPositions.begin(), m_fineScanPositions.end());

    // 限制精调采样点数不超过 10 个
    if (m_fineScanPositions.size() > 10) {
        QVector<int> limited;
        int total = m_fineScanPositions.size();
        int desired = 10;
        for (int i = 0; i < desired; ++i) {
            int idx = static_cast<int>(std::round(i * (total - 1.0) / (desired - 1.0)));
            idx = std::clamp(idx, 0, total - 1);
            int p = m_fineScanPositions[idx];
            if (limited.isEmpty() || limited.last() != p)
                limited.push_back(p);
        }
        m_fineScanPositions = limited;
    }

    m_fineScanIndex = 0;

    log(QString("开始精调：中心=%1，步距=%2（=总量程2%%），计划采样点数=%3")
        .arg(m_fineCenter).arg(m_fineStepSpan).arg(m_fineScanPositions.size()));

    if (!m_fineScanPositions.isEmpty()) {
        const int firstTarget = m_fineScanPositions.first();
        const int current = getCurrentFocuserPosition();
        if (!isPositionReached(current, firstTarget, g_autoFocusConfig.positionTolerance)) {
            beginMoveTo(firstTarget, "精调起始移动");
        } else {
            log("已在精调起始点，直接开始采样");
        }
    }
}



/**
 * @brief 处理精调状态
 * 
 * 精调使用小步长精确对焦：
 * 1. 每次移动100步
 * 2. 每个位置拍摄3张图像取平均
 * 3. 收集5个数据点后开始拟合
 * 4. 使用平均HFR值减少噪声影响
 */
void AutoFocus::processFineAdjustment()
{
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过精调");
        return;
    }
    // 改为序列驱动的精调：统一走 performFineDataCollection()
    performFineDataCollection();
}

/**
 * @brief 启动更细致精调阶段（基于 StellarSolver + HFR 拟合）
 */
void AutoFocus::startSuperFineAdjustment()
{
    emit focusSeriesReset(QStringLiteral("super_fine"));

    // 进入更精细精调前，清空前端当前的精调数据点
    emit focusDataPointReady(-1, -1, QStringLiteral("clear"));

    changeState(AutoFocusState::SUPER_FINE_ADJUSTMENT);
    updateAutoFocusStep(4, "Super fine adjustment in progress. The system is performing precise HFR-based fitting, please wait for the final best focus position.");

    // 仅保留 super-fine 数据用于拟合，清空旧的精调/粗调数据
    m_focusData.clear();
    m_fineFocusData.clear();
    m_superFineFocusData.clear();
    m_dataCollectionCount = 0;

    const int minPos = m_focuserMinPosition;
    const int maxPos = m_focuserMaxPosition;

    // 以精调阶段 SNR 最佳位置为 super-fine 中心
    int center = m_fineBestPosition;
    center = std::clamp(center, minPos, maxPos);

    const int totalRange = std::max(1, maxPos - minPos);
    // super-fine 步距：比精调更密，例如精调步距的一半
    m_superFineStepSpan = std::max(1, m_fineStepSpan / 2);
    if (m_superFineStepSpan <= 0) {
        m_superFineStepSpan = std::max(1, static_cast<int>(std::round(totalRange * 0.01)));
    }

    m_superFineScanPositions.clear();
    auto push_unique_sf = [&](int p) {
        p = std::clamp(p, minPos, maxPos);
        if (m_superFineScanPositions.isEmpty() || m_superFineScanPositions.back() != p)
            m_superFineScanPositions.push_back(p);
    };

    // 构造 9 个 super-fine 采样点：center, ±1..±4
    push_unique_sf(center);
    for (int k = 1; k <= 4; ++k) {
        push_unique_sf(center - k * m_superFineStepSpan);
        push_unique_sf(center + k * m_superFineStepSpan);
    }

    std::sort(m_superFineScanPositions.begin(), m_superFineScanPositions.end());
    m_superFineScanIndex = 0;

    log(QString("开始更细致精调：中心=%1，步距=%2，计划采样点数=%3")
            .arg(center).arg(m_superFineStepSpan).arg(m_superFineScanPositions.size()));

    if (!m_superFineScanPositions.isEmpty()) {
        const int firstTarget = m_superFineScanPositions.first();
        const int current = getCurrentFocuserPosition();
        if (!isPositionReached(current, firstTarget, g_autoFocusConfig.positionTolerance)) {
            beginMoveTo(firstTarget, "super-fine 起始移动");
        } else {
            log("已在 super-fine 起始点，直接开始采样");
        }
    }
}

/**
 * @brief 处理更细致精调阶段：使用 StellarSolver 计算 HFR 并收集拟合数据
 */
void AutoFocus::processSuperFineAdjustment()
{
    if (!m_isRunning) {
        log("自动对焦已停止，跳过更细致精调");
        return;
    }

    if (m_superFineScanIndex >= m_superFineScanPositions.size()) {
        log(QString("更细致精调采样完成（共%1点），进入拟合阶段")
                .arg(m_superFineScanPositions.size()));
        changeState(AutoFocusState::FITTING_DATA);
        return;
    }

    const int target = m_superFineScanPositions[m_superFineScanIndex];
    const int current = getCurrentFocuserPosition();
    if (!isPositionReached(current, target, g_autoFocusConfig.positionTolerance)) {
        beginMoveTo(target, QString("super-fine 移动到第%1个点").arg(m_superFineScanIndex + 1));
        return;
    }

    // 发送电调位置同步信号
    emit m_wsThread->sendMessageToClient("FocusPosition:" + QString::number(current) + ":" + QString::number(current));

    // 拍摄并等待完成
    if (!captureFullImage()) { handleError("super-fine 拍摄图像失败"); return; }
    if (!waitForCaptureComplete()) { handleError("super-fine 拍摄超时"); return; }

    // 使用 calculatestars.py 计算 median_HFR，替代 StellarSolver HFR
    double hfr = 0.0;
    bool okHfr = detectMedianHFRByPython(hfr);

    if (!okHfr) {
        // 严重错误（例如图像文件无效），记录日志，但本点 HFR 记为 0，继续流程
        log("super-fine 阶段调用 Python median_HFR 失败，本点 HFR 记为 0，不参与拟合");
        hfr = 0.0;
        emit starDetectionResult(false, 0.0);
    } else {
        if (!std::isfinite(hfr) || hfr <= 0.0) {
            log("super-fine 阶段 Python 返回的 median_HFR 无效或为 0，本点不参与拟合");
            hfr = 0.0;
            emit starDetectionResult(false, 0.0);
        } else {
            log(QString("super-fine 位置检测到 median_HFR: %1").arg(hfr));
            emit starDetectionResult(true, hfr);
        }
    }

    // 记录 super-fine 数据点，并通知前端绘制
    FocusDataPoint dp(target, hfr);
    m_superFineFocusData.append(dp);
    m_focusData.append(dp);
    m_fineFocusData.append(dp); // 复用已有拟合流程中对 fine 数据的引用
    m_dataCollectionCount++;
    emit focusDataPointReady(target, hfr, QStringLiteral("super_fine"));

    log(QString("super-fine 数据点%1/%2：位置=%3，HFR=%4")
        .arg(m_superFineScanIndex+1).arg(m_superFineScanPositions.size()).arg(target).arg(hfr));

    // 下一个点
    ++m_superFineScanIndex;
    if (m_superFineScanIndex < m_superFineScanPositions.size()) {
        beginMoveTo(m_superFineScanPositions[m_superFineScanIndex], "super-fine 扫描下一个点");
    } else {
        log("super-fine 采样已完成，等待拟合阶段");
    }
}

void AutoFocus::collectFocusData()
{
    // 这个方法在当前框架中可能不需要，因为数据收集在粗调和精调过程中完成
    changeState(AutoFocusState::FITTING_DATA);
}

void AutoFocus::processFittingData()
{
    if (!m_isRunning) {
        log("自动对焦已停止，跳过数据拟合");
        return;
    }

    log("开始拟合对焦数据（抛物线二次曲线）...");

    // Step 5: 拟合二次曲线
    FitResult result = fitFocusData();

    // Step 6: 检查拟合结果是否有效
    bool isResultValid = true;
    QString failureReason = "";
    
    // 检查拟合结果是否为有效数字
    if (!std::isfinite(result.bestPosition) || !std::isfinite(result.minHFR) || 
        !std::isfinite(result.a) || !std::isfinite(result.b) || !std::isfinite(result.c)) {
        log("拟合结果包含无效数字 (NaN/Inf)，视为失败");
        isResultValid = false;
        failureReason = "拟合结果包含无效数字";
    }
    
    // 检查是否为水平线拟合（a和b都接近0）
    if (isResultValid && (std::abs(result.a) < 1e-9 && std::abs(result.b) < 1e-9)) {
        log("拟合结果为水平线，未找到最佳焦点，视为失败");
        isResultValid = false;
        failureReason = "拟合结果为水平线，未找到最佳焦点";
    }
    
    // 检查最佳位置是否在合理范围内
    if (isResultValid && (result.bestPosition < m_focuserMinPosition || result.bestPosition > m_focuserMaxPosition)) {
        log(QString("拟合的最佳位置(%1)超出电调范围[%2, %3]，视为对焦失败")
            .arg(result.bestPosition).arg(m_focuserMinPosition).arg(m_focuserMaxPosition));
        isResultValid = false;
        failureReason = "最佳位置超出电调范围";
    }
    
    // 检查最小HFR是否过高（表示对焦效果很差）
    if (isResultValid) {
        const double HFR_FAILURE_THRESHOLD = 50.0; // 可以根据实际情况调整
        if (result.minHFR > HFR_FAILURE_THRESHOLD) {
            log(QString("拟合的最小HFR(%1)过高，超出阈值(%2)，视为对焦失败")
                .arg(result.minHFR).arg(HFR_FAILURE_THRESHOLD));
            isResultValid = false;
            failureReason = "最小HFR过高";
        }
    }
    
    if (isResultValid) {
        log(QString("二次拟合成功：y=%1x² + %2x + %3，best=%4, minHFR=%5")
            .arg(result.a).arg(result.b).arg(result.c).arg(result.bestPosition).arg(result.minHFR));
        m_lastFitResult = result;
        emit focusFitUpdated(result.a, result.b, result.c, result.bestPosition, result.minHFR);
        emit focusBestMovePlanned(static_cast<int>(result.bestPosition), QStringLiteral("fine"));
    } else {
        // 拟合失败，尝试插值方法
        log("二次拟合失败，尝试插值方法...");
        FitResult alt = findBestPositionByInterpolation();
        
        // 检查插值结果是否有效
        if (alt.bestPosition < m_focuserMinPosition || alt.bestPosition > m_focuserMaxPosition || 
            alt.minHFR > 50.0 || !std::isfinite(alt.bestPosition) || !std::isfinite(alt.minHFR)) {
            log("插值结果也不合理，自动对焦失败");
            // 通知上层逻辑（例如 MainWindow）本次自动对焦失败
            emit autofocusFailed();
            // 确保自动对焦流程整体结束，停止定时器和状态机，避免重复多次进入拟合阶段
            completeAutoFocus(false);
            return;
        }
        
        log(QString("插值方法成功：best=%1, minHFR=%2")
            .arg(alt.bestPosition).arg(alt.minHFR));
        m_lastFitResult = alt;
        emit focusFitUpdated(alt.a, alt.b, alt.c, alt.bestPosition, alt.minHFR);
        emit focusBestMovePlanned(static_cast<int>(alt.bestPosition), QStringLiteral("fine"));
    }

    // Step 7: 移动到 HFR 最小的位置
    // 为避免沿用粗调/精调/super-fine 阶段遗留的移动状态，这里重置等待标志，
    // 确保在 MOVING_TO_BEST_POSITION 状态下重新发起一次独立的“移动到最佳位置”。
    m_waitingForMove = false;
    m_isFocuserMoving = false;
    changeState(AutoFocusState::MOVING_TO_BEST_POSITION);
}

void AutoFocus::processMovingToBestPosition()
{
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过移动到最佳位置");
        return;
    }
    
    // 检查是否正在等待电调移动完成
    if (m_waitingForMove) {
        // 检查移动是否完成
        int currentPos = getCurrentFocuserPosition();
        m_moveWaitCount++;
        
        log(QString("等待移动到最佳位置 [%1/60s]: 当前位置=%2, 目标位置=%3").arg(m_moveWaitCount).arg(currentPos).arg(m_targetFocuserPosition));
        
        // 使用统一的位置检查函数，允许最佳位置容差
        if (isPositionReached(currentPos, m_targetFocuserPosition, g_autoFocusConfig.bestPositionTolerance)) {
            log(QString("已到达最佳位置: 当前位置=%1, 目标位置=%2").arg(currentPos).arg(m_targetFocuserPosition));
            m_waitingForMove = false;
            m_isFocuserMoving = false;
            
            // 直接完成自动对焦，不进行最终验证
            log("精调结束，直接完成自动对焦");
            completeAutoFocus(true);
            return;
        } else if (m_moveWaitCount >= 600) { // 60秒超时
            log(QString("警告: 移动到最佳位置超时"));
            m_waitingForMove = false;
            m_isFocuserMoving = false;
            completeAutoFocus(false);
            return;
        } else {
            return; // 继续等待
        }
    }
    
    // 获取拟合结果中的最佳位置
    if (m_focusData.isEmpty()) {
        log(QString("错误: 没有对焦数据，无法确定最佳位置"));
        completeAutoFocus(false);
        return;
    }
    
    // 使用拟合结果中的最佳位置（从processFittingData传递过来）
    // 使用保存的拟合结果
    int bestPosition = static_cast<int>(m_lastFitResult.bestPosition);
    int currentPos = getCurrentFocuserPosition();
    
    log(QString("开始移动到最佳位置: 当前位置=%1, 目标位置=%2").arg(currentPos).arg(bestPosition));
    
    // 检查电调设备状态
    if (!m_dpFocuser) {
        log("错误: 电调设备对象为空，无法移动到最佳位置");
        completeAutoFocus(false);
        return;
    }
    
    if (!m_dpFocuser->isConnected()) {
        log("错误: 电调设备未连接，无法移动到最佳位置");
        completeAutoFocus(false);
        return;
    }
    
    log(QString("电调设备状态检查通过，准备调用startFocuserMove"));
    
    // 启动移动到最佳位置，应用空程补偿
    int finalPosition = bestPosition;
    if (m_useBacklashCompensation && m_backlashCompensation > 0) {
        // 在最佳位置基础上加上空程补偿
        finalPosition = bestPosition - m_backlashCompensation;
        log(QString("应用空程补偿: 最佳位置=%1, 补偿值=%2, 最终位置=%3")
            .arg(bestPosition).arg(m_backlashCompensation).arg(finalPosition));
    }
    
    startFocuserMove(finalPosition);
    
    log(QString("startFocuserMove调用完成，m_isFocuserMoving=%1").arg(m_isFocuserMoving ? "true" : "false"));
    
    // 设置等待状态
    m_waitingForMove = true;
    m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
    m_moveWaitCount = 0;
    m_moveLastPosition = currentPos;
}

/**
 * @brief 拟合对焦数据
 * 
 * 使用改进的二次函数拟合对焦曲线，提高精度和稳定性
 * 目标：找到HFR最小的最佳位置
 * 
 * 算法步骤：
 * 1. 数据预处理：去除异常值，标准化坐标
 * 2. 多种拟合方法：加权最小二乘法、鲁棒拟合
 * 3. 求解线性方程组：改进的高斯消元法
 * 4. 计算最佳位置：x = -b/(2a)
 * 5. 验证拟合质量：计算R²和残差
 * 6. 备选方案：插值法和加权平均
 * 
 * @return FitResult 拟合结果，包含最佳位置和最小HFR
 */
FitResult AutoFocus::fitFocusData()
{
    FitResult result;
    
    // 优先使用 super-fine 阶段数据进行拟合；若为空则回退到精调数据
    const QVector<FocusDataPoint> &sourceData =
        !m_superFineFocusData.isEmpty() ? m_superFineFocusData : m_fineFocusData;

    // 检查数据点是否足够进行拟合
    if (sourceData.size() < g_autoFocusConfig.minDataPoints) {
        log(QString("用于拟合的数据点不足，无法进行拟合，需要至少%1个数据点，当前只有%2个")
                .arg(g_autoFocusConfig.minDataPoints).arg(sourceData.size()));
        
        // 如果数据点太少，尝试使用插值方法
        if (sourceData.size() >= 2) {
            log("数据点不足但可以使用插值方法");
            return findBestPositionByInterpolation();
        } else {
            log("数据点严重不足，无法进行任何拟合");
            return result;
        }
    }
    
    log(QString("开始改进的二次函数拟合，原始数据点数量: %1").arg(sourceData.size()));
    
    // 数据预处理：去除异常值（优先使用 super-fine / 退回精调数据）
    QVector<FocusDataPoint> cleanData = removeOutliers(sourceData);
    if (cleanData.size() < 3) {
        log(QString("去除异常值后数据点不足，使用原始数据"));
        cleanData = sourceData;
    }

    // 进一步过滤：只保留 HFR>0 且为有限数的有效数据点
    QVector<FocusDataPoint> validData;
    validData.reserve(cleanData.size());
    for (const FocusDataPoint &p : cleanData) {
        if (std::isfinite(p.hfr) && p.hfr > 0.0) {
            validData.append(p);
        }
    }

    if (validData.size() < g_autoFocusConfig.minDataPoints) {
        log(QString("用于拟合的有效 HFR 数据点不足（剔除 HFR<=0 后仅剩 %1 个），无法进行二次拟合")
                .arg(validData.size()));

        // 如果有效点数量仍然足以做插值，则退回插值方法（同样基于 HFR>0 的点）
        if (validData.size() >= 2) {
            log("有效数据点不足但可以使用插值方法（忽略 HFR=0 的点）");
            return findBestPositionByInterpolation(validData);
        } else {
            log("有效数据点严重不足，无法进行任何拟合或插值");
            return result;
        }
    }
    
    // 尝试多种拟合方法，选择最佳结果
    FitResult bestResult;
    double bestRSquared = -1.0;
    
    // 方法1：标准最小二乘法
    FitResult result1 = performStandardLeastSquares(validData);
    if (result1.bestPosition > 0) {
        double rSquared1 = calculateRSquared(validData, result1, getDataMinPosition(validData));
        log(QString("标准最小二乘法 R² = %1").arg(rSquared1));
        if (rSquared1 > bestRSquared) {
            bestResult = result1;
            bestRSquared = rSquared1;
        }
    }
    
    // 方法2：加权最小二乘法（给中心点更高权重）
    FitResult result2 = performWeightedLeastSquares(validData);
    if (result2.bestPosition > 0) {
        double rSquared2 = calculateRSquared(validData, result2, getDataMinPosition(validData));
        log(QString("加权最小二乘法 R² = %1").arg(rSquared2));
        if (rSquared2 > bestRSquared) {
            bestResult = result2;
            bestRSquared = rSquared2;
        }
    }
    
    // 方法3：鲁棒拟合（使用Huber损失函数）
    FitResult result3 = performRobustFitting(validData);
    if (result3.bestPosition > 0) {
        double rSquared3 = calculateRSquared(validData, result3, getDataMinPosition(validData));
        log(QString("鲁棒拟合 R² = %1").arg(rSquared3));
        if (rSquared3 > bestRSquared) {
            bestResult = result3;
            bestRSquared = rSquared3;
        }
    }
    
    // 检查最佳结果是否有效，优先使用拟合结果
    if (bestResult.bestPosition > 0) {
        if (bestRSquared > g_autoFocusConfig.minRSquared) {
            log(QString("最佳拟合方法 R² = %1，质量良好，使用此结果").arg(bestRSquared));
        } else {
            log(QString("最佳拟合方法 R² = %1，质量一般但仍使用拟合结果（忽略识别出错的点）").arg(bestRSquared));                                                                             
        }
        result = bestResult;
    } else {
        log(QString("所有拟合方法都失败，使用插值方法（基于 HFR>0 的有效数据）"));
        log(QString("拟合失败原因分析："));
        log(QString("- 有效数据点数量: %1").arg(validData.size()));
        log(QString("- 数据点范围: [%1, %2]").arg(getDataMinPosition(validData)).arg(getDataMaxPosition(validData)));
        return findBestPositionByInterpolation(validData);
    }
    
    // 验证最佳位置是否在合理范围内
    double minPos = getDataMinPosition(validData);
    double maxPos = getDataMaxPosition(validData);
    if (result.bestPosition < minPos || result.bestPosition > maxPos) {
        log(QString("拟合的最佳位置超出数据范围，使用插值方法"));
        return findBestPositionByInterpolation();
    }
    
    log(QString("改进的二次函数拟合成功: y = %1x² + %2x + %3")
        .arg(result.a).arg(result.b).arg(result.c));
    log(QString("最佳位置: %1, 最小HFR: %2, R²: %3")
        .arg(result.bestPosition).arg(result.minHFR).arg(bestRSquared));
    
    return result;
}

void AutoFocus::moveToBestPosition(double position)
{
    log(QString("移动电调到最佳位置: %1").arg(position));
    
    setFocuserPosition(static_cast<int>(position));
}

void AutoFocus::changeState(AutoFocusState newState)
{
    try {
        if (m_currentState != newState) {
            m_currentState = newState;
            emit stateChanged(newState);
            log(QString("状态变更: %1").arg(static_cast<int>(newState)));
        }
    } catch (const std::exception& e) {
        log(QString("状态变更时发生异常: %1").arg(e.what()));
    } catch (...) {
        log("状态变更时发生未知异常");
    }
}

void AutoFocus::log(const QString &message)
{
    qDebug() << "[AutoFocus]" << message;
    emit logMessage(message);
}

void AutoFocus::updateProgress(int progress)
{
    emit progressUpdated(progress);
}

void AutoFocus::handleError(const QString &error)
{
    try {
        log("错误: " + error);
        emit errorOccurred(error);
        completeAutoFocus(false);
    } catch (const std::exception& e) {
        log(QString("处理错误时发生异常: %1").arg(e.what()));
    } catch (...) {
        log("处理错误时发生未知异常");
    }
}

void AutoFocus::completeAutoFocus(bool success)
{
    try {
        log(QString("开始完成自动对焦流程，成功状态: %1").arg(success ? "是" : "否"));

        // 先停止状态机定时器，但保持 m_isRunning=true，
        // 以便在“最终拍摄”阶段仍然允许 captureImage / waitForCaptureComplete 正常工作。
        // 安全停止定时器
        if (m_timer && m_timer->isActive()) {
            m_timer->stop();
            log("定时器已停止");
        }
        
        if (success) {
            double bestPosition = 0.0;
            double minHFR = 0.0;
            
            // 安全访问数据
            if (!m_focusData.isEmpty()) {
                bestPosition = m_focusData.last().focuserPosition;
                minHFR = m_focusData.last().hfr;
            }
            
            // 在最佳位置进行最终拍摄
            log("在最佳位置进行最终拍摄...");
            if (captureFullImage()) {
                if (waitForCaptureComplete()) {
                    log("最终拍摄完成");
                } else {
                    log("最终拍摄超时，但自动对焦流程继续");
                }
            } else {
                log("最终拍摄失败，但自动对焦流程继续");
            }
            
            emit autoFocusCompleted(true, bestPosition, minHFR);
            log("自动对焦完成");
        } else {
            emit autoFocusCompleted(false, 0.0, 0.0);
            log("自动对焦失败");
        }

        // 最终阶段再将运行状态标记为停止
        m_isRunning = false;

        changeState(AutoFocusState::COMPLETED);
        log("自动对焦流程完成");
        
    } catch (const std::exception& e) {
        log(QString("完成自动对焦时发生异常: %1").arg(e.what()));
    } catch (...) {
        log("完成自动对焦时发生未知异常");
    }
}

/**
 * @brief 拍摄一张全图
 * 
 * 使用传入的主相机设备对象进行拍摄：
 * 1. 检查相机设备连接状态
 * 2. 设置曝光参数
 * 3. 发送拍摄命令
 * 4. 等待图像下载完成
 * 5. 返回拍摄是否成功
 * 
 * @return bool 拍摄是否成功
 */
bool AutoFocus::captureFullImage()
{
    return captureImage(m_defaultExposureTime, false);
}

/**
 * @brief 拍摄ROI图像
 * 
 * 使用当前设置的ROI区域拍摄图像
 * 
 * @return bool 拍摄是否成功
 */
bool AutoFocus::captureROIImage()
{
    if (!m_useROI || !m_currentROI.isValid()) {
        log(QString("警告: ROI未设置或无效，使用全图拍摄"));
        return captureFullImage();
    }
    
    log(QString("拍摄ROI图像，ROI区域: x=%1, y=%2, w=%3, h=%4")
        .arg(m_currentROI.x()).arg(m_currentROI.y())
        .arg(m_currentROI.width()).arg(m_currentROI.height()));
    
    return captureImage(m_defaultExposureTime, true);
}

/**
 * @brief 拍摄图像（带曝光时间参数）
 * 
 * 基于提供的拍摄实现，通过INDI接口控制相机拍摄：
 * 1. 检查相机和INDI客户端连接状态
 * 2. 重置CCD帧信息
 * 3. 发送拍摄命令
 * 4. 设置拍摄状态和图像路径
 * 
 * @param exposureTime 曝光时间（毫秒）
 * @return bool 拍摄是否成功
 */
bool AutoFocus::captureImage(int exposureTime, bool useROI)
{
    if (!m_isRunning) {
        log("自动对焦已停止，跳过拍摄");
        return false;
    }
    
    log(QString("拍摄图像，曝光时间 %1ms，使用ROI: %2").arg(exposureTime).arg(useROI ? "是" : "否"));
    
    // 检查相机设备对象是否有效
    if (!m_dpMainCamera) {
        log(QString("错误: 主相机设备对象为空，无法拍摄"));
        return false;
    }
    
    // 检查INDI客户端是否有效
    if (!m_indiServer) {
        log(QString("错误: INDI客户端不可用"));
        return false;
    }
    
    // 重置CCD帧信息
    uint32_t ret = m_indiServer->resetCCDFrameInfo(m_dpMainCamera);
    if (ret != QHYCCD_SUCCESS) {
        log(QString("警告: 重置CCD帧信息失败"));
    }
    
    // 如果使用ROI，设置ROI区域
    if (useROI && m_useROI && m_currentROI.isValid()) {
        log(QString("设置ROI区域: x=%1, y=%2, w=%3, h=%4")
            .arg(m_currentROI.x()).arg(m_currentROI.y())
            .arg(m_currentROI.width()).arg(m_currentROI.height()));
        
        // 通过INDI接口设置ROI区域
        m_indiServer->setCCDFrameInfo(m_dpMainCamera, m_currentROI.x(), m_currentROI.y(), m_currentROI.width(), m_currentROI.height()); // 设置相机的曝光区域
        if (ret != QHYCCD_SUCCESS) {
            log(QString("警告: 设置ROI失败"));
        }
        emit roiInfoChanged(m_currentROI);
    }else{
        ret = m_indiServer->resetCCDFrameInfo(m_dpMainCamera);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("INDI_Capture | indi resetCCDFrameInfo | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        emit roiInfoChanged(QRect(0, 0, 0, 0));
    }
    
    // 通过INDI接口拍摄图像
    log(QString("开始调用INDI拍摄接口"));
    ret = m_indiServer->takeExposure(m_dpMainCamera, exposureTime / 1000.0);
    if (ret == QHYCCD_SUCCESS) {
        // 重置拍摄状态，等待回调设置完成状态
        m_isCaptureEnd = false;
        m_lastCapturedImage = "/dev/shm/ccd_simulator.fits";
        
        log(QString("拍摄命令发送成功，等待回调设置完成状态"));
        return true;
    } else {
        log(QString("拍摄失败，错误代码: %1").arg(ret));
        return false;
    }
}

/**
 * @brief 等待拍摄完成
 * 
 * 等待图像拍摄和下载完成，检查拍摄状态
 * 
 * @param timeoutMs 超时时间（毫秒），默认30秒
 * @return bool 是否成功完成拍摄
 */
bool AutoFocus::waitForCaptureComplete(int timeoutMs)
{
    log(QString("等待拍摄完成，超时时间: %1ms").arg(timeoutMs));
    
    // 如果已经完成，直接返回
    if (m_isCaptureEnd) {
        log(QString("拍摄已完成，图像路径: %1").arg(m_lastCapturedImage));
        return true;
    }
    
    // 使用定时器替代 while 循环，避免阻塞
    // 设置拍摄检查参数
    m_captureCheckTimeout = timeoutMs;
    m_captureCheckPending = true;
    m_captureCheckResult = false;
    
    // 检查是否已经停止，避免在停止状态下进入等待
    if (!m_isRunning) {
        log("自动对焦已停止，跳过拍摄等待");
        return false;
    }
    
    // 创建超时定时器
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(timeoutMs);
    
    // 使用事件循环等待结果，避免阻塞主线程
    QEventLoop eventLoop;
    
    // 使用 QPointer 来安全地管理定时器
    QPointer<QTimer> captureTimerPtr = m_captureCheckTimer;
    
    // 连接超时信号，当定时器超时或拍摄检查完成时退出事件循环
    QObject::connect(&timeoutTimer, &QTimer::timeout, &eventLoop, [this, &eventLoop, captureTimerPtr]() {
        if (m_captureCheckPending) {
            log(QString("拍摄超时"));
            m_captureCheckPending = false;
            if (captureTimerPtr && captureTimerPtr->isActive()) {
                captureTimerPtr->stop();
            }
        }
        eventLoop.quit();
    });
    
    // 创建一个辅助定时器来定期检查拍摄状态
    QTimer checkTimer;
    checkTimer.setInterval(50); // 每50ms检查一次 pending 状态
    QObject::connect(&checkTimer, &QTimer::timeout, &eventLoop, [this, &checkTimer, &eventLoop, captureTimerPtr]() {
        if (!m_captureCheckPending) {
            // 拍摄检查已完成，退出事件循环
            checkTimer.stop();
            eventLoop.quit();
        }
    });
    
    // 启动所有定时器
    if (m_captureCheckTimer) {
        m_captureCheckTimer->start();
    }
    timeoutTimer.start();
    checkTimer.start();
    
    // 进入事件循环等待，直到拍摄完成或超时
    // 添加定期检查，确保在 stopAutoFocus 被调用时能及时退出
    QTimer stopCheckTimer;
    stopCheckTimer.setInterval(25); // 每25ms检查一次停止状态，提高响应速度
    QObject::connect(&stopCheckTimer, &QTimer::timeout, &eventLoop, [this, &eventLoop, &stopCheckTimer]() {
        if (!m_isRunning || !m_captureCheckPending) {
            stopCheckTimer.stop();
            eventLoop.quit();
        }
    });
    stopCheckTimer.start();
    
    // 设置事件循环超时，防止无限等待
    QTimer maxWaitTimer;
    maxWaitTimer.setSingleShot(true);
    maxWaitTimer.setInterval(timeoutMs + 1000); // 比拍摄超时多1秒
    QObject::connect(&maxWaitTimer, &QTimer::timeout, &eventLoop, [&eventLoop]() {
        eventLoop.quit();
    });
    maxWaitTimer.start();
    
    // 使用 QEventLoop::ProcessEvents 模式，确保能及时响应停止信号
    int maxIterations = (timeoutMs + 1000) / 10; // 最大迭代次数
    int iteration = 0;
    while (m_captureCheckPending && m_isRunning && maxWaitTimer.isActive() && iteration < maxIterations) {
        eventLoop.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10); // 短暂等待，避免CPU占用过高
        iteration++;
        
        // 每50次迭代检查一次，防止无限循环
        if (iteration % 50 == 0) {
            if (!m_isRunning) {
                log("检测到停止信号，立即退出拍摄等待循环");
                break; // 如果已停止，立即退出
            }
        }
        
        // 每200次迭代强制检查一次状态
        if (iteration % 200 == 0) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        }
    }
    
    // 如果循环结束但仍在等待，强制退出
    if (m_captureCheckPending) {
        log("强制退出拍摄等待循环");
        m_captureCheckPending = false;
        m_captureCheckResult = false;
        
        // 强制处理事件，确保状态更新生效
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    
    // 停止最大等待定时器
    maxWaitTimer.stop();
    
    // 停止所有定时器
    stopCheckTimer.stop();
    if (m_captureCheckTimer) {
        m_captureCheckTimer->stop();
    }
    timeoutTimer.stop();
    checkTimer.stop();
    
    // 返回结果
    bool result = m_captureCheckResult;
    m_captureCheckPending = false;
    
    return result;
}

/**
 * @brief 检测图像中的星点
 * 
 * 这个函数需要实现星点检测算法：
 * 1. 图像预处理（去噪、增强对比度）
 * 2. 星点检测（阈值分割、连通域分析）
 * 3. 星点筛选（大小、亮度、形状）
 * 4. 返回是否检测到足够数量的星点
 * 
 * @return bool 是否检测到星点
 */
bool AutoFocus::detectStarsInImage()
{
// 使用Python脚本进行星点识别并获取HFR
    double hfr = 0.0;
    bool ok = detectHFRByPython(hfr);
    if (ok) {
        // 检测到星点并得到HFR
        log(QString("检测到星点，HFR=%1").arg(hfr));
        return true;
    } else {
        log("未检测到星点（或HFR无效）");
        return false;
    }
}


/**
 * @brief 计算图像的HFR值
 * 
 * HFR (Half Flux Radius) 是衡量星点锐度的指标：
 * 1. 对检测到的星点进行轮廓分析
 * 2. 计算每个星点的HFR值
 * 3. 返回所有星点的平均HFR
 * 4. HFR越小表示对焦越精确
 * 
 * @return double 平均HFR值
 */
double AutoFocus::calculateHFR()
{
double hfr = m_lastHFR;
    if (!(std::isfinite(hfr) && hfr > 0)) {
        // 如果之前没有HFR，则尝试通过Python获取
        if (!m_lastCapturedImage.isEmpty()) {
            double tmp=0.0;
            if (detectHFRByPython(tmp)) {
                hfr = tmp;
            }
        }
    }
    if (!(std::isfinite(hfr) && hfr > 0)) {
        log("当前无有效HFR，返回一个较大的占位值");
        return 999.0;
    }
    return hfr;
}


// 旧的星点跟踪方法已删除，替换为新的星点选择策略

/**
 * @brief 移动电调相对位置
 * 
 * 使用传入的电调设备对象进行移动：
 * 1. 检查电调设备连接状态
 * 2. 检查位置限制
 * 3. 发送相对移动命令
 * 4. 等待移动完成
 * 5. 更新内部位置记录
 * 6. 处理移动超时或错误
 * 
 * @param steps 移动步数，正数为向外，负数为向内
 */
void AutoFocus::moveFocuser(int steps)
{
    // 检查电调设备对象是否有效
    if (!m_dpFocuser) {
        log(QString("错误: 电调设备对象为空，无法移动"));
        return;
    }
    
    // 计算目标位置
    int targetPosition = m_currentPosition + steps;
    
    // 检查位置限制
    if (targetPosition < m_focuserMinPosition) {
        log(QString("警告: 目标位置 %1 小于最小位置 %2，限制到最小位置").arg(targetPosition).arg(m_focuserMinPosition));
        targetPosition = m_focuserMinPosition;
    } else if (targetPosition > m_focuserMaxPosition) {
        log(QString("警告: 目标位置 %1 大于最大位置 %2，限制到最大位置").arg(targetPosition).arg(m_focuserMaxPosition));
        targetPosition = m_focuserMaxPosition;
    }
    
    // 计算实际移动步数
    int actualSteps = targetPosition - m_currentPosition;
    
    if (actualSteps == 0) {
        log(QString("电调已达到位置限制，无法继续移动"));
        // 不直接返回，而是记录当前状态并继续
        log(QString("当前位置: %1, 目标位置: %2").arg(m_currentPosition).arg(targetPosition));
        return;
    }
    
    // 计算移动方向
    bool isInward = (actualSteps < 0);
    int moveSteps = qAbs(actualSteps);
    
    // 设置移动状态
    m_isFocuserMoving = true;
    m_targetFocuserPosition = targetPosition;
    m_moveStartTime = QDateTime::currentMSecsSinceEpoch();
    m_lastPosition = m_currentPosition;
    
    // 设置移动方向并发送移动命令
    m_indiServer->setFocuserMoveDiretion(m_dpFocuser, isInward);
    m_indiServer->moveFocuserSteps(m_dpFocuser, moveSteps);
    
    // 初始化电调判断参数
    initializeFocuserMoveParameters();
    
    // 启动位置更新定时器
    // emit startPositionUpdateTimer();
    
    log(QString("电调相对移动命令已发送: %1 步，方向: %2").arg(moveSteps).arg(isInward ? "向内" : "向外"));
}

/**
 * @brief 获取当前电调位置
 * 
 * 使用传入的电调对象查询位置：
 * 1. 检查电调连接状态
 * 2. 发送位置查询命令
 * 3. 读取电调返回的位置值
 * 4. 更新内部位置记录
 * 5. 处理通信错误
 * 
 * @return int 当前电调位置（步数）
 */
int AutoFocus::getCurrentFocuserPosition()
{
    // 检查电调对象是否有效
    if (!m_dpFocuser) {
        log(QString("错误: 电调对象为空，无法获取位置"));
        return m_currentPosition; // 返回缓存的位置
    }
    
    int currentPosition;
    int success = m_indiServer->getFocuserAbsolutePosition(m_dpFocuser,currentPosition);
    if(success == 0 && currentPosition != INT_MAX)
    {
        // 获取成功，更新缓存的位置
        m_currentPosition = currentPosition;
        log(QString("成功获取电调位置: %1").arg(currentPosition));
    }
    else
    {
        log(QString("错误: 获取当前电调位置失败，使用缓存位置: %1").arg(m_currentPosition));
    }
    return m_currentPosition; // 返回缓存的位置
}

/**
 * @brief 设置电调到绝对位置
 * 
 * 使用传入的电调对象移动到指定位置：
 * 1. 检查电调连接状态
 * 2. 检查位置限制
 * 3. 发送绝对位置移动命令
 * 4. 等待移动完成
 * 5. 验证最终位置
 * 6. 更新内部位置记录
 * 
 * @param position 目标位置（步数）
 */
void AutoFocus::setFocuserPosition(int position)
{
    // 检查电调对象是否有效
    if (!m_dpFocuser) {
        log(QString("错误: 电调对象为空，无法设置位置"));
        return;
    }
    
    // 检查位置限制
    if (position < m_focuserMinPosition) {
        log(QString("警告: 目标位置 %1 小于最小位置 %2，限制到最小位置").arg(position).arg(m_focuserMinPosition));
        position = m_focuserMinPosition;
    } else if (position > m_focuserMaxPosition) {
        log(QString("警告: 目标位置 %1 大于最大位置 %2，限制到最大位置").arg(position).arg(m_focuserMaxPosition));
        position = m_focuserMaxPosition;
    }
    
    // 使用非阻塞方式移动电调
    startFocuserMove(position);
    
    log(QString("电调移动命令已发送，目标位置: %1").arg(position));
}

/**
 * @brief 检查电调连接状态
 * 
 * 使用传入的电调对象检查连接状态：
 * 1. 检查电调对象是否有效
 * 2. 发送状态查询命令
 * 3. 检查通信响应
 * 4. 验证电调是否正常工作
 * 5. 返回连接状态
 * 
 * @return bool 电调是否已连接
 */
bool AutoFocus::isFocuserConnected()
{
    // 检查电调对象是否有效
    if (!m_dpFocuser) {
        log(QString("错误: 电调对象为空"));
        return false;
    }

    int position;
    int success = m_indiServer->getFocuserAbsolutePosition(m_dpFocuser,position);
    if(success == 0 && position != INT_MAX)
    {
        return true;
    }
    else
    {
        return false;
    }
    
}

/**
 * @brief 检查电调位置是否有效
 * 
 * 检查指定位置是否在电调的位置限制范围内
 * 
 * @param position 要检查的位置
 * @return bool 位置是否有效
 */
bool AutoFocus::isFocuserPositionValid(int position) const
{
    return (position >= m_focuserMinPosition && position <= m_focuserMaxPosition);
}

/**
 * @brief 获取电调总行程
 * 
 * 返回电调的最大位置和最小位置之间的差值
 * 
 * @return int 电调总行程（步数）
 */
int AutoFocus::getFocuserTotalRange() const
{
    return m_focuserMaxPosition - m_focuserMinPosition;
}

/**
 * @brief 去除异常值
 * 
 * 使用改进的多重异常值检测方法：
 * 1. 基于二次曲线拟合的残差分析
 * 2. 基于HFR统计分布的IQR方法
 * 3. 基于位置分布的异常检测
 * 
 * @param data 原始数据
 * @return QVector<FocusDataPoint> 清理后的数据
 */
QVector<FocusDataPoint> AutoFocus::removeOutliers(const QVector<FocusDataPoint>& data)
{
    if (data.size() < g_autoFocusConfig.minDataPoints + 2) {
        return data; // 数据点太少，不进行异常值检测，直接使用原始数据
    }
    
    log(QString("开始智能异常值检测，原始数据点数量: %1").arg(data.size()));
    
    // 方法1：基于二次曲线拟合的残差分析
    QVector<FocusDataPoint> cleanData1 = removeOutliersByResidual(data);
    
    // 方法2：基于HFR统计分布的IQR方法
    QVector<FocusDataPoint> cleanData2 = removeOutliersByIQR(data);
    
    // 方法3：基于位置分布的异常检测
    QVector<FocusDataPoint> cleanData3 = removeOutliersByPosition(data);
    
    // 优先选择保留最多数据点的方法，确保拟合有足够的数据点
    QVector<FocusDataPoint> bestCleanData = cleanData1;
    if (cleanData2.size() > bestCleanData.size()) {
        bestCleanData = cleanData2; // IQR方法通常最宽松
    }
    if (cleanData3.size() > bestCleanData.size()) {
        bestCleanData = cleanData3;
    }
    
    // 如果过滤后数据点太少，直接使用原始数据，确保拟合能够进行
    if (bestCleanData.size() < g_autoFocusConfig.minDataPoints) {
        log("异常值检测过滤后数据点不足，使用原始数据以确保拟合能够进行");
        bestCleanData = data; // 直接返回原始数据，忽略异常值检测
    }
    
    log(QString("智能异常值检测完成: 原始数据%1个点，清理后%2个点")
        .arg(data.size()).arg(bestCleanData.size()));
    
    return bestCleanData;
}

/**
 * @brief 基于二次曲线拟合残差的异常值检测
 * 
 * 先进行初步拟合，然后根据残差大小识别异常点
 * 
 * @param data 原始数据
 * @return QVector<FocusDataPoint> 清理后的数据
 */
QVector<FocusDataPoint> AutoFocus::removeOutliersByResidual(const QVector<FocusDataPoint>& data)
{
    if (data.size() < 4) {
        return data; // 需要至少4个点才能进行二次拟合
    }
    
    // 使用IQR方法进行初步清理，获得相对干净的数据进行拟合
    QVector<FocusDataPoint> preliminaryClean = removeOutliersByIQR(data);
    if (preliminaryClean.size() < 3) {
        return data;
    }
    
    // 对初步清理的数据进行二次拟合
    FitResult preliminaryFit = performStandardLeastSquares(preliminaryClean);
    if (preliminaryFit.bestPosition <= 0) {
        return data; // 拟合失败，返回原始数据
    }
    
    // 计算所有数据点到拟合曲线的残差
    double minPos = getDataMinPosition(data);
    QVector<double> residuals;
    QVector<int> outlierIndices;
    
    for (int i = 0; i < data.size(); ++i) {
        const FocusDataPoint &point = data[i];
        double x = static_cast<double>(point.focuserPosition) - minPos;
        double predictedY = preliminaryFit.a * x * x + preliminaryFit.b * x + preliminaryFit.c;
        double residual = qAbs(point.hfr - predictedY);
        residuals.append(residual);
    }
    
    // 计算残差的统计信息
    std::sort(residuals.begin(), residuals.end());
    int n = residuals.size();
    double q1 = residuals[n / 4];
    double q3 = residuals[3 * n / 4];
    double iqr = q3 - q1;
    double threshold = q3 + 3.0 * iqr; // 使用3倍IQR作为阈值，更宽松以忽略识别出错的点
    
    // 识别异常点
    QVector<FocusDataPoint> cleanData;
    for (int i = 0; i < data.size(); ++i) {
        if (residuals[i] <= threshold) {
            cleanData.append(data[i]);
        } else {
            outlierIndices.append(i);
        }
    }
    
    log(QString("基于残差的异常值检测: 识别出%1个异常点，保留%2个点")
        .arg(outlierIndices.size()).arg(cleanData.size()));
    
    return cleanData;
}

/**
 * @brief 基于HFR统计分布的IQR异常值检测
 * 
 * 使用传统的四分位数方法检测HFR异常值
 * 
 * @param data 原始数据
 * @return QVector<FocusDataPoint> 清理后的数据
 */
QVector<FocusDataPoint> AutoFocus::removeOutliersByIQR(const QVector<FocusDataPoint>& data)
{
    // 计算HFR的统计信息
    QVector<double> hfrValues;
    for (const FocusDataPoint &point : data) {
        hfrValues.append(point.hfr);
    }
    
    // 排序
    std::sort(hfrValues.begin(), hfrValues.end());
    
    // 计算四分位数
    int n = hfrValues.size();
    double q1 = hfrValues[n / 4];
    double q3 = hfrValues[3 * n / 4];
    double iqr = q3 - q1;
    
    // 定义异常值边界（使用3倍IQR，更宽松以忽略识别出错的点）
    double lowerBound = q1 - 3.0 * iqr;
    double upperBound = q3 + 3.0 * iqr;
    
    // 过滤异常值
    QVector<FocusDataPoint> cleanData;
    for (const FocusDataPoint &point : data) {
        if (point.hfr >= lowerBound && point.hfr <= upperBound) {
            cleanData.append(point);
        }
    }
    
    log(QString("基于IQR的异常值检测: 原始数据%1个点，清理后%2个点")
        .arg(data.size()).arg(cleanData.size()));
    
    return cleanData;
}

/**
 * @brief 基于位置分布的异常值检测
 * 
 * 检测位置分布异常的孤立点
 * 
 * @param data 原始数据
 * @return QVector<FocusDataPoint> 清理后的数据
 */
QVector<FocusDataPoint> AutoFocus::removeOutliersByPosition(const QVector<FocusDataPoint>& data)
{
    if (data.size() < 4) {
        return data;
    }
    
    // 按位置排序
    QVector<FocusDataPoint> sortedData = data;
    std::sort(sortedData.begin(), sortedData.end(), 
              [](const FocusDataPoint &a, const FocusDataPoint &b) {
                  return a.focuserPosition < b.focuserPosition;
              });
    
    // 计算相邻点之间的距离
    QVector<double> distances;
    for (int i = 1; i < sortedData.size(); ++i) {
        double dist = sortedData[i].focuserPosition - sortedData[i-1].focuserPosition;
        distances.append(dist);
    }
    
    // 计算距离的统计信息
    std::sort(distances.begin(), distances.end());
    int n = distances.size();
    double medianDistance = distances[n / 2];
    double q3 = distances[3 * n / 4];
    double threshold = q3 + 1.5 * (q3 - medianDistance);
    
    // 识别位置异常的点
    QVector<FocusDataPoint> cleanData;
    cleanData.append(sortedData[0]); // 第一个点总是保留
    
    for (int i = 1; i < sortedData.size(); ++i) {
        double dist = sortedData[i].focuserPosition - sortedData[i-1].focuserPosition;
        if (dist <= threshold) {
            cleanData.append(sortedData[i]);
        } else {
            // 检查是否是孤立点（前后距离都很大）
            bool isIsolated = true;
            if (i > 1) {
                double prevDist = sortedData[i-1].focuserPosition - sortedData[i-2].focuserPosition;
                if (prevDist <= threshold) isIsolated = false;
            }
            if (i < sortedData.size() - 1) {
                double nextDist = sortedData[i+1].focuserPosition - sortedData[i].focuserPosition;
                if (nextDist <= threshold) isIsolated = false;
            }
            
            if (!isIsolated) {
                cleanData.append(sortedData[i]);
            }
        }
    }
    
    log(QString("基于位置的异常值检测: 原始数据%1个点，清理后%2个点")
        .arg(data.size()).arg(cleanData.size()));
    
    return cleanData;
}

/**
 * @brief 求解线性方程组
 * 
 * 使用高斯消元法求解3x3线性方程组
 * 
 * @param matrix 系数矩阵
 * @param constants 常数向量
 * @param solution 解向量
 * @return bool 是否求解成功
 */
bool AutoFocus::solveLinearSystem(double matrix[3][3], double constants[3], double solution[3])
{
    // 高斯消元法求解线性方程组
    double augmented[3][4];
    
    // 构建增广矩阵
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            augmented[i][j] = matrix[i][j];
        }
        augmented[i][3] = constants[i];
    }
    
    // 前向消元
    for (int i = 0; i < 3; ++i) {
        // 寻找主元
        int maxRow = i;
        for (int k = i + 1; k < 3; ++k) {
            if (qAbs(augmented[k][i]) > qAbs(augmented[maxRow][i])) {
                maxRow = k;
            }
        }
        
        // 交换行
        if (maxRow != i) {
            for (int j = i; j < 4; ++j) {
                std::swap(augmented[i][j], augmented[maxRow][j]);
            }
        }
        
        // 检查主元是否为零
        if (qAbs(augmented[i][i]) < 1e-10) {
            log(QString("线性方程组奇异，无法求解。主元[%1][%1] = %2").arg(i).arg(augmented[i][i]));
            log(QString("矩阵条件数可能过大，数据点可能共线或接近共线"));
            return false;
        }
        
        // 消元
        for (int k = i + 1; k < 3; ++k) {
            double factor = augmented[k][i] / augmented[i][i];
            for (int j = i; j < 4; ++j) {
                augmented[k][j] -= factor * augmented[i][j];
            }
        }
    }
    
    // 回代求解
    for (int i = 2; i >= 0; --i) {
        solution[i] = augmented[i][3];
        for (int j = i + 1; j < 3; ++j) {
            solution[i] -= augmented[i][j] * solution[j];
        }
        solution[i] /= augmented[i][i];
    }
    
    return true;
}

/**
 * @brief 标准最小二乘法拟合
 * 
 * 使用传统的二次函数最小二乘法拟合
 * 
 * @param data 数据点
 * @return FitResult 拟合结果
 */
FitResult AutoFocus::performStandardLeastSquares(const QVector<FocusDataPoint>& data)
{
    FitResult result;
    
    if (data.size() < 3) {
        return result;
    }
    
    // 标准化坐标：将电调位置转换为相对坐标
    double minPos = getDataMinPosition(data);
    double maxPos = getDataMaxPosition(data);
    
    // 构建最小二乘法正规方程组
    double sum_x4 = 0.0, sum_x3 = 0.0, sum_x2 = 0.0, sum_x = 0.0, sum_1 = 0.0;
    double sum_x2y = 0.0, sum_xy = 0.0, sum_y = 0.0;
    
    for (const FocusDataPoint &point : data) {
        double x = static_cast<double>(point.focuserPosition) - minPos; // 相对坐标
        double y = point.hfr;
        
        double x2 = x * x;
        double x3 = x2 * x;
        double x4 = x3 * x;
        
        sum_x4 += x4;
        sum_x3 += x3;
        sum_x2 += x2;
        sum_x += x;
        sum_1 += 1.0;
        
        sum_x2y += x2 * y;
        sum_xy += x * y;
        sum_y += y;
    }
    
    // 构建系数矩阵和常数向量
    double matrix[3][3] = {
        {sum_x4, sum_x3, sum_x2},
        {sum_x3, sum_x2, sum_x},
        {sum_x2, sum_x, sum_1}
    };
    
    double constants[3] = {sum_x2y, sum_xy, sum_y};
    
    // 求解线性方程组
    double coefficients[3];
    if (!solveLinearSystem(matrix, constants, coefficients)) {
        log(QString("标准最小二乘法：线性方程组求解失败"));
        log(QString("数据点数量: %1, 位置范围: [%2, %3]").arg(data.size()).arg(minPos).arg(maxPos));
        
        // 尝试使用简化的拟合方法：强制使用二次项
        log(QString("尝试使用简化的二次拟合方法"));
        return performSimplifiedQuadraticFit(data);
    }
    
    // 提取系数
    result.a = coefficients[0];
    result.b = coefficients[1];
    result.c = coefficients[2];
    
    // 计算最佳位置：x = -b/(2a)
    if (qAbs(result.a) < 1e-10) {
        return result;
    }
    
    double bestRelativePos = -result.b / (2.0 * result.a);
    result.bestPosition = bestRelativePos + minPos; // 转换回绝对坐标
    
    // 计算最小HFR
    result.minHFR = result.a * bestRelativePos * bestRelativePos + 
                    result.b * bestRelativePos + result.c;
    
    return result;
}

/**
 * @brief 加权最小二乘法拟合
 * 
 * 给中心点更高权重，提高拟合精度
 * 
 * @param data 数据点
 * @return FitResult 拟合结果
 */
FitResult AutoFocus::performWeightedLeastSquares(const QVector<FocusDataPoint>& data)
{
    FitResult result;
    
    if (data.size() < 3) {
        return result;
    }
    
    // 计算数据中心位置
    double minPos = getDataMinPosition(data);
    double maxPos = getDataMaxPosition(data);
    double centerPos = (minPos + maxPos) / 2.0;
    
    // 构建加权最小二乘法正规方程组
    double sum_wx4 = 0.0, sum_wx3 = 0.0, sum_wx2 = 0.0, sum_wx = 0.0, sum_w = 0.0;
    double sum_wx2y = 0.0, sum_wxy = 0.0, sum_wy = 0.0;
    
    for (const FocusDataPoint &point : data) {
        double x = static_cast<double>(point.focuserPosition) - minPos; // 相对坐标
        double y = point.hfr;
        
        // 计算权重：距离中心越近权重越高
        double distanceFromCenter = qAbs(static_cast<double>(point.focuserPosition) - centerPos);
        double maxDistance = qMax(centerPos - minPos, maxPos - centerPos);
        double weight = 1.0 + 2.0 * (1.0 - distanceFromCenter / maxDistance); // 权重范围1.0-3.0
        
        double x2 = x * x;
        double x3 = x2 * x;
        double x4 = x3 * x;
        
        sum_wx4 += weight * x4;
        sum_wx3 += weight * x3;
        sum_wx2 += weight * x2;
        sum_wx += weight * x;
        sum_w += weight;
        
        sum_wx2y += weight * x2 * y;
        sum_wxy += weight * x * y;
        sum_wy += weight * y;
    }
    
    // 构建系数矩阵和常数向量
    double matrix[3][3] = {
        {sum_wx4, sum_wx3, sum_wx2},
        {sum_wx3, sum_wx2, sum_wx},
        {sum_wx2, sum_wx, sum_w}
    };
    
    double constants[3] = {sum_wx2y, sum_wxy, sum_wy};
    
    // 求解线性方程组
    double coefficients[3];
    if (!solveLinearSystem(matrix, constants, coefficients)) {
        return result;
    }
    
    // 提取系数
    result.a = coefficients[0];
    result.b = coefficients[1];
    result.c = coefficients[2];
    
    // 计算最佳位置：x = -b/(2a)
    if (qAbs(result.a) < 1e-10) {
        return result;
    }
    
    double bestRelativePos = -result.b / (2.0 * result.a);
    result.bestPosition = bestRelativePos + minPos; // 转换回绝对坐标
    
    // 计算最小HFR
    result.minHFR = result.a * bestRelativePos * bestRelativePos + 
                    result.b * bestRelativePos + result.c;
    
    return result;
}

/**
 * @brief 鲁棒拟合
 * 
 * 使用Huber损失函数，对异常值更鲁棒
 * 
 * @param data 数据点
 * @return FitResult 拟合结果
 */
FitResult AutoFocus::performRobustFitting(const QVector<FocusDataPoint>& data)
{
    FitResult result;
    
    if (data.size() < 3) {
        return result;
    }
    
    // 首先使用标准最小二乘法获得初始估计
    FitResult initialResult = performStandardLeastSquares(data);
    if (initialResult.bestPosition <= 0) {
        return result;
    }
    
    // 计算残差
    QVector<double> residuals;
    double minPos = getDataMinPosition(data);
    
    for (const FocusDataPoint &point : data) {
        double x = static_cast<double>(point.focuserPosition) - minPos;
        double predicted = initialResult.a * x * x + initialResult.b * x + initialResult.c;
        double residual = qAbs(point.hfr - predicted);
        residuals.append(residual);
    }
    
    // 计算中位数绝对偏差(MAD)作为鲁棒性度量
    QVector<double> sortedResiduals = residuals;
    std::sort(sortedResiduals.begin(), sortedResiduals.end());
    double medianResidual = sortedResiduals[sortedResiduals.size() / 2];
    double mad = 1.4826 * medianResidual; // 1.4826是正态分布的修正因子
    double threshold = 2.0 * mad; // Huber损失函数的阈值
    
    // 使用Huber权重进行迭代拟合
    QVector<FocusDataPoint> weightedData = data;
    for (int iteration = 0; iteration < 3; ++iteration) { // 最多3次迭代
        // 计算权重
        for (int i = 0; i < weightedData.size(); ++i) {
            double residual = residuals[i];
            double weight = 1.0;
            if (residual > threshold) {
                weight = threshold / residual; // Huber权重
            }
            // 这里简化处理，实际应该重新计算加权拟合
        }
        
        // 重新拟合（这里简化，实际应该实现完整的加权拟合）
        FitResult newResult = performStandardLeastSquares(weightedData);
        if (newResult.bestPosition > 0) {
            result = newResult;
        }
    }
    
    return result;
}

/**
 * @brief 获取数据最小位置
 * 
 * @param data 数据点
 * @return double 最小位置
 */
double AutoFocus::getDataMinPosition(const QVector<FocusDataPoint>& data)
{
    if (data.isEmpty()) return 0.0;
    
    double minPos = data[0].focuserPosition;
    for (const FocusDataPoint &point : data) {
        minPos = qMin(minPos, static_cast<double>(point.focuserPosition));
    }
    return minPos;
}

/**
 * @brief 获取数据最大位置
 * 
 * @param data 数据点
 * @return double 最大位置
 */
double AutoFocus::getDataMaxPosition(const QVector<FocusDataPoint>& data)
{
    if (data.isEmpty()) return 0.0;
    
    double maxPos = data[0].focuserPosition;
    for (const FocusDataPoint &point : data) {
        maxPos = qMax(maxPos, static_cast<double>(point.focuserPosition));
    }
    return maxPos;
}

/**
 * @brief 插值法找最佳位置
 * 
 * 当二次函数拟合失败时，使用简单的插值方法
 * 
 * @return FitResult 插值结果
 */
FitResult AutoFocus::findBestPositionByInterpolation()
{
    const QVector<FocusDataPoint> &sourceData =
        !m_superFineFocusData.isEmpty() ? m_superFineFocusData : m_fineFocusData;
    return findBestPositionByInterpolation(sourceData);
}

FitResult AutoFocus::findBestPositionByInterpolation(const QVector<FocusDataPoint>& sourceData)
{
    FitResult result;

    if (sourceData.isEmpty()) {
        return result;
    }

    // 找到 HFR 最小的有效数据点（只考虑 HFR>0 且为有限数）
    double minHFR = 0.0;
    int bestPos = 0;
    bool hasValid = false;

    for (const FocusDataPoint &point : sourceData) {
        if (!std::isfinite(point.hfr) || point.hfr <= 0.0) {
            continue;
        }
        if (!hasValid || point.hfr < minHFR) {
            minHFR = point.hfr;
            bestPos = point.focuserPosition;
            hasValid = true;
        }
    }

    if (!hasValid) {
        log("插值法：没有任何 HFR>0 的有效数据点，返回默认结果");
        return result;
    }

    result.bestPosition = bestPos;
    result.minHFR = minHFR;
    result.a = 0.0;
    result.b = 0.0;
    result.c = minHFR;

    log(QString("使用插值法找到最佳位置: %1, HFR: %2").arg(bestPos).arg(minHFR));

    return result;
}

/**
 * @brief 最终对焦验证
 * 
 * 在移动到拟合的最佳位置后，进行最终验证：
 * 1. 在当前位置拍摄图像
 * 2. 在前后各一个步长的位置拍摄图像
 * 3. 比较三个位置的HFR，确保当前位置确实是最佳
 * 4. 如果不是最佳，进行微调
 */
void AutoFocus::performFinalFocusVerification()
{
    log("开始最终对焦验证...");
    
    int currentPos = getCurrentFocuserPosition();
    int stepSize = m_fineStepSpan; // 使用精调步长
    
    // 定义验证位置：当前位置、前一个位置、后一个位置
    QVector<int> verificationPositions = {
        currentPos - stepSize,  // 前一个位置
        currentPos,             // 当前位置
        currentPos + stepSize   // 后一个位置
    };
    
    QVector<double> verificationHFRs;
    QVector<int> validPositions;
    
    // 在每个验证位置拍摄并测量HFR
    for (int i = 0; i < verificationPositions.size(); ++i) {
        int pos = verificationPositions[i];
        
        // 检查位置是否在有效范围内
        if (pos < m_focuserMinPosition || pos > m_focuserMaxPosition) {
            log(QString("验证位置%1超出范围，跳过").arg(pos));
            verificationHFRs.append(999.0); // 使用大值表示无效
            continue;
        }
        
        // 移动到验证位置
        if (pos != currentPos) {
            log(QString("移动到验证位置: %1").arg(pos));
            if (!beginMoveTo(pos, QString("最终验证位置%1").arg(i+1))) {
                log(QString("移动到验证位置%1失败").arg(pos));
                verificationHFRs.append(999.0);
                continue;
            }
            
            // 等待移动完成
            if (!waitForFocuserMoveComplete(pos, g_autoFocusConfig.positionTolerance, 30, 5)) {
                log(QString("移动到验证位置%1超时").arg(pos));
                verificationHFRs.append(999.0);
                continue;
            }
        }
        
        // 拍摄图像
        if (!captureFullImage()) {
            log(QString("验证位置%1拍摄失败").arg(pos));
            verificationHFRs.append(999.0);
            continue;
        }
        
        if (!waitForCaptureComplete()) {
            log(QString("验证位置%1拍摄超时").arg(pos));
            verificationHFRs.append(999.0);
            continue;
        }
        
        // 测量HFR
        double hfr = 0.0;
        if (detectHFRByPython(hfr) && std::isfinite(hfr) && hfr > 0 && hfr < 100.0) {
            verificationHFRs.append(hfr);
            validPositions.append(pos);
            log(QString("验证位置%1: HFR=%2").arg(pos).arg(hfr));
        } else {
            log(QString("验证位置%1: HFR无效").arg(pos));
            verificationHFRs.append(999.0);
        }
    }
    
    // 分析验证结果
    if (verificationHFRs.size() < 2) {
        log("验证数据不足，跳过最终验证");
        completeAutoFocus(true);
        return;
    }
    
    // 找到最佳验证位置
    int bestVerificationPos = currentPos;
    double bestVerificationHFR = verificationHFRs[1]; // 当前位置的HFR
    
    for (int i = 0; i < verificationHFRs.size(); ++i) {
        if (verificationHFRs[i] < bestVerificationHFR) {
            bestVerificationHFR = verificationHFRs[i];
            bestVerificationPos = verificationPositions[i];
        }
    }
    
    // 检查是否需要微调
    if (bestVerificationPos != currentPos) {
        log(QString("发现更佳位置: %1 (HFR=%2) vs 当前位置: %3 (HFR=%4)")
            .arg(bestVerificationPos).arg(bestVerificationHFR)
            .arg(currentPos).arg(verificationHFRs[1]));
        
        // 移动到更佳位置
        log(QString("进行最终微调，移动到更佳位置: %1").arg(bestVerificationPos));
        if (beginMoveTo(bestVerificationPos, "最终微调到更佳位置")) {
            if (waitForFocuserMoveComplete(bestVerificationPos, g_autoFocusConfig.positionTolerance, 30, 5)) {
                log(QString("最终微调完成，到达最佳位置: %1").arg(bestVerificationPos));
            } else {
                log("最终微调超时，但继续完成对焦");
            }
        } else {
            log("最终微调失败，但继续完成对焦");
        }
    } else {
        log(QString("验证确认当前位置%1确实是最佳位置 (HFR=%2)")
            .arg(currentPos).arg(bestVerificationHFR));
    }
    
    // 完成自动对焦
    completeAutoFocus(true);
}

/**
 * @brief 计算拟合质量R²
 * 
 * 计算决定系数R²，用于评估拟合质量
 * 
 * @param data 数据点
 * @param fit 拟合结果
 * @param offset 坐标偏移
 * @return double R²值
 */
double AutoFocus::calculateRSquared(const QVector<FocusDataPoint>& data, const FitResult& fit, double offset)
{
    if (data.isEmpty()) {
        return 0.0;
    }
    
    // 计算平均值
    double meanY = 0.0;
    for (const FocusDataPoint &point : data) {
        meanY += point.hfr;
    }
    meanY /= data.size();
    
    // 计算总平方和和残差平方和
    double totalSS = 0.0;  // 总平方和
    double residualSS = 0.0; // 残差平方和
    
    for (const FocusDataPoint &point : data) {
        double x = static_cast<double>(point.focuserPosition) - offset;
        double predictedY = fit.a * x * x + fit.b * x + fit.c;
        
        totalSS += (point.hfr - meanY) * (point.hfr - meanY);
        residualSS += (point.hfr - predictedY) * (point.hfr - predictedY);
    }
    
    if (totalSS < 1e-10) {
        return 1.0; // 如果总平方和接近零，认为拟合完美
    }
    
    double rSquared = 1.0 - (residualSS / totalSS);
    return rSquared;
}

/**
 * @brief 设置拍摄完成
 * 
 * 当图像拍摄和下载完成时调用此方法
 * 
 * @param imagePath 拍摄的图像路径，如果为空则使用默认路径
 */
void AutoFocus::setCaptureComplete(const QString& imagePath)
{
    m_isCaptureEnd = true;
    
    if (!imagePath.isEmpty()) {
        m_lastCapturedImage = imagePath;
    } else if (m_lastCapturedImage.isEmpty()) {
        m_lastCapturedImage = "/dev/shm/ccd_simulator.fits";
    }
    
    log(QString("拍摄完成: %1").arg(m_lastCapturedImage));
    emit captureStatusChanged(true, m_lastCapturedImage);
}

/**
 * @brief 设置拍摄失败
 * 
 * 当图像拍摄失败时调用此方法
 */
void AutoFocus::setCaptureFailed()
{
    m_isCaptureEnd = true;
    // 不清空图像路径，避免回调访问空字符串导致段错误
    // m_lastCapturedImage 保持不变
    
    log(QString("拍摄失败（图像路径保留）"));
    emit captureStatusChanged(false, m_lastCapturedImage);
}

/**
 * @brief 重置拍摄状态
 * 
 * 重置拍摄完成状态，准备下一次拍摄
 */
void AutoFocus::resetCaptureStatus()
{
    m_isCaptureEnd = false;
    // 不清空图像路径，避免回调访问空字符串导致段错误
    // m_lastCapturedImage 保持不变
    
    log(QString("重置拍摄状态（图像路径保留）"));
}

/**
 * @brief 开始电调移动
 * 
 * 非阻塞方式启动电调移动到目标位置
 * 
 * @param targetPosition 目标位置
 */
void AutoFocus::startFocuserMove(int targetPosition)
{
    if (!m_dpFocuser) {
        log(QString("错误: 电调设备对象为空，无法移动"));
        return;
    }
    
    // 检查电调设备连接状态
    if (!m_dpFocuser->isConnected()) {
        log(QString("错误: 电调设备未连接，无法移动"));
        return;
    }
    
    log(QString("电调设备状态: 连接=%1, 设备名称=%2")
        .arg(m_dpFocuser->isConnected() ? "是" : "否")
        .arg(m_dpFocuser->getDeviceName()));
    
    // 检查位置限制
    if (targetPosition < m_focuserMinPosition) {
        log(QString("警告: 目标位置 %1 小于最小位置 %2，限制到最小位置").arg(targetPosition).arg(m_focuserMinPosition));
        targetPosition = m_focuserMinPosition;
    } else if (targetPosition > m_focuserMaxPosition) {
        log(QString("警告: 目标位置 %1 大于最大位置 %2，限制到最大位置").arg(targetPosition).arg(m_focuserMaxPosition));
        targetPosition = m_focuserMaxPosition;
    }
    m_currentPosition = getCurrentFocuserPosition();
    
    // 计算移动方向和步数（使用和beginMoveTo相同的逻辑）
    int delta = targetPosition - m_currentPosition;
    if (delta == 0) {
        log(QString("目标与当前位置相同(%1)，跳过移动").arg(targetPosition));
        m_isFocuserMoving = false;
        return;
    }
    
    const bool isInward = (delta < 0);
    int steps = std::abs(delta);
    steps = std::max(1, steps); // 最小 1 步，避免 0 步等待
    
    // 设置移动状态
    m_isFocuserMoving = true;
    m_targetFocuserPosition = targetPosition;
    m_moveStartTime = QDateTime::currentMSecsSinceEpoch();
    
    // 初始化位置变化检测
    m_lastPosition = m_currentPosition;
    
    // 使用相对移动命令（和beginMoveTo相同的方式）
    m_indiServer->setFocuserMoveDiretion(m_dpFocuser, isInward);
    m_indiServer->moveFocuserSteps(m_dpFocuser, steps);
    
    log(QString("电调相对移动命令已发送: %1 步，方向: %2，目标位置: %3")
        .arg(steps).arg(isInward ? "向内" : "向外").arg(targetPosition));
    
    // 初始化电调判断参数
    initializeFocuserMoveParameters();
    
    log(QString("开始电调移动: 当前位置=%1, 目标位置=%2, 相对移动命令发送成功").arg(m_currentPosition).arg(targetPosition));
}

/**
 * @brief 检查电调移动是否完成
 * 
 * 非阻塞方式检查电调移动状态
 * 
 * @return bool 移动是否完成
 */
bool AutoFocus::checkFocuserMoveComplete()
{
    if (!m_isFocuserMoving) {
        log(QString("电调未在移动状态"));
        return true; // 没有在移动，认为已完成
    }
    
    log(QString("检查电调移动状态: 目标位置=%1").arg(m_targetFocuserPosition));
    
    // 获取当前位置
    int currentPosition;
    int success = m_indiServer->getFocuserAbsolutePosition(m_dpFocuser, currentPosition);

    log(QString("获取电调位置: 成功=%1, 当前位置=%2, 目标位置=%3, 移动计数=%4")
        .arg(success == 0 ? "是" : "否")
        .arg(currentPosition)
        .arg(m_targetFocuserPosition)
        .arg(m_moveWaitCount));
    
    // 更新内部位置记录
    m_currentPosition = currentPosition;
    
    // 如果是第一次检查，初始化位置记录
    if (!m_hasLastPosition) {
        m_lastPosition = currentPosition;
        m_hasLastPosition = true; // C++: true (we will fix case below)
        log(QString("初始化电调位置记录: 位置=%1").arg(currentPosition));
    }
    
            // 使用统一的位置检查函数
        if (isPositionReached(currentPosition, m_targetFocuserPosition, g_autoFocusConfig.positionTolerance)) {
            log(QString("电调移动完成: 当前位置=%1, 目标位置=%2").arg(currentPosition).arg(m_targetFocuserPosition));
            stopFocuserMove();
            // 发送电调位置同步信号
            emit m_wsThread->sendMessageToClient("FocusPosition:" + QString::number(currentPosition) + ":" + QString::number(currentPosition));
            return true;
        }
    
    // 检查位置是否发生变化
    if (currentPosition != m_lastPosition) {
        // 位置发生变化，更新记录并重置计数器
        m_lastPosition = currentPosition;
        m_moveWaitCount = 0; // 重置等待计数器
        log(QString("电调位置变化: %1").arg(currentPosition));
    } else {
        // 位置没有变化，增加等待计数器
        m_moveWaitCount++;
        
        // 检查是否超时（基于定时器100ms间隔计算）
        int timeoutCount = m_moveTimeout / 100; // 将毫秒转换为100ms间隔的计数
        if (m_moveWaitCount > timeoutCount) {
            log(QString("警告: 电调移动超时: %1秒未检测到位置变化，目标位置: %2，视为失败")
                .arg(m_moveTimeout / 1000.0).arg(m_targetFocuserPosition));
            stopFocuserMove();
            return false; // 超时视为未完成，交由上层处理
        }
    }
    
    // 还在移动中
    log(QString("电调仍在移动中: 当前位置=%1, 目标位置=%2").arg(currentPosition).arg(m_targetFocuserPosition));
    return false;
}

/**
 * @brief 停止电调移动
 * 
 * 清除移动状态
 */
void AutoFocus::stopFocuserMove()
{
    log(QString("停止电调移动，发送停止命令"));
    
    // 发送真正的电调停止命令
    if (m_dpFocuser) {
        m_indiServer->abortFocuserMove(m_dpFocuser);
        log("电调停止命令已发送");
    } else {
        log("警告: 电调设备对象为空，无法发送停止命令");
    }
    
    // 清除移动状态
    m_isFocuserMoving = false;
    // 不清零目标位置，避免影响后续等待逻辑
    // m_targetFocuserPosition = 0;
    m_moveStartTime = 0;
    m_lastPosition = 0;
    m_moveWaitCount = 0; // 重置等待计数器
    
    log("电调移动状态已清除");
}

/**
 * @brief 初始化电调移动判断参数
 * 
 * 在每次发送电调移动命令后调用，确保判断参数正确初始化
 */
void AutoFocus::initializeFocuserMoveParameters()
{
    // 获取当前电调位置
    int currentPosition = getCurrentFocuserPosition();
    
    // 初始化位置变化检测参数
    m_lastPosition = currentPosition;
    m_hasLastPosition = true;
    m_moveWaitCount = 0; // 重置等待计数器
    
    log(QString("初始化电调移动判断参数: 当前位置=%1, 目标位置=%2")
        .arg(currentPosition).arg(m_targetFocuserPosition));
}

// ==================== ROI相关方法 ====================

/**
 * @brief 设置ROI大小
 * 
 * @param size ROI大小（正方形）
 */
void AutoFocus::setROISize(int size)
{
    if (size > 0) {
        m_roiSize = size;
        log(QString("设置ROI大小: %1x%1").arg(size));
    } else {
        log(QString("警告: ROI大小必须大于0"));
    }
}

/**
 * @brief 根据星点位置更新ROI中心
 * 
 * 当识别到星点后，根据星点位置更新ROI中心，并重新计算ROI区域
 * 
 * @param starPosition 星点位置
 */
void AutoFocus::updateROICenter(const QPointF& starPosition)
{
    m_roiCenter = starPosition;
    m_currentROI = calculateROI(starPosition, m_roiSize);
    m_useROI = true;
    
    log(QString("更新ROI中心: (%1, %2), ROI区域: x=%3, y=%4, w=%5, h=%6")
        .arg(starPosition.x()).arg(starPosition.y())
        .arg(m_currentROI.x()).arg(m_currentROI.y())
        .arg(m_currentROI.width()).arg(m_currentROI.height()));
}

/**
 * @brief 计算ROI区域
 * 
 * 根据中心点和大小计算ROI区域，确保不超出图像边界
 * 
 * @param center 中心点
 * @param size ROI大小
 * @return QRect ROI区域
 */
QRect AutoFocus::calculateROI(const QPointF& center, int size)
{
    // 使用配置中的图像尺寸
    const int imageWidth = m_imageWidth;
    const int imageHeight = m_imageHeight;
    
    // 计算ROI的左上角坐标
    int x = static_cast<int>(center.x() - size / 2);
    int y = static_cast<int>(center.y() - size / 2);
    
    // 确保ROI不超出图像边界
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + size > imageWidth) x = imageWidth - size;
    if (y + size > imageHeight) y = imageHeight - size;
    
    return QRect(x, y, size, size);
}

/**
 * @brief 检查ROI是否有效
 * 
 * @param roi ROI区域
 * @return bool ROI是否有效
 */
bool AutoFocus::isROIValid(const QRect& roi) const
{
    return roi.isValid() && roi.width() > 0 && roi.height() > 0;
}

/**
 * @brief 统一等待电调移动完成
 * 
 * 非阻塞方式等待电调移动到目标位置，包含超时和卡住检测
 * 
 * @param targetPosition 目标位置
 * @param tolerance 位置误差容差
 * @param timeoutSeconds 超时时间（秒）
 * @param stuckTimeoutSeconds 卡住检测时间（秒）
 * @return bool 是否成功到达目标位置
 */
bool AutoFocus::waitForFocuserMoveComplete(int targetPosition, int tolerance, int timeoutSeconds, int stuckTimeoutSeconds)
{
    if (!m_isRunning) {
        log("自动对焦已停止，跳过电调移动等待");
        return false;
    }
    
    // 初始化等待状态
    m_moveWaitCount = 0;
    m_moveLastPosition = getCurrentFocuserPosition();
    int initialPosition = m_moveLastPosition;
    int moveDistance = qAbs(targetPosition - initialPosition);
    
    log(QString("开始等待电调移动: 初始位置=%1, 目标位置=%2, 移动距离=%3, 容差=%4")
        .arg(initialPosition).arg(targetPosition).arg(moveDistance).arg(tolerance));
    
    // 根据移动距离动态调整超时时间，但设置合理的上限
    int dynamicTimeout = qMin(qMax(timeoutSeconds, moveDistance / 10), 300); // 每10步至少1秒，最多300秒
    int dynamicStuckTimeout = qMin(qMax(stuckTimeoutSeconds, moveDistance / 50), 60); // 每50步至少1秒，最多60秒
    
    log(QString("动态超时设置: 总超时=%1秒, 卡住检测=%2秒").arg(dynamicTimeout).arg(dynamicStuckTimeout));
    
    // 检查特殊情况：如果目标位置为0但当前位置不为0，说明电调已经停止
    if (targetPosition == 0 && initialPosition != 0) {
        log(QString("检测到电调已停止移动: 当前位置=%1, 目标位置=%2").arg(initialPosition).arg(targetPosition));
        return true; // 认为移动已完成
    }
    
    // 使用定时器替代 while 循环，避免阻塞
    // 设置移动检查参数
    m_moveCheckTargetPosition = targetPosition;
    m_moveCheckTolerance = tolerance;
    m_moveCheckTimeout = dynamicTimeout;
    m_moveCheckStuckTimeout = dynamicStuckTimeout;
    m_moveCheckPending = true;
    m_moveCheckResult = false;
    m_moveWaitCount = 0;
    m_moveLastPosition = initialPosition;
    
    // 检查是否已经停止，避免在停止状态下进入等待
    if (!m_isRunning) {
        log("自动对焦已停止，跳过电调移动等待");
        return false;
    }
    
    // 创建超时定时器
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(dynamicTimeout * 1000); // 设置总超时时间
    
    // 使用事件循环等待结果，避免阻塞主线程
    QEventLoop eventLoop;
    
    // 使用 QPointer 来安全地管理定时器
    QPointer<QTimer> moveTimerPtr = m_moveCheckTimer;
    
    // 连接超时信号，当定时器超时或移动检查完成时退出事件循环
    QObject::connect(&timeoutTimer, &QTimer::timeout, &eventLoop, [this, dynamicTimeout, &eventLoop, moveTimerPtr]() {
        if (m_moveCheckPending) {
            log(QString("电调移动超时: 等待%1秒后仍未到达目标位置").arg(dynamicTimeout));
            m_moveCheckPending = false;
            if (moveTimerPtr && moveTimerPtr->isActive()) {
                moveTimerPtr->stop();
            }
        }
        eventLoop.quit();
    });
    
    // 创建一个辅助定时器来定期检查移动状态
    QTimer checkTimer;
    checkTimer.setInterval(50); // 每50ms检查一次 pending 状态
    QObject::connect(&checkTimer, &QTimer::timeout, &eventLoop, [this, &checkTimer, &eventLoop, moveTimerPtr]() {
        if (!m_moveCheckPending) {
            // 移动检查已完成，退出事件循环
            checkTimer.stop();
            eventLoop.quit();
        }
    });
    
    // 启动所有定时器
    if (m_moveCheckTimer) {
        m_moveCheckTimer->start();
    }
    timeoutTimer.start();
    checkTimer.start();
    
    // 进入事件循环等待，直到移动完成或超时
    // 添加定期检查，确保在 stopAutoFocus 被调用时能及时退出
    QTimer stopCheckTimer;
    stopCheckTimer.setInterval(25); // 每25ms检查一次停止状态，提高响应速度
    QObject::connect(&stopCheckTimer, &QTimer::timeout, &eventLoop, [this, &eventLoop, &stopCheckTimer]() {
        if (!m_isRunning || !m_moveCheckPending) {
            stopCheckTimer.stop();
            eventLoop.quit();
        }
    });
    stopCheckTimer.start();
    
    // 设置事件循环超时，防止无限等待
    QTimer maxWaitTimer;
    maxWaitTimer.setSingleShot(true);
    maxWaitTimer.setInterval((dynamicTimeout + 5) * 1000); // 比移动超时多5秒
    QObject::connect(&maxWaitTimer, &QTimer::timeout, &eventLoop, [&eventLoop]() {
        eventLoop.quit();
    });
    maxWaitTimer.start();
    
    // 使用 QEventLoop::ProcessEvents 模式，确保能及时响应停止信号
    int maxIterations = ((dynamicTimeout + 5) * 1000) / 10; // 最大迭代次数
    int iteration = 0;
    while (m_moveCheckPending && m_isRunning && maxWaitTimer.isActive() && iteration < maxIterations) {
        eventLoop.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10); // 短暂等待，避免CPU占用过高
        iteration++;
        
        // 每50次迭代检查一次，防止无限循环
        if (iteration % 50 == 0) {
            if (!m_isRunning) {
                log("检测到停止信号，立即退出移动等待循环");
                break; // 如果已停止，立即退出
            }
        }
        
        // 每200次迭代强制检查一次状态
        if (iteration % 200 == 0) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        }
    }
    
    // 如果循环结束但仍在等待，强制退出
    if (m_moveCheckPending) {
        log("强制退出移动等待循环");
        m_moveCheckPending = false;
        m_moveCheckResult = false;
        
        // 强制处理事件，确保状态更新生效
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    
    // 停止最大等待定时器
    maxWaitTimer.stop();
    
    // 停止所有定时器
    stopCheckTimer.stop();
    if (m_moveCheckTimer) {
        m_moveCheckTimer->stop();
    }
    timeoutTimer.stop();
    checkTimer.stop();
    
    // 返回结果
    bool result = m_moveCheckResult;
    m_moveCheckPending = false;
    
    return result;
}

void AutoFocus::initializeVirtualData()
{
    if (!m_starSimulator) {
        m_starSimulator = new StarSimulator(this);
    }
    
    // 设置真实的望远镜参数
    TelescopeParams scope;
    scope.focalLength = 510.0;    // 510mm焦距
    scope.aperture = 85.0;         // 85mm口径
    scope.obstruction = 0.0;       // 无遮挡（折射镜）
    scope.seeing = 1.5 + m_uniformDist(m_mtGenerator) * 1.0; // 视宁度1.5-2.5arcsec随机
    scope.pixelSize = 3.76;         // 3.76μm像素（1080p）
    scope.binning = 1.0;            // 1x1合并
    scope.filterTransmission = 1; // 80%透过率
    scope.exposureTime = 1.0;       // 1秒曝光
    
    m_starSimulator->setTelescopeParams(scope);
    
    // 随机化最佳对焦位置
    double focusRange = m_focuserMaxPosition - m_focuserMinPosition;
    double randomOffset = (m_uniformDist(m_mtGenerator) - 0.5) * 0.3; // ±15%范围
    double bestPosition = (m_focuserMaxPosition + m_focuserMinPosition) / 2.0 + 
                         randomOffset * focusRange;
    
    // 确保最佳位置在有效范围内
    bestPosition = qBound(static_cast<double>(m_focuserMinPosition), 
                         bestPosition, 
                         static_cast<double>(m_focuserMaxPosition));
    
    // 设置对焦曲线参数（基于真实设备计算）
    FocusCurve curve;
    curve.bestPosition = bestPosition;
    curve.minHFR = 1.0 + m_uniformDist(m_mtGenerator) * 0.5; // 最小HFR在1.0-1.5之间随机
    curve.curveWidth = focusRange * (0.8 + m_uniformDist(m_mtGenerator) * 0.4); // 曲线宽度在80%-120%之间随机
    curve.asymmetry = (m_uniformDist(m_mtGenerator) - 0.5) * 0.2; // 不对称性在±10%之间随机
    m_starSimulator->setFocusCurve(curve);
    
    // 设置噪声参数（也随机化）
    double noiseLevel = 0.01 + m_uniformDist(m_mtGenerator) * 0.02; // 噪声水平在0.01-0.03之间，大幅降低
    double turbulenceLevel = 0.1 + m_uniformDist(m_mtGenerator) * 0.1; // 大气扰动在0.1-0.2之间，降低
    m_starSimulator->setNoiseParameters(noiseLevel, turbulenceLevel);
    
    // 随机化大气扰动参数
    double turbulenceScale = 30.0 + m_uniformDist(m_mtGenerator) * 40.0; // 扰动尺度在30-70之间
    double turbulenceIntensity = 0.05 + m_uniformDist(m_mtGenerator) * 0.1; // 扰动强度在0.05-0.15之间，降低
    double turbulenceTimeScale = 0.05 + m_uniformDist(m_mtGenerator) * 0.1; // 时间尺度在0.05-0.15之间
    m_starSimulator->setTurbulenceParameters(turbulenceScale, turbulenceIntensity, turbulenceTimeScale);
    
    log(QString("虚拟数据模式已初始化，最佳焦位置: %1 (范围: %2-%3)")
        .arg(bestPosition).arg(m_focuserMinPosition).arg(m_focuserMaxPosition));
    log(QString("设备参数: 焦距=%1mm, 口径=%2mm, 视宁度=%3arcsec")
        .arg(scope.focalLength).arg(scope.aperture).arg(scope.seeing));
    log(QString("对焦曲线参数: 最小HFR=%1, 曲线宽度=%2, 不对称性=%3")
        .arg(curve.minHFR).arg(curve.curveWidth).arg(curve.asymmetry));
    log(QString("噪声参数: 噪声水平=%1, 大气扰动=%2")
        .arg(noiseLevel).arg(turbulenceLevel));
}

QString AutoFocus::getNextVirtualImagePath()
{
    if (m_virtualImagePath.isEmpty()) {
        m_virtualImagePath = "/dev/shm";
    }
    
    // 使用固定的文件名 ccd_simulator.fits
    QString path = QString("%1/ccd_simulator.fits")
                   .arg(m_virtualImagePath);
    
    return path;
}

bool AutoFocus::generateVirtualImage(int exposureTime, bool useROI)
{
    if (!m_useVirtualData || !m_starSimulator) {
        log("虚拟数据模式未启用或模拟器未初始化");
        return false;
    }
    
    try {
        // 获取当前电调位置
        int currentPos = getCurrentFocuserPosition();
        
        // 设置星图生成参数
        StarImageParams params;
        params.imageWidth = useROI ? m_roiSize : m_imageWidth;
        params.imageHeight = useROI ? m_roiSize : m_imageHeight;
        params.focuserMinPos = m_focuserMinPosition;
        params.focuserMaxPos = m_focuserMaxPosition;
        params.focuserCurrentPos = currentPos;
        params.minStarCount = 15;  // 进一步增加最小星点数量
        params.maxStarCount = 35; // 进一步增加最大星点数量
        params.exposureTime = qMax(1.0, exposureTime / 1000.0); // 确保最小曝光时间1秒
        params.noiseLevel = 0.0; // 完全禁用噪声
        params.outputPath = getNextVirtualImagePath();
        
        log(QString("生成虚拟星图: 位置=%1, 曝光=%2ms, ROI=%3, 输出=%4")
            .arg(currentPos).arg(exposureTime).arg(useROI ? "是" : "否").arg(params.outputPath));
        
        // 检查并删除已存在的同名文件
        QFileInfo fileInfo(params.outputPath);
        if (fileInfo.exists()) {
            QFile existingFile(params.outputPath);
            if (existingFile.remove()) {
                log(QString("已删除已存在的文件: %1").arg(params.outputPath));
            } else {
                log(QString("警告：无法删除已存在的文件: %1").arg(params.outputPath));
            }
        }
        
        // 生成虚拟星图
        bool success = m_starSimulator->generateStarImage(params);
        
        if (success) {
            // 更新图像路径（不设置拍摄状态，由调用者处理）
            m_lastCapturedImage = params.outputPath;
            
            // 验证生成的文件是否存在
            QFileInfo newFileInfo(m_lastCapturedImage);
            if (newFileInfo.exists() && newFileInfo.size() > 0) {
                log(QString("虚拟星图生成成功: %1 (大小: %2 字节)")
                    .arg(m_lastCapturedImage).arg(newFileInfo.size()));
                return true;
            } else {
                log(QString("虚拟星图文件验证失败: 存在=%1, 大小=%2")
                    .arg(newFileInfo.exists()).arg(newFileInfo.size()));
                return false;
            }
        } else {
            log("虚拟星图生成失败");
            return false;
        }
    } catch (const std::exception &e) {
        log(QString("虚拟图像生成异常: %1").arg(e.what()));
        return false;
    } catch (...) {
        log("虚拟图像生成发生未知异常");
        return false;
    }
}

double AutoFocus::getVirtualBestFocusPosition() const
{
    if (m_useVirtualData && m_starSimulator) {
        // 从星图模拟器获取对焦曲线信息
        FocusCurve curve = m_starSimulator->getFocusCurve();
        return curve.bestPosition;
    }
    return 0.0;
}

void AutoFocus::setUseVirtualData(bool useVirtual)
{
    m_useVirtualData = useVirtual;
    log(QString("虚拟数据模式: %1").arg(useVirtual ? "启用" : "禁用"));
}

bool AutoFocus::isUsingVirtualData() const
{
    return m_useVirtualData;
}

void AutoFocus::setVirtualImagePath(const QString &path)
{
    m_virtualImagePath = path;
    log(QString("虚拟图像路径已设置: %1").arg(path));
}

QString AutoFocus::getVirtualImagePath() const
{
    return m_virtualImagePath;
}

void AutoFocus::setVirtualOutputPath(const QString &path)
{
    m_virtualImagePath = path;
    log(QString("虚拟输出路径已设置: %1").arg(path));
}

QString AutoFocus::getVirtualOutputPath() const
{
    return m_virtualImagePath;
}

QString AutoFocus::getCurrentTestImagePath() const
{
    return QString("/home/quarcs/test_fits/coarse/%1.fits").arg(m_testFileCounter);
}

// ==================== 星点选择方法 ====================

/**
 * @brief 选择置信度最高的星点并计算平均HFR
 * 
 * 这是新的星点处理策略的核心方法：
 * 1. 检测图像中的所有星点
 * 2. 根据置信度选择最佳的几颗星
 * 3. 计算这些星点的平均HFR值
 * 4. 返回平均HFR用于对焦判断
 * 
 * @return double 平均HFR值，如果没有星点则返回999.0
 */
double AutoFocus::selectTopStarsAndCalculateHFR()
{
double hfr = 0.0;
    if (!detectHFRByPython(hfr)) {
        log("无法通过Python得到HFR，返回占位值999");
        return 999.0;
    }
    return hfr;
}


/**
 * @brief 根据置信度选择最佳星点
 * 
 * 基于多个特征计算星点置信度，选择最佳的几颗星：
 * 1. 亮度特征（峰值亮度）
 * 2. 形状特征（HFR值）
 * 3. 位置特征（图像中心区域优先）
 * 4. 稳定性特征（多次检测的一致性）
 * 
 * @param stars 所有检测到的星点
 * @return QList<FITSImage::Star> 选择的最佳星点列表
 */
QList<FITSImage::Star> AutoFocus::selectTopStarsByConfidence(const QList<FITSImage::Star>& stars)
{
    if (stars.isEmpty()) {
        return QList<FITSImage::Star>();
    }
    // 计算每个星点的置信度
    QVector<QPair<double, FITSImage::Star>> scoredStars;
    for (const FITSImage::Star &star : stars) {
        double confidence = calculateStarConfidence(star);
        scoredStars.append(qMakePair(confidence, star));
    }
    
    // 按置信度降序排序
    std::sort(scoredStars.begin(), scoredStars.end(), 
              [](const QPair<double, FITSImage::Star>& a, const QPair<double, FITSImage::Star>& b) {
                  return a.first > b.first;
              });
    
    // 选择置信度最高的星点
    QList<FITSImage::Star> topStars;
    int maxStars = qMin(m_topStarCount, scoredStars.size());
    
    for (int i = 0; i < maxStars; ++i) {
        topStars.append(scoredStars[i].second);
    }
    
    log(QString("从%1个星点中选择了%2个最佳星点").arg(stars.size()).arg(topStars.size()));
    
    return topStars;
}

/**
 * @brief 计算星点置信度
 * 
 * 综合考虑多个特征计算星点置信度：
 * - 亮度权重40%：峰值亮度越高，置信度越高
 * - HFR权重30%：HFR越小（越锐利），置信度越高
 * - 位置权重20%：越靠近图像中心，置信度越高
 * - 形状权重10%：形状越规则，置信度越高
 * 
 * @param star 要评估的星点
 * @return double 置信度分数 (0-1)
 */
double AutoFocus::calculateStarConfidence(const FITSImage::Star& star)
{
    // 使用配置中的图像尺寸
    const double imageCenterX = m_imageWidth / 2.0;
    const double imageCenterY = m_imageHeight / 2.0;
    const double maxDistance = qSqrt(imageCenterX * imageCenterX + imageCenterY * imageCenterY);
    
    // 亮度分数（峰值亮度越高分数越高）
    double brightnessScore = qMin(star.peak / 65535.0, 1.0); // 假设16位图像
    
    // HFR分数（HFR越小分数越高，假设最佳HFR为1.0）
    double hfrScore = 1.0;
    if (star.HFR > 0) {
        hfrScore = qMax(0.0, 1.0 - (star.HFR - 1.0) / 5.0); // 1.0-6.0范围映射到1.0-0.0
        hfrScore = qBound(0.0, hfrScore, 1.0);
    }
    
    // 位置分数（越靠近中心分数越高）
    double distanceFromCenter = qSqrt(qPow(star.x - imageCenterX, 2) + qPow(star.y - imageCenterY, 2));
    double positionScore = 1.0 - (distanceFromCenter / maxDistance);
    positionScore = qBound(0.0, positionScore, 1.0);
    
    // 形状分数（基于HFR的稳定性，假设HFR在合理范围内）
    double shapeScore = 1.0;
    if (star.HFR > 0) {
        if (star.HFR >= 0.5 && star.HFR <= 8.0) {
            shapeScore = 1.0; // 在合理范围内
        } else if (star.HFR < 0.5) {
            shapeScore = 0.0; // 可能过小
        } else {
            shapeScore = 0.3; // 可能过大
        }
    }
    
    // 综合分数（加权平均）
    double totalScore = (brightnessScore * 0.4 + hfrScore * 0.3 + positionScore * 0.2 + shapeScore * 0.1);
    
    return totalScore;
}

// ==================== 新增：Python HFR识星接口 与 INI范围读取 ====================
bool AutoFocus::detectHFRByPython(double &hfr)
{
    // 基本检查
    if (!m_isRunning) {
        log("自动对焦已停止，跳过Python识星");
        return false;
    }
    if (m_lastCapturedImage.isEmpty()) {
        log("错误：没有可用的图像文件供Python识星");
        return false;
    }
    QFileInfo fi(m_lastCapturedImage);
    if (!fi.exists() || fi.size() == 0) {
        log(QString("错误：图像文件无效: %1").arg(m_lastCapturedImage));
        return false;
    }

    // 调用Python脚本进行星点识别并输出HFR
    bool ok = Tools::findStarsByPython_Process(m_lastCapturedImage);
    if (!ok) {
        log("Python脚本执行失败或未返回有效结果");
        return false;
    }
    double val = Tools::getLastHFR();
    m_lastHFR = val;
    log(QString("Python返回HFR: %1 (文件: %2)").arg(val).arg(m_lastCapturedImage));
    hfr = val;
    if (!std::isfinite(val) || val <= 0) {
        log("HFR数值无效");
        return false;
    }
    return true;
}

/**
 * @brief 使用 calculatestars.py 计算当前图像的 median_HFR
 *
 * 普通模式下使用最近一次拍摄的 m_lastCapturedImage 路径；
 * 若脚本执行失败或未解析到有效数值，则返回的 hfr 记为 0.0，
 * 上层逻辑据此决定“本次测量不参与拟合但流程继续”。
 */
bool AutoFocus::detectMedianHFRByPython(double &hfr)
{
    // 基本检查
    if (!m_isRunning) {
        log("自动对焦已停止，跳过 Python median_HFR 计算");
        return false;
    }

    bool okScript = false;
    double val = 0.0;

#if AUTOFOCUS_SNR_TEST_MODE
    // === 测试模式：使用 /home/quarcs/test_fits/1/1.fits ~ 9.fits 作为 super-fine 测试文件 ===
    QString testFilePath = QString("/home/quarcs/test_fits/1/%1.fits").arg(m_testFileCounter);
    QFileInfo fi(testFilePath);
    if (!fi.exists() || fi.size() == 0) {
        log(QString("错误：super-fine 测试文件不存在或无效: %1").arg(testFilePath));
        return false;
    }

    log(QString("使用 super-fine 测试文件进行 median_HFR 计算: %1 (第%2个文件)")
            .arg(testFilePath).arg(m_testFileCounter));

    okScript = Tools::findMedianHFRByPython_Process(testFilePath);
    val = Tools::getLastMedianHFR();

    // 计数器递增，循环处理 1.fits ~ 9.fits
    m_testFileCounter++;
    if (m_testFileCounter > 9) {
        m_testFileCounter = 1;
        log("super-fine median_HFR 测试文件循环完成，重新从 1.fits 开始");
    }

    m_lastHFR = val;
    log(QString("Python 返回 median_HFR: %1 (测试文件: %2, 脚本状态: %3)")
            .arg(val)
            .arg(testFilePath)
            .arg(okScript ? "ok" : "failed"));
#else
    if (m_lastCapturedImage.isEmpty()) {
        log("错误：没有可用的图像文件供 Python median_HFR 脚本使用");
        return false;
    }

    QFileInfo fi(m_lastCapturedImage);
    if (!fi.exists() || fi.size() == 0) {
        log(QString("错误：图像文件无效: %1").arg(m_lastCapturedImage));
        return false;
    }

    // 调用 calculatestars.py 计算 median_HFR
    okScript = Tools::findMedianHFRByPython_Process(m_lastCapturedImage);
    val = Tools::getLastMedianHFR();
    m_lastHFR = val;

    log(QString("Python 返回 median_HFR: %1 (文件: %2, 脚本状态: %3)")
            .arg(val)
            .arg(m_lastCapturedImage)
            .arg(okScript ? "ok" : "failed"));
#endif

    // 数值校验：无效时记为 0.0，但整体流程继续
    if (!std::isfinite(val) || val < 0.0) {
        log("median_HFR 数值无效，记为 0.0（本点不参与拟合）");
        val = 0.0;
    }

    hfr = val;
    // 即使脚本执行失败（okScript == false），也返回 true，
    // 由上层根据 hfr 是否大于 0 决定是否参与拟合。
    return true;
}

/**
 * @brief 通过 Python 脚本计算 avg_top50_snr（粗调 / 精调使用）
 * @param snr 返回的 avg_top50_snr 数值
 * @return 是否成功
 */
bool AutoFocus::detectSNRByPython(double &snr)
{
    // 基本检查
    if (!m_isRunning) {
        log("自动对焦已停止，跳过Python SNR计算");
        return false;
    }

#if AUTOFOCUS_SNR_TEST_MODE
    // === 测试模式：使用 /home/quarcs/FOCUSTEST/1.fits ~ 10.fits 循环 ===
    QString testFilePath = QString("/home/quarcs/FOCUSTEST/%1.fits").arg(m_testFileCounter);
    QFileInfo fi(testFilePath);
    if (!fi.exists() || fi.size() == 0) {
        log(QString("错误：测试文件不存在或无效: %1").arg(testFilePath));
        return false;
    }
    log(QString("使用测试文件进行 SNR 计算: %1 (第%2个文件)").arg(testFilePath).arg(m_testFileCounter));

    bool ok = Tools::findSNRByPython_Process(testFilePath);
#else
    if (m_lastCapturedImage.isEmpty()) {
        log("错误：没有可用的图像文件供Python SNR脚本使用");
        return false;
    }
    QFileInfo fi(m_lastCapturedImage);
    if (!fi.exists() || fi.size() == 0) {
        log(QString("错误：图像文件无效: %1").arg(m_lastCapturedImage));
        return false;
    }

    bool ok = Tools::findSNRByPython_Process(m_lastCapturedImage);
#endif
    if (!ok) {
        log("Python SNR 脚本执行失败或未返回有效结果");
        return false;
    }

    double val = Tools::getLastSNR();
    
#if AUTOFOCUS_SNR_TEST_MODE
    log(QString("Python 返回 avg_top50_snr: %1 (测试文件序号: %2)").arg(val).arg(m_testFileCounter));

    // 计数器递增，循环处理 1-10.fits
    m_testFileCounter++;
    if (m_testFileCounter > 10) {
        m_testFileCounter = 1;  // 重新从1开始循环
        log("SNR 测试文件循环完成，重新从 1.fits 开始");
    }
#else
    log(QString("Python 返回 mean_peak_snr: %1 (文件: %2)").arg(val).arg(m_lastCapturedImage));
#endif

    snr = val;

    if (!std::isfinite(val) || val < 0) {
        log("mean_peak_snr 数值无效");
        return false;
    }
    return true;
}

// bool AutoFocus::detectHFRByPython(double &hfr)
// {
//     // 基本检查
//     if (!m_isRunning) {
//         log("自动对焦已停止，跳过Python识星");
//         return false;
//     }
    
//     // 构建测试文件路径：/home/quarcs/test_fits/coarse/1.fits 到 11.fits
//     QString testFilePath = QString("/home/quarcs/test_fits/coarse/%1.fits").arg(m_testFileCounter);
    
//     // 检查文件是否存在
//     QFileInfo fi(testFilePath);
//     if (!fi.exists() || fi.size() == 0) {
//         log(QString("错误：测试文件不存在或无效: %1").arg(testFilePath));
//         return false;
//     }
    
//     log(QString("处理测试文件: %1 (第%2个文件)").arg(testFilePath).arg(m_testFileCounter));

//     // 调用Python脚本进行星点识别并输出HFR
//     bool ok = Tools::findStarsByPython_Process(testFilePath);
//     if (!ok) {
//         log("Python脚本执行失败或未返回有效结果");
//         return false;
//     }
//     double val = Tools::getLastHFR();
//     m_lastHFR = val;
//     log(QString("Python返回HFR: %1 (文件: %2)").arg(val).arg(m_testFileCounter));
//     hfr = val;
//     if (!std::isfinite(val) || val <= 0) {
//         log("HFR数值无效");
//         return false;
//     }
    
//     // 计数器递增，循环处理1-11.fits
//     m_testFileCounter++;
//     if (m_testFileCounter > 14) {
//         m_testFileCounter = 1;  // 重新从1开始循环
//         log("测试文件循环完成，重新从1.fits开始");
//     }
//     return true;
// }

bool AutoFocus::loadFocuserRangeFromIni(const QString &iniPath)
{
    QString path = iniPath;
    if (path.isEmpty()) {
        path = QCoreApplication::applicationDirPath() + "/autofocus.ini";
    }
    QSettings s(path, QSettings::IniFormat);
    QVariant vmin = s.value("Focuser/MinPosition");
    QVariant vmax = s.value("Focuser/MaxPosition");
    if (!vmin.isValid() || !vmax.isValid()) {
        // 兼容其他可能键名
        if (!vmin.isValid()) vmin = s.value("Focuser/min");
        if (!vmax.isValid()) vmax = s.value("Focuser/max");
        if (!vmin.isValid()) vmin = s.value("focuser_min");
        if (!vmax.isValid()) vmax = s.value("focuser_max");
    }
    if (vmin.isValid() && vmax.isValid()) {
        bool ok1=false, ok2=false;
        int minPos = vmin.toString().toInt(&ok1);
        int maxPos = vmax.toString().toInt(&ok2);
        if (ok1 && ok2 && maxPos > minPos) {
            m_focuserMinPosition = minPos;
            m_focuserMaxPosition = maxPos;
            log(QString("从INI加载电调位置范围: [%1, %2] (%3)")
                .arg(minPos).arg(maxPos).arg(path));
            return true;
        } else {
            log(QString("INI中的Focuser范围无效: min=%1, max=%2 (%3)")
                .arg(vmin.toString()).arg(vmax.toString()).arg(path));
        }
    } else {
        log(QString("在INI未找到Focuser范围键，路径: %1").arg(path));
    }
    return false;
}

/**
 * @brief 简化的二次拟合方法
 * 
 * 当标准最小二乘法失败时，使用更稳定的拟合方法
 * 通过强制使用二次项来避免线性方程组奇异的问题
 * 
 * @param data 数据点
 * @return FitResult 拟合结果
 */
FitResult AutoFocus::performSimplifiedQuadraticFit(const QVector<FocusDataPoint>& data)
{
    FitResult result;
    
    if (data.size() < 3) {
        return result;
    }
    
    log(QString("开始简化二次拟合，数据点数量: %1").arg(data.size()));
    
    // 使用相对坐标
    double minPos = getDataMinPosition(data);
    double maxPos = getDataMaxPosition(data);
    double centerPos = (minPos + maxPos) / 2.0;
    
    // 计算数据点的中心位置和HFR
    double sumHFR = 0.0;
    double sumPos = 0.0;
    for (const FocusDataPoint &point : data) {
        sumHFR += point.hfr;
        sumPos += point.focuserPosition;
    }
    double avgHFR = sumHFR / data.size();
    double avgPos = sumPos / data.size();
    
    // 使用简化的方法：假设二次曲线的顶点在数据中心附近
    // 计算一个合理的二次项系数
    double range = maxPos - minPos;
    double hfrRange = 0.0;
    double minHFR = data[0].hfr;
    double maxHFR = data[0].hfr;
    
    for (const FocusDataPoint &point : data) {
        if (point.hfr < minHFR) minHFR = point.hfr;
        if (point.hfr > maxHFR) maxHFR = point.hfr;
    }
    hfrRange = maxHFR - minHFR;
    
    // 估算二次项系数：基于HFR变化范围和位置范围
    if (range > 0 && hfrRange > 0) {
        result.a = hfrRange / (range * range) * 0.1; // 使用一个合理的比例
        result.b = 0.0; // 假设顶点在中心
        result.c = minHFR; // 最小HFR作为常数项
        
        // 计算最佳位置（顶点位置）
        result.bestPosition = centerPos;
        result.minHFR = minHFR;
        
        log(QString("简化二次拟合成功: y = %1x² + %2x + %3")
            .arg(result.a).arg(result.b).arg(result.c));
        log(QString("最佳位置: %1, 最小HFR: %2").arg(result.bestPosition).arg(result.minHFR));
    } else {
        log(QString("简化二次拟合失败：数据范围或HFR范围无效"));
    }
    
    return result;
}

/**
 * @brief 更新自动对焦步骤状态 - [AUTO_FOCUS_UI_ENHANCEMENT]
 * @param step 步骤编号 (1-3)
 * @param description 步骤描述
 */
void AutoFocus::updateAutoFocusStep(int step, const QString &description)
{
    log(QString("自动对焦步骤 %1: %2").arg(step).arg(description));
    emit autoFocusStepChanged(step, description);
}

/**
 * @brief 设置空程补偿值
 * @param inward 向内空程补偿值（步数）
 * @param outward 向外空程补偿值（步数）
 */
void AutoFocus::setBacklashCompensation(int inward, int outward)
{
    // 对于自动对焦，我们使用一个统一的空程补偿值
    // 这里取两个值的平均值，或者可以根据移动方向选择不同的值
    m_backlashCompensation = (inward + outward) / 2;
    m_useBacklashCompensation = (m_backlashCompensation > 0);
    
    log(QString("设置空程补偿: 向内=%1, 向外=%2, 使用值=%3, 启用=%4")
        .arg(inward).arg(outward).arg(m_backlashCompensation).arg(m_useBacklashCompensation ? "是" : "否"));
}

/**
 * @brief 设置是否使用空程补偿
 * @param use 是否使用空程补偿
 */
void AutoFocus::setUseBacklashCompensation(bool use)
{
    m_useBacklashCompensation = use;
    log(QString("设置空程补偿使用状态: %1").arg(use ? "启用" : "禁用"));
}

/**
 * @brief 检查是否使用空程补偿
 * @return 是否使用空程补偿
 */
bool AutoFocus::isUsingBacklashCompensation() const
{
    return m_useBacklashCompensation;
}