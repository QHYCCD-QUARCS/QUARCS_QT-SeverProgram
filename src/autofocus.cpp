#include "autofocus.h"
#include <QDebug>
#include <QThread>
#include <QtMath>
#include <QDateTime>
#include <QFileInfo>
#include <string>
#include <limits>
#include <cmath>
#include <algorithm>
#include "myclient.h"  // MyClient类定义
#include "qhyccd.h"    // QHYCCD_SUCCESS常量
#include "tools.h"   // Tools类
#include <stellarsolver.h> // FITSImage类来自stellarsolver

// ==================== 自动对焦配置结构体 ====================
struct AutoFocusConfig {
    // HFR相关参数
    double hfrThreshold = 3.0;                    // HFR阈值，超过此值进入粗调
    
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
    int moveTimeout = 3000;                       // 移动超时时间(ms)
    int stuckTimeout = 1000;                      // 卡住检测时间(ms)
    
    // 数据拟合参数
    double minRSquared = 0.7;                     // 最小拟合质量R²
    int minDataPoints = 3;                        // 最小数据点数量
    
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
                     QObject *parent)
    : QObject(parent)
    , m_indiServer(indiServer)                       // INDI客户端对象
    , m_dpFocuser(dpFocuser)                         // 电调设备对象
    , m_dpMainCamera(dpMainCamera)                   // 主相机设备对象
    , m_currentState(AutoFocusState::IDLE)           // 初始状态为空闲
    , m_timer(new QTimer(this))                      // 创建定时器用于状态处理
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
    , m_topStarCount(g_autoFocusConfig.topStarCount) // 选择置信度最高的星点数量
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
    , m_lastFitResult()                            // 初始化拟合结果
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
{
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
 * @brief 开始自动对焦流程
 * 
 * 这是自动对焦的主要入口函数，执行以下步骤：
 * 1. 检查是否已在运行
 * 2. 检查电调连接状态
 * 3. 初始化所有参数
 * 4. 启动定时器开始状态机处理
 */
void AutoFocus::startAutoFocus()
{
    QMutexLocker locker(&m_mutex);  // 线程安全锁
    
    if (m_isRunning) {
        log(QString("自动对焦已在运行中"));
        return;
    }

    // 检查设备对象是否有效
    if (!m_dpFocuser) {
        log(QString("错误: 电调设备对象为空，无法开始自动对焦"));
        emit errorOccurred("电调设备未连接");
        return;
    }
    
    if (!m_dpMainCamera) {
        log(QString("错误: 主相机设备对象为空，无法开始自动对焦"));
        emit errorOccurred("主相机设备未连接");
        return;
    }
    
    if (!m_indiServer) {
        log(QString("错误: INDI客户端对象为空，无法开始自动对焦"));
        emit errorOccurred("INDI客户端未连接");
        return;
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
    m_topStarCount = 5; // 选择置信度最高的5颗星
    
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
    
    // 获取电调位置范围
    if (m_dpFocuser) {
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
    
    // 切换到初始状态
    changeState(AutoFocusState::CHECKING_STARS);
    
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

void AutoFocus::stopAutoFocus()
{
    try {
        log("开始停止自动对焦");
        
        QMutexLocker locker(&m_mutex);
        
        if (!m_isRunning) {
            log("自动对焦未在运行，无需停止");
            return;
        }

        // 立即重置运行状态，防止定时器继续调用processCurrentState
        m_isRunning = false;
        log("运行状态已重置");
        
        // 立即停止定时器，防止任何回调继续执行
        if (m_timer) {
            m_timer->stop();
            log("定时器已停止");
        }
        
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
    
    log(QString("拍摄全图并检查星点..."));
    
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

    // 检测星点并计算HFR
    if (detectStarsInImage()) {
        log(QString("检测到星点，计算HFR..."));
        
        // 选择置信度最高的星点并计算平均HFR
        double hfr = selectTopStarsAndCalculateHFR();
        log(QString("当前HFR: %1").arg(hfr));
        
        if (hfr > m_hfrThreshold) {
            log(QString("HFR大于阈值，进入粗调模式"));
            startCoarseAdjustment();
        } else {
            log(QString("HFR小于阈值，进入精调模式"));
            startFineAdjustment();
        }
    } else {
        log(QString("未检测到星点，进入大范围找星流程"));
        startLargeRangeSearch();
    }
}

void AutoFocus::startLargeRangeSearch()
{
    changeState(AutoFocusState::LARGE_RANGE_SEARCH);
    m_currentLargeRangeShots = 0;
    m_currentLargeRangeStep = g_autoFocusConfig.initialLargeRangeStep; // 使用配置的初始步长
    
    // 获取当前电调位置
    m_currentPosition = getCurrentFocuserPosition();
    
    // 计算距离两个极限位置的距离
    int distanceToMin = m_currentPosition - m_focuserMinPosition;
    int distanceToMax = m_focuserMaxPosition - m_currentPosition;
    
    // 根据距离最近的极限位置决定初始搜索方向
    if (distanceToMin <= distanceToMax) {
        // 距离最小位置更近，先移动到最小位置，然后向外搜索
        m_searchDirection = 1; // 向外搜索
        log(QString("当前位置 %1 距离最小位置更近，先移动到最小位置，然后向外搜索").arg(m_currentPosition));
        m_initialTargetPosition = m_focuserMinPosition; // 设置初始目标位置
    } else {
        // 距离最大位置更近，先移动到最大位置，然后向内搜索
        m_searchDirection = -1; // 向内搜索
        log(QString("当前位置 %1 距离最大位置更近，先移动到最大位置，然后向内搜索").arg(m_currentPosition));
        m_initialTargetPosition = m_focuserMaxPosition; // 设置初始目标位置
    }
    
    log(QString("开始大范围找星，初始步长: %1%，搜索方向: %2").arg(m_currentLargeRangeStep).arg(m_searchDirection > 0 ? "向外" : "向内"));
}

void AutoFocus::processLargeRangeSearch()
{
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过大范围搜索");
        return;
    }
    
    if (m_currentLargeRangeShots >= m_maxLargeRangeShots) {
        handleError(QString("大范围找星失败：拍摄%1次仍未识别到星点").arg(m_maxLargeRangeShots));
        return;
    }

    // 获取当前电调位置
    m_currentPosition = getCurrentFocuserPosition();
    
    // 检查是否正在等待电调移动完成
    if (m_waitingForMove) {
        // 使用统一的等待函数，使用配置参数
        bool moveSuccess = waitForFocuserMoveComplete(m_targetFocuserPosition, 
                                                    g_autoFocusConfig.positionTolerance, 
                                                    g_autoFocusConfig.moveTimeout / 1000, 
                                                    g_autoFocusConfig.stuckTimeout / 1000);
        
        // 清除等待状态
        m_waitingForMove = false;
        m_isFocuserMoving = false;
        
        // 移动完成后，检查是否需要减少步长
        checkAndReduceStepSize();
        
        // 无论移动是否成功，都要拍摄检查星点
        log("电调移动完成，开始拍摄检查星点");
        if (!captureFullImage()) {
            handleError("拍摄图像失败");
            return;
        }
        
        // 等待拍摄完成
        if (!waitForCaptureComplete()) {
            handleError("拍摄超时");
            return;
        }

        if (detectStarsInImage()) {
            log("大范围找星成功，检测到星点");
            changeState(AutoFocusState::CHECKING_STARS);
            return;
        }
        
        log("未检测到星点，继续搜索");
        // 注意：这里不增加搜索计数，因为后续逻辑会处理
        return;
    }
    
    // 增加搜索计数（只在非等待状态下增加）
    m_currentLargeRangeShots++;
    log(QString("大范围找星第%1次尝试").arg(m_currentLargeRangeShots));
    
    // 如果是第一次尝试，先移动到初始目标位置
    if (m_currentLargeRangeShots == 1) {
        log(QString("第一次尝试，移动到初始目标位置: %1").arg(m_initialTargetPosition));
        
        // 检查是否需要移动
        if (m_currentPosition != m_initialTargetPosition) {
            // 使用统一的电调移动函数
            executeFocuserMove(m_initialTargetPosition, "第一次尝试移动到初始目标位置");
            
            // 立即检查电调状态
            QThread::msleep(100); // 等待100ms让命令生效
            int afterMovePosition = getCurrentFocuserPosition();
            log(QString("移动命令发送后电调位置: %1").arg(afterMovePosition));
            
            return; // 返回，让定时器处理等待
        } else {
            log(QString("当前位置已经是目标位置，无需移动"));
            
            // 即使不需要移动，也要拍摄检查星点
            if (!captureFullImage()) {
                handleError("拍摄图像失败");
                return;
            }
            
            // 等待拍摄完成
            if (!waitForCaptureComplete()) {
                handleError("拍摄超时");
                return;
            }

            if (detectStarsInImage()) {
                log("大范围找星成功，检测到星点");
                changeState(AutoFocusState::CHECKING_STARS);
                return;
            }

            // 第一次尝试完成，继续当前方向搜索
            log(QString("第一次尝试完成，继续当前方向搜索"));
            // 注意：这里不增加搜索计数，因为已经在前面增加了
        }
    }
    
    // 检查是否已达到位置限制并处理往返搜索
    if (m_currentPosition >= m_focuserMaxPosition && m_searchDirection > 0) {
        log(QString("已达到最大位置 %1，开始向内搜索").arg(m_currentPosition));
        m_searchDirection = -1; // 改变搜索方向向内
        // 计算向内移动的步数
        int totalRange = m_focuserMaxPosition - m_focuserMinPosition;
        int moveSteps = static_cast<int>(totalRange * m_currentLargeRangeStep / 100.0);
        int targetPosition = m_currentPosition - moveSteps;
        
        // 确保不超出最小位置
        if (targetPosition < m_focuserMinPosition) {
            targetPosition = m_focuserMinPosition;
        }
        
        // 计算移动方向和步数
        int actualMoveSteps = targetPosition - m_currentPosition;
        bool isInward = (actualMoveSteps < 0);
        
        if (isInward) {
            actualMoveSteps = -actualMoveSteps; // 取绝对值
        }
        
        log(QString("向内搜索: 从位置 %1 移动到 %2，方向: 向内，步数: %3").arg(m_currentPosition).arg(targetPosition).arg(actualMoveSteps));
        
        // 设置移动状态
        m_isFocuserMoving = true;
        m_targetFocuserPosition = targetPosition;
        m_moveStartTime = QDateTime::currentMSecsSinceEpoch();
        m_lastPosition = m_currentPosition;
        
        // 设置移动方向并发送移动命令
        m_indiServer->setFocuserMoveDiretion(m_dpFocuser, isInward);
        m_indiServer->moveFocuserSteps(m_dpFocuser, actualMoveSteps);
        
        // 初始化电调判断参数
        initializeFocuserMoveParameters();
        
        log(QString("移动命令详情: 方向=%1, 步数=%2, 目标位置=%3").arg(isInward ? "向内" : "向外").arg(actualMoveSteps).arg(targetPosition));
        
        // 立即检查电调状态
        QThread::msleep(100); // 等待100ms让命令生效
        int afterMovePosition = getCurrentFocuserPosition();
        log(QString("移动命令发送后电调位置: %1").arg(afterMovePosition));
        
        // 设置非阻塞等待状态
        m_waitingForMove = true;
        m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
        m_moveWaitCount = 0;
        m_moveLastPosition = m_currentPosition;
        log(QString("开始等待电调移动完成，目标位置: %1").arg(targetPosition));
        return; // 返回，让定时器处理等待
    } else if (m_currentPosition <= m_focuserMinPosition && m_searchDirection < 0) {
        log(QString("已达到最小位置 %1，开始向外搜索").arg(m_currentPosition));
        m_searchDirection = 1; // 改变搜索方向向外
        // 计算向外移动的步数
        int totalRange = m_focuserMaxPosition - m_focuserMinPosition;
        int moveSteps = static_cast<int>(totalRange * m_currentLargeRangeStep / 100.0);
        int targetPosition = m_currentPosition + moveSteps;
        
        // 确保不超出最大位置
        if (targetPosition > m_focuserMaxPosition) {
            targetPosition = m_focuserMaxPosition;
        }
        
        // 计算移动方向和步数
        int actualMoveSteps2 = targetPosition - m_currentPosition;
        bool isInward = (actualMoveSteps2 < 0);
        
        if (isInward) {
            actualMoveSteps2 = -actualMoveSteps2; // 取绝对值
        }
        
        log(QString("向外搜索: 从位置 %1 移动到 %2，方向: 向外，步数: %3").arg(m_currentPosition).arg(targetPosition).arg(actualMoveSteps2));
        
        // 设置移动状态
        m_isFocuserMoving = true;
        m_targetFocuserPosition = targetPosition;
        m_moveStartTime = QDateTime::currentMSecsSinceEpoch();
        m_lastPosition = m_currentPosition;
        
        // 设置移动方向并发送移动命令
        m_indiServer->setFocuserMoveDiretion(m_dpFocuser, isInward);
        m_indiServer->moveFocuserSteps(m_dpFocuser, actualMoveSteps2);
        
        // 初始化电调判断参数
        initializeFocuserMoveParameters();
        
        // 设置非阻塞等待状态，让定时器处理等待
        m_waitingForMove = true;
        m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
        m_moveWaitCount = 0;
        m_moveLastPosition = m_currentPosition;
        log(QString("开始等待电调移动完成，目标位置: %1").arg(targetPosition));
        return; // 返回，让定时器处理等待
    } else {
        // 正常搜索：按照当前方向移动
        int totalRange = m_focuserMaxPosition - m_focuserMinPosition;
        int moveSteps = static_cast<int>(totalRange * m_currentLargeRangeStep / 100.0) * m_searchDirection;
        
        // 检查移动后是否会超出限制
        int targetPosition = m_currentPosition + moveSteps;
        if (targetPosition > m_focuserMaxPosition) {
            log(QString("警告: 目标位置 %1 大于最大位置 %2，限制到最大位置").arg(targetPosition).arg(m_focuserMaxPosition));
            targetPosition = m_focuserMaxPosition;
        } else if (targetPosition < m_focuserMinPosition) {
            log(QString("警告: 目标位置 %1 小于最小位置 %2，限制到最小位置").arg(targetPosition).arg(m_focuserMinPosition));
            targetPosition = m_focuserMinPosition;
        }
        
        // 计算移动方向和步数
        int actualMoveSteps3 = targetPosition - m_currentPosition;
        bool isInward = (actualMoveSteps3 < 0);
        
        if (isInward) {
            actualMoveSteps3 = -actualMoveSteps3; // 取绝对值
        }
        
        log(QString("电调移动: 当前位置=%1, 目标位置=%2, 方向=%3, 步数=%4").arg(m_currentPosition).arg(targetPosition).arg(m_searchDirection > 0 ? "向外" : "向内").arg(actualMoveSteps3));
        
        // 设置移动状态
        m_isFocuserMoving = true;
        m_targetFocuserPosition = targetPosition;
        m_moveStartTime = QDateTime::currentMSecsSinceEpoch();
        m_lastPosition = m_currentPosition;
        
        // 设置移动方向并发送移动命令
        m_indiServer->setFocuserMoveDiretion(m_dpFocuser, isInward);
        m_indiServer->moveFocuserSteps(m_dpFocuser, actualMoveSteps3);
        
        // 初始化电调判断参数
        initializeFocuserMoveParameters();
        
        log(QString("移动命令详情: 方向=%1, 步数=%2, 目标位置=%3").arg(isInward ? "向内" : "向外").arg(actualMoveSteps3).arg(targetPosition));
        
        // 立即检查电调状态
        QThread::msleep(100); // 等待100ms让命令生效
        int afterMovePosition = getCurrentFocuserPosition();
        log(QString("移动命令发送后电调位置: %1").arg(afterMovePosition));
        
        // 设置非阻塞等待状态，让定时器处理等待
        m_waitingForMove = true;
        m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
        m_moveWaitCount = 0;
        m_moveLastPosition = m_currentPosition;
        log(QString("开始等待电调移动完成，目标位置: %1").arg(targetPosition));
        return; // 返回，让定时器处理等待
    }
    
    // 额外的安全检查：确保电调设备仍然有效
    if (!m_dpFocuser || !m_indiServer) {
        log(QString("错误: 电调设备或INDI客户端已失效"));
        handleError("电调设备连接已断开");
        return;
    }
    
    // 拍摄并等待完成
    if (!captureFullImage()) {
        handleError("拍摄图像失败");
        return;
    }
    
    // 等待拍摄完成
    if (!waitForCaptureComplete()) {
        handleError("拍摄超时");
        return;
    }

    if (detectStarsInImage()) {
        log("大范围找星成功，检测到星点");
        changeState(AutoFocusState::CHECKING_STARS);
        return;
    }

    // 步长减少逻辑已移到checkAndReduceStepSize()函数中
    // 在电调移动完成后调用
}

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
    changeState(AutoFocusState::COARSE_ADJUSTMENT);
    m_focusData.clear();
    m_dataCollectionCount = 0;
    log("开始粗调模式，步长1000步");
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
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过粗调");
        return;
    }
    
    // 检查是否收集了足够的数据点
    if (m_dataCollectionCount >= g_autoFocusConfig.minDataPoints) {
        log(QString("粗调数据收集完成，开始拟合"));
        changeState(AutoFocusState::FITTING_DATA);
        return;
    }

    // 移动电调到下一个位置
    moveFocuser(m_coarseStepSize);
    
    // 设置等待电调移动完成的状态
    if (m_isFocuserMoving) {
        log(QString("电调开始移动，等待移动完成"));
        m_waitingForMove = true;
        m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
        m_moveWaitCount = 0;
        m_moveLastPosition = m_currentPosition;
        // 目标位置已经在moveFocuser中设置，这里不需要重复设置
        return; // 返回，让定时器处理等待
    }
    
    // 电调没有移动，直接进行拍摄
    log(QString("电调无需移动，直接进行拍摄"));
    
    // 执行粗调数据收集逻辑
    performCoarseDataCollection();
}

void AutoFocus::performCoarseDataCollection()
{
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过粗调数据收集");
        return;
    }
    
    // 拍摄图像并等待完成
    if (!captureFullImage()) {
        handleError("拍摄图像失败");
        return;
    }
    
    // 再次检查运行状态
    if (!m_isRunning) {
        log("自动对焦已停止，跳过拍摄等待");
        return;
    }
    
    // 等待拍摄完成
    if (!waitForCaptureComplete()) {
        handleError("拍摄超时");
        return;
    }

    // 再次检查运行状态
    if (!m_isRunning) {
        log("自动对焦已停止，跳过星点检测");
        return;
    }

    // 检测星点并计算HFR
    if (!detectStarsInImage()) {
        handleError("无法检测到星点");
        return;
    }
    
    // 再次检查运行状态
    if (!m_isRunning) {
        log("自动对焦已停止，跳过HFR计算");
        return;
    }
    
    // 选择置信度最高的星点并计算平均HFR
    double hfr = selectTopStarsAndCalculateHFR();
    int currentPos = getCurrentFocuserPosition();
    
    // 创建数据点并添加到数据集
    FocusDataPoint dataPoint(currentPos, hfr);
    m_focusData.append(dataPoint);
    m_dataCollectionCount++;
    
    // 记录数据点信息
    log(QString("粗调数据点%1: 位置=%2, HFR=%3")
        .arg(m_dataCollectionCount).arg(currentPos).arg(hfr));
}

void AutoFocus::performFineDataCollection()
{
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过精调数据收集");
        return;
    }
    
    // 每个位置拍摄多张图像以提高精度
    QVector<double> measurements;
    for (int i = 0; i < m_fineShotsPerPosition; ++i) {
        // 再次检查运行状态
        if (!m_isRunning) {
            log("自动对焦已停止，跳过精调拍摄");
            return;
        }
        
        // 精调使用ROI拍摄以提高效率
        if (!captureROIImage()) {
            handleError("拍摄ROI图像失败");
            return;
        }
        
        // 再次检查运行状态
        if (!m_isRunning) {
            log("自动对焦已停止，跳过拍摄等待");
            return;
        }
        
        // 等待拍摄完成
        if (!waitForCaptureComplete()) {
            handleError("拍摄超时");
            return;
        }
        
        // 再次检查运行状态
        if (!m_isRunning) {
            log("自动对焦已停止，跳过HFR计算");
            return;
        }
        
        // 检测星点并计算HFR
        if (!detectStarsInImage()) {
            handleError("无法检测到星点");
            return;
        }
        
        // 选择置信度最高的星点并计算平均HFR
        double hfr = selectTopStarsAndCalculateHFR();
        measurements.append(hfr);
        
        log(QString("精调拍摄%1: HFR=%2").arg(i + 1).arg(hfr));
    }
    
    // 再次检查运行状态
    if (!m_isRunning) {
        log("自动对焦已停止，跳过数据点创建");
        return;
    }
    
    // 计算多次拍摄的平均HFR值
    double avgHFR = 0.0;
    for (double hfr : measurements) {
        avgHFR += hfr;
    }
    avgHFR /= measurements.size();
    
    // 创建数据点并保存所有测量值
    int currentPos = getCurrentFocuserPosition();
    FocusDataPoint dataPoint(currentPos, avgHFR);
    dataPoint.measurements = measurements;  // 保存所有测量值用于后续分析
    m_focusData.append(dataPoint);
    m_dataCollectionCount++;
    
    // 记录数据点信息
    log(QString("精调数据点%1: 位置=%2, 平均HFR=%3")
        .arg(m_dataCollectionCount).arg(currentPos).arg(avgHFR));
}

void AutoFocus::startFineAdjustment()
{
    changeState(AutoFocusState::FINE_ADJUSTMENT);
    m_focusData.clear();
    m_dataCollectionCount = 0;
    log("开始精调模式，步长100步");
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
    
    // 检查是否收集了足够的数据点
    if (m_dataCollectionCount >= g_autoFocusConfig.minDataPoints) {
        log(QString("精调数据收集完成，开始拟合"));
        changeState(AutoFocusState::FITTING_DATA);
        return;
    }

    // 移动电调到下一个位置
    moveFocuser(m_fineStepSize);
    
    // 设置等待电调移动完成的状态
    if (m_isFocuserMoving) {
        log(QString("电调开始移动，等待移动完成"));
        m_waitingForMove = true;
        m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
        m_moveWaitCount = 0;
        m_moveLastPosition = m_currentPosition;
        // 目标位置已经在moveFocuser中设置，这里不需要重复设置
        return; // 返回，让定时器处理等待
    }
    
    // 电调没有移动，直接进行拍摄
    log(QString("电调无需移动，直接进行拍摄"));
    
    // 执行精调数据收集逻辑
    performFineDataCollection();
}

void AutoFocus::collectFocusData()
{
    // 这个方法在当前框架中可能不需要，因为数据收集在粗调和精调过程中完成
    changeState(AutoFocusState::FITTING_DATA);
}

void AutoFocus::processFittingData()
{
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过数据拟合");
        return;
    }
    
    log(QString("开始拟合对焦数据..."));
    
    FitResult result = fitFocusData();
    
    if (result.a == 0.0 && result.b == 0.0 && result.c == 0.0) {
        handleError("数据拟合失败");
        return;
    }
    
    log(QString("拟合结果: y = %1x² + %2x + %3")
        .arg(result.a).arg(result.b).arg(result.c));
    log(QString("最佳位置: %1, 最小HFR: %2")
        .arg(result.bestPosition).arg(result.minHFR));
    
    // 保存拟合结果供后续使用
    m_lastFitResult = result;
    
    changeState(AutoFocusState::MOVING_TO_BEST_POSITION);
    // 注意：moveToBestPosition只是设置位置，实际的移动会在processMovingToBestPosition中处理
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
    
    // 启动移动到最佳位置
    startFocuserMove(bestPosition);
    
    // 设置等待状态
    m_waitingForMove = true;
    m_moveWaitStartTime = QDateTime::currentMSecsSinceEpoch();
    m_moveWaitCount = 0;
    m_moveLastPosition = currentPos;
}

/**
 * @brief 拟合对焦数据
 * 
 * 使用二次函数 y = ax² + bx + c 拟合对焦曲线
 * 目标：找到HFR最小的最佳位置
 * 
 * 算法步骤：
 * 1. 数据预处理：去除异常值，标准化坐标
 * 2. 构建正规方程组：最小二乘法求解
 * 3. 求解线性方程组：高斯消元法
 * 4. 计算最佳位置：x = -b/(2a)
 * 5. 验证拟合质量：计算R²和残差
 * 
 * @return FitResult 拟合结果，包含最佳位置和最小HFR
 */
FitResult AutoFocus::fitFocusData()
{
    FitResult result;
    
    // 检查数据点是否足够进行拟合
    if (m_focusData.size() < g_autoFocusConfig.minDataPoints) {
        log(QString("数据点不足，无法进行拟合，需要至少%1个数据点").arg(g_autoFocusConfig.minDataPoints));
        return result;
    }
    
    log(QString("开始二次函数拟合，数据点数量: %1").arg(m_focusData.size()));
    
    // 数据预处理：去除异常值
    QVector<FocusDataPoint> cleanData = removeOutliers(m_focusData);
    if (cleanData.size() < 3) {
        log(QString("去除异常值后数据点不足，使用原始数据"));
        cleanData = m_focusData;
    }
    
    // 标准化坐标：将电调位置转换为相对坐标
    double minPos = cleanData[0].focuserPosition;
    double maxPos = cleanData[0].focuserPosition;
    for (const FocusDataPoint &point : cleanData) {
        minPos = qMin(minPos, static_cast<double>(point.focuserPosition));
        maxPos = qMax(maxPos, static_cast<double>(point.focuserPosition));
    }
    
    // 构建最小二乘法正规方程组
    // 对于 y = ax² + bx + c，需要求解：
    // Σx⁴a + Σx³b + Σx²c = Σx²y
    // Σx³a + Σx²b + Σxc = Σxy
    // Σx²a + Σxb + Σc = Σy
    
    double sum_x4 = 0.0, sum_x3 = 0.0, sum_x2 = 0.0, sum_x = 0.0, sum_1 = 0.0;
    double sum_x2y = 0.0, sum_xy = 0.0, sum_y = 0.0;
    
    for (const FocusDataPoint &point : cleanData) {
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
        log(QString("线性方程组求解失败，使用简单插值"));
        return findBestPositionByInterpolation();
    }
    
    // 提取系数（注意：x是相对坐标，需要转换回绝对坐标）
    result.a = coefficients[0];
    result.b = coefficients[1];
    result.c = coefficients[2];
    
    // 计算最佳位置：x = -b/(2a)
    if (qAbs(result.a) < 1e-10) {
        log(QString("二次项系数过小，拟合可能不准确"));
        return findBestPositionByInterpolation();
    }
    
    double bestRelativePos = -result.b / (2.0 * result.a);
    result.bestPosition = bestRelativePos + minPos; // 转换回绝对坐标
    
    // 计算最小HFR
    result.minHFR = result.a * bestRelativePos * bestRelativePos + 
                    result.b * bestRelativePos + result.c;
    
    // 验证拟合质量
    double rSquared = calculateRSquared(cleanData, result, minPos);
    log(QString("拟合质量 R² = %1").arg(rSquared));
    
    if (rSquared < g_autoFocusConfig.minRSquared) {
        log(QString("拟合质量较差 (R² < %1)，使用插值方法").arg(g_autoFocusConfig.minRSquared));
        return findBestPositionByInterpolation();
    }
    
    // 验证最佳位置是否在合理范围内
    if (result.bestPosition < minPos || result.bestPosition > maxPos) {
        log(QString("拟合的最佳位置超出数据范围，使用插值方法"));
        return findBestPositionByInterpolation();
    }
    
    log(QString("二次函数拟合成功: y = %1x² + %2x + %3")
        .arg(result.a).arg(result.b).arg(result.c));
    log(QString("最佳位置: %1, 最小HFR: %2")
        .arg(result.bestPosition).arg(result.minHFR));
    
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
        
        m_isRunning = false;
        
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
            
            emit autoFocusCompleted(true, bestPosition, minHFR);
            log("自动对焦完成");
        } else {
            emit autoFocusCompleted(false, 0.0, 0.0);
            log("自动对焦失败");
        }
        
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
    
    int elapsed = 0;
    while (!m_isCaptureEnd && elapsed < timeoutMs) {
        // 检查是否已停止
        if (!m_isRunning) {
            log("自动对焦已停止，中断拍摄等待");
            return false;
        }
        
        QThread::msleep(100);  // 等待100ms
        elapsed += 100;
    }
    
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
        
        return true;
    } else {
        log(QString("拍摄超时"));
        return false;
    }
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
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过星点检测");
        return false;
    }
    
    log(QString("开始检测星点..."));
    
    // 检查图像文件是否存在
    if (m_lastCapturedImage.isEmpty()) {
        log("错误：没有可用的图像文件");
        return false;
    }
    
    QFileInfo fileInfo(m_lastCapturedImage);
    if (!fileInfo.exists()) {
        log(QString("错误：图像文件不存在: %1").arg(m_lastCapturedImage));
        return false;
    }
    
    // 检查文件大小
    if (fileInfo.size() == 0) {
        log(QString("错误：图像文件为空: %1").arg(m_lastCapturedImage));
        return false;
    }
    
    try {
        // 使用StellarSolver进行星点检测
        QList<FITSImage::Star> stars = Tools::FindStarsByStellarSolver(true, true);
        
        // 添加调试代码：立即检查返回的stars列表
        log(QString("StellarSolver返回了 %1 个星点").arg(stars.size()));
        
        // 逐个检查星点的基本信息
        if (stars.size() > 0) {
            for (int i = 0; i < stars.size() && i < 5; ++i) {
                try {
                    const FITSImage::Star& star = stars.at(i);
                    log(QString("星点 %1: 位置(%2, %3), HFR=%4, 峰值=%5")
                        .arg(i).arg(star.x).arg(star.y).arg(star.HFR).arg(star.peak));
                } catch (const std::exception &e) {
                    log(QString("访问星点 %1 时发生异常: %2").arg(i).arg(e.what()));
                    break;
                } catch (...) {
                    log(QString("访问星点 %1 时发生未知异常").arg(i));
                    break;
                }
            }
        } else {
            log("没有检测到星点，跳过星点信息输出");
        }
        
        if (stars.size() > 0) {
            log(QString("检测到 %1 个星点").arg(stars.size()));
            return true;
        } else {
            log(QString("未检测到星点"));
            return false;
        }
    } catch (const std::exception &e) {
        log(QString("星点检测异常: %1").arg(e.what()));
        return false;
    } catch (...) {
        log("星点检测发生未知异常");
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
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过HFR计算");
        return 999.0;
    }
    
    log(QString("开始计算HFR..."));
    
    // 检查图像文件是否存在
    if (m_lastCapturedImage.isEmpty()) {
        log("错误：没有可用的图像文件");
        return 999.0;
    }
    
    QFileInfo fileInfo(m_lastCapturedImage);
    if (!fileInfo.exists()) {
        log(QString("错误：图像文件不存在: %1").arg(m_lastCapturedImage));
        return 999.0;
    }
    
    // 检查文件大小
    if (fileInfo.size() == 0) {
        log(QString("错误：图像文件为空: %1").arg(m_lastCapturedImage));
        return 999.0;
    }
    
    try {
        // 使用StellarSolver检测星点并计算HFR
        QList<FITSImage::Star> stars = Tools::FindStarsByStellarSolver(false, true);
        
        // 添加调试代码：立即检查返回的stars列表
        log(QString("HFR计算: StellarSolver返回了 %1 个星点").arg(stars.size()));
        
        // 逐个检查星点的基本信息
        if (stars.size() > 0) {
            for (int i = 0; i < stars.size() && i < 5; ++i) {
                try {
                    const FITSImage::Star& star = stars.at(i);
                    log(QString("HFR计算: 星点 %1: 位置(%2, %3), HFR=%4, 峰值=%5")
                        .arg(i).arg(star.x).arg(star.y).arg(star.HFR).arg(star.peak));
                } catch (const std::exception &e) {
                    log(QString("HFR计算: 访问星点 %1 时发生异常: %2").arg(i).arg(e.what()));
                    break;
                } catch (...) {
                    log(QString("HFR计算: 访问星点 %1 时发生未知异常").arg(i));
                    break;
                }
            }
        } else {
            log("HFR计算: 没有检测到星点，跳过星点信息输出");
        }
        
        if (stars.isEmpty()) {
            log(QString("未检测到星点，无法计算HFR"));
            return 999.0; // 返回一个很大的值表示对焦很差
        }
        
        // 计算所有星点的平均HFR
        double totalHFR = 0.0;
        int validStars = 0;
        
        for (const FITSImage::Star &star : stars) {
            if (star.HFR > 0) {
                totalHFR += star.HFR;
                validStars++;
            }
        }
        
        if (validStars > 0) {
            double avgHFR = totalHFR / validStars;
            log(QString("计算HFR完成: 平均HFR=%1, 有效星点数=%2").arg(avgHFR).arg(validStars));
            return avgHFR;
        } else {
            log(QString("没有有效的HFR数据"));
            return 999.0;
        }
    } catch (const std::exception &e) {
        log(QString("HFR计算异常: %1").arg(e.what()));
        return 999.0;
    } catch (...) {
        log("HFR计算发生未知异常");
        return 999.0;
    }
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
 * 使用IQR方法去除异常值，提高拟合质量
 * 
 * @param data 原始数据
 * @return QVector<FocusDataPoint> 清理后的数据
 */
QVector<FocusDataPoint> AutoFocus::removeOutliers(const QVector<FocusDataPoint>& data)
{
    if (data.size() < g_autoFocusConfig.minDataPoints + 1) {
        return data; // 数据点太少，不进行异常值检测
    }
    
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
    
    // 定义异常值边界
    double lowerBound = q1 - 1.5 * iqr;
    double upperBound = q3 + 1.5 * iqr;
    
    // 过滤异常值
    QVector<FocusDataPoint> cleanData;
    for (const FocusDataPoint &point : data) {
        if (point.hfr >= lowerBound && point.hfr <= upperBound) {
            cleanData.append(point);
        }
    }
    
    log(QString("异常值检测: 原始数据%1个点，清理后%2个点")
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
            log(QString("线性方程组奇异，无法求解"));
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
 * @brief 插值法找最佳位置
 * 
 * 当二次函数拟合失败时，使用简单的插值方法
 * 
 * @return FitResult 插值结果
 */
FitResult AutoFocus::findBestPositionByInterpolation()
{
    FitResult result;
    
    if (m_focusData.isEmpty()) {
        return result;
    }
    
    // 找到HFR最小的数据点
    double minHFR = m_focusData[0].hfr;
    int bestPos = m_focusData[0].focuserPosition;
    
    for (const FocusDataPoint &point : m_focusData) {
        if (point.hfr < minHFR) {
            minHFR = point.hfr;
            bestPos = point.focuserPosition;
        }
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
    
    // 检查位置限制
    if (targetPosition < m_focuserMinPosition) {
        log(QString("警告: 目标位置 %1 小于最小位置 %2，限制到最小位置").arg(targetPosition).arg(m_focuserMinPosition));
        targetPosition = m_focuserMinPosition;
    } else if (targetPosition > m_focuserMaxPosition) {
        log(QString("警告: 目标位置 %1 大于最大位置 %2，限制到最大位置").arg(targetPosition).arg(m_focuserMaxPosition));
        targetPosition = m_focuserMaxPosition;
    }
    m_currentPosition = getCurrentFocuserPosition();
    // 设置移动状态
    m_isFocuserMoving = true;
    m_targetFocuserPosition = targetPosition;
    m_moveStartTime = QDateTime::currentMSecsSinceEpoch();
    
    // 初始化位置变化检测
    m_lastPosition = m_currentPosition;
    
    // 发送移动命令
    m_indiServer->moveFocuserToAbsolutePosition(m_dpFocuser, targetPosition);
    
    // 初始化电调判断参数
    initializeFocuserMoveParameters();
    
    log(QString("开始电调移动: 当前位置=%1, 目标位置=%2").arg(m_currentPosition).arg(targetPosition));
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

    log(QString("获取电调位置: 成功=%1, 当前位置=%2").arg(success == 0 ? "是" : "否").arg(currentPosition));
    
    // 更新内部位置记录
    m_currentPosition = currentPosition;
    
    // 如果是第一次检查，初始化位置记录
    if (m_lastPosition == 0) {
        m_lastPosition = currentPosition;
        log(QString("初始化电调位置记录: 位置=%1").arg(currentPosition));
    }
    
            // 使用统一的位置检查函数
        if (isPositionReached(currentPosition, m_targetFocuserPosition, g_autoFocusConfig.positionTolerance)) {
            log(QString("电调移动完成: 当前位置=%1, 目标位置=%2").arg(currentPosition).arg(m_targetFocuserPosition));
            stopFocuserMove();
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
            log(QString("电调移动超时: 位置%1秒未变化，目标位置: %2")
                .arg(m_moveTimeout / 1000.0).arg(m_targetFocuserPosition));
            stopFocuserMove();
            return true; // 超时也算完成
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
    
    while (m_isRunning && m_moveWaitCount < dynamicTimeout * 10) { // 每100ms检查一次
        int currentPos = getCurrentFocuserPosition();
        m_moveWaitCount++;
        
        log(QString("等待电调移动 [%1/%2]: 当前位置=%3, 目标位置=%4")
            .arg(m_moveWaitCount / 10.0, 0, 'f', 1).arg(dynamicTimeout).arg(currentPos).arg(targetPosition));
        
        // 使用统一的位置检查函数
        if (isPositionReached(currentPos, targetPosition, tolerance)) {
            log(QString("电调移动完成: 当前位置=%1, 目标位置=%2, 误差=%3")
                .arg(currentPos).arg(targetPosition).arg(qAbs(currentPos - targetPosition)));
            return true;
        }
        
        // 检查是否卡住
        if (currentPos == m_moveLastPosition && m_moveWaitCount > dynamicStuckTimeout * 10) {
            log(QString("电调可能卡住: 位置%1秒未变化").arg(dynamicStuckTimeout));
            return false;
        }
        
        // 更新位置记录
        if (currentPos != m_moveLastPosition) {
            m_moveLastPosition = currentPos;
        }
        
        // 等待100ms
        QThread::msleep(100);
    }
    
    log(QString("电调移动超时: 等待%1秒后仍未到达目标位置").arg(dynamicTimeout));
    return false;
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
    // 立即检查运行状态，如果已停止则直接返回
    if (!m_isRunning) {
        log("自动对焦已停止，跳过星点选择");
        return 999.0;
    }
    
    log(QString("开始选择置信度最高的星点..."));
    
    // 检测图像中的所有星点
    QList<FITSImage::Star> stars = Tools::FindStarsByStellarSolver(false, true);
    
    if (stars.isEmpty()) {
        log(QString("未检测到星点，无法计算HFR"));
        return 999.0;
    }
    
    log(QString("检测到 %1 个星点，开始选择最佳星点").arg(stars.size()));
    
    // 选择置信度最高的星点
    QList<FITSImage::Star> topStars = selectTopStarsByConfidence(stars);
    
    if (topStars.isEmpty()) {
        log(QString("没有找到合适的星点"));
        return 999.0;
    }
    
    // 计算平均HFR值
    double totalHFR = 0.0;
    int validStars = 0;
    
    for (const FITSImage::Star &star : topStars) {
        if (star.HFR > 0) {
            totalHFR += star.HFR;
            validStars++;
            log(QString("选择星点: 位置(%1, %2), HFR=%3, 置信度=%4")
                .arg(star.x).arg(star.y).arg(star.HFR).arg(calculateStarConfidence(star), 0, 'f', 3));
        }
    }
    
    if (validStars > 0) {
        double avgHFR = totalHFR / validStars;
        log(QString("星点选择完成: 选择了%1颗星，平均HFR=%2").arg(validStars).arg(avgHFR));
        return avgHFR;
    } else {
        log(QString("没有有效的HFR数据"));
        return 999.0;
    }
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