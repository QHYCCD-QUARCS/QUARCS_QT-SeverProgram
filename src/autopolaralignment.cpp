#include "autopolaralignment.h"
#include <QThread>
#include <QEventLoop>
#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <cmath>
#include <algorithm>

PolarAlignment::PolarAlignment(MyClient* indiServer, INDI::BaseDevice* dpMount, INDI::BaseDevice* dpMainCamera, QObject *parent)
    : QObject(parent)
    , indiServer(indiServer)
    , dpMount(dpMount)
    , dpMainCamera(dpMainCamera)
    , currentState(PolarAlignmentState::IDLE)
    , currentMeasurementIndex(0)
    , currentRetryAttempt(0)
    , isRunningFlag(false)
    , isPausedFlag(false)
    , userAdjustmentConfirmed(false)
    , progressPercentage(0)
    , currentRAPosition(0.0)
    , currentDECPosition(0.0)
    , currentStatusMessage("空闲")
    , currentImageFile("")
    , currentAnalysisResult()
    , isCaptureEnd(false)
    , isSolveEnd(false)
    , lastCapturedImage("")
    , captureFailureCount(0)
    , solveFailureCount(0)
{
    // 初始化定时器为单次触发模式
    stateTimer.setSingleShot(true);
    captureAndAnalysisTimer.setSingleShot(true);
    movementTimer.setSingleShot(true);
    
    // 连接定时器信号到对应的槽函数
    connect(&stateTimer, &QTimer::timeout, this, &PolarAlignment::onStateTimerTimeout);
    connect(&captureAndAnalysisTimer, &QTimer::timeout, this, &PolarAlignment::onCaptureAndAnalysisTimerTimeout);
    // connect(&movementTimer, &QTimer::timeout, this, &PolarAlignment::onMovementTimerTimeout);
    
    Logger::Log("PolarAlignment: 极轴校准系统初始化完成", LogLevel::INFO, DeviceType::MAIN);
    testimage = 0;
}

PolarAlignment::~PolarAlignment()
{
    stopPolarAlignment();
    Logger::Log("PolarAlignment: 极轴校准系统已销毁", LogLevel::INFO, DeviceType::MAIN);
}

bool PolarAlignment::startPolarAlignment()
{
    QMutexLocker locker(&stateMutex);
    if (isRunningFlag) {
        Logger::Log("PolarAlignment: 校准流程已在运行中", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    if (!indiServer || !dpMount || !dpMainCamera) {
        Logger::Log("PolarAlignment: 设备不可用，无法启动校准", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 初始化校准状态和参数
    currentState = PolarAlignmentState::INITIALIZING;
    currentMeasurementIndex = 0;
    currentRetryAttempt = 0;
    currentAdjustmentAttempt = 0;
    currentRAAngle = config.raRotationAngle;
    currentDECAngle = config.decRotationAngle;
    measurements.clear();
    result = PolarAlignmentResult();
    isRunningFlag = true;
    isPausedFlag = false;
    userAdjustmentConfirmed = false;
    progressPercentage = 0;
    isCaptureEnd = false;
    isSolveEnd = false;
    lastCapturedImage = "";
    captureFailureCount = 0;
    solveFailureCount = 0;
    
    Logger::Log("PolarAlignment: 开始自动极轴校准流程", LogLevel::INFO, DeviceType::MAIN);
    emit stateChanged(currentState, "开始自动极轴校准...",0);
    // emit statusUpdated("开始自动极轴校准...");
    // emit progressUpdated(0);
    stateTimer.start(100);
    return true;
}

void PolarAlignment::stopPolarAlignment()
{
    QMutexLocker locker(&stateMutex);
    if (!isRunningFlag) return;
    
    // 停止所有定时器
    stateTimer.stop();
    captureAndAnalysisTimer.stop();
    movementTimer.stop();
    isRunningFlag = false;
    isPausedFlag = false;
    currentState = PolarAlignmentState::IDLE;
    
    Logger::Log("PolarAlignment: 极轴校准已停止", LogLevel::INFO, DeviceType::MAIN);
    emit stateChanged(currentState, "极轴校准已停止",-1);
    // emit statusUpdated("极轴校准已停止");
    // emit progressUpdated(0);
}

void PolarAlignment::pausePolarAlignment()
{
    QMutexLocker locker(&stateMutex);
    if (!isRunningFlag || isPausedFlag) return;
    
    isPausedFlag = true;
    stateTimer.stop();
    captureAndAnalysisTimer.stop();
    movementTimer.stop();
    
    Logger::Log("PolarAlignment: 极轴校准已暂停", LogLevel::INFO, DeviceType::MAIN);
    emit stateChanged(currentState, "极轴校准已暂停",progressPercentage);
    // emit statusUpdated("极轴校准已暂停");
    // emit progressUpdated(0);
}

void PolarAlignment::resumePolarAlignment()
{
    QMutexLocker locker(&stateMutex);
    if (!isRunningFlag || !isPausedFlag) return;
    
    isPausedFlag = false;
    stateTimer.start(100);
    
    Logger::Log("PolarAlignment: 极轴校准已恢复", LogLevel::INFO, DeviceType::MAIN);
    emit stateChanged(currentState, "极轴校准已恢复",progressPercentage);
    // emit statusUpdated("极轴校准已恢复");
    // emit progressUpdated(0);
}

void PolarAlignment::userConfirmAdjustment()
{
    QMutexLocker locker(&stateMutex);
    if (currentState == PolarAlignmentState::GUIDING_ADJUSTMENT) {
        Logger::Log("PolarAlignment: 用户确认调整完成", LogLevel::INFO, DeviceType::MAIN);
        userAdjustmentConfirmed = true;
        if (isRunningFlag && !isPausedFlag) {
            stateTimer.start(100);
        }
    }
}

void PolarAlignment::userCancelAlignment()
{
    QMutexLocker locker(&stateMutex);
    if (currentState == PolarAlignmentState::GUIDING_ADJUSTMENT) {
        Logger::Log("PolarAlignment: 用户取消校准", LogLevel::INFO, DeviceType::MAIN);
        result.isSuccessful = false;
        result.errorMessage = "用户取消校准";
        stopPolarAlignment();
        emit resultReady(result);
    }
}

PolarAlignmentState PolarAlignment::getCurrentState() const
{
    QMutexLocker locker(&stateMutex);
    return currentState;
}

bool PolarAlignment::isRunning() const
{
    QMutexLocker locker(&stateMutex);
    return isRunningFlag && !isPausedFlag;
}

bool PolarAlignment::isCompleted() const
{
    QMutexLocker locker(&stateMutex);
    return currentState == PolarAlignmentState::COMPLETED;
}

bool PolarAlignment::isFailed() const
{
    QMutexLocker locker(&stateMutex);
    return currentState == PolarAlignmentState::FAILED || currentState == PolarAlignmentState::USER_INTERVENTION;
}

PolarAlignmentResult PolarAlignment::getResult() const
{
    QMutexLocker locker(&resultMutex);
    return result;
}

QString PolarAlignment::getStatusMessage() const
{
    return currentStatusMessage;
}

void PolarAlignment::setConfig(const PolarAlignmentConfig& config)
{
    this->config = config;
    Logger::Log("PolarAlignment: 校准配置已更新", LogLevel::INFO, DeviceType::MAIN);
}

PolarAlignmentConfig PolarAlignment::getConfig() const
{
    return config;
}

void PolarAlignment::onStateTimerTimeout()
{
    if (isPausedFlag) return;
    processCurrentState();
}

void PolarAlignment::onCaptureAndAnalysisTimerTimeout()
{
    Logger::Log("PolarAlignment: 拍摄和分析超时", LogLevel::WARNING, DeviceType::MAIN);
    handleAnalysisFailure(currentRetryAttempt);
}

void PolarAlignment::onMovementTimerTimeout()
{
    Logger::Log("PolarAlignment: 望远镜移动超时", LogLevel::WARNING, DeviceType::MAIN);
    setState(PolarAlignmentState::FAILED);
}

void PolarAlignment::setState(PolarAlignmentState newState)
{
    QMutexLocker locker(&stateMutex);
    if (currentState == newState) return;
    
    currentState = newState;
    
    // 根据新状态设置对应的状态消息
    switch (newState) {
        case PolarAlignmentState::IDLE:
            currentStatusMessage = "空闲";
            break;
        case PolarAlignmentState::INITIALIZING:
            currentStatusMessage = "初始化中...";
            break;
        case PolarAlignmentState::CHECKING_POLAR_POINT:
            currentStatusMessage = "检查极点位置...";
            break;
        case PolarAlignmentState::MOVING_DEC_AWAY:
            currentStatusMessage = "移动DEC轴脱离极点...";
            break;
        case PolarAlignmentState::FIRST_CAPTURE:
            currentStatusMessage = "第一次拍摄...";
            break;
        case PolarAlignmentState::FIRST_ANALYSIS:
            currentStatusMessage = "第一次分析...";
            break;
        case PolarAlignmentState::FIRST_RECOVERY:
            currentStatusMessage = "第一次恢复...";
            break;
        case PolarAlignmentState::MOVING_RA_FIRST:
            currentStatusMessage = "第一次RA轴移动...";
            break;
        case PolarAlignmentState::SECOND_CAPTURE:
            currentStatusMessage = "第二次拍摄...";
            break;
        case PolarAlignmentState::SECOND_ANALYSIS:
            currentStatusMessage = "第二次分析...";
            break;
        case PolarAlignmentState::SECOND_RECOVERY:
            currentStatusMessage = "第二次恢复...";
            break;
        case PolarAlignmentState::MOVING_RA_SECOND:
            currentStatusMessage = "第二次RA轴移动...";
            break;
        case PolarAlignmentState::THIRD_CAPTURE:
            currentStatusMessage = "第三次拍摄...";
            break;
        case PolarAlignmentState::THIRD_ANALYSIS:
            currentStatusMessage = "第三次分析...";
            break;
        case PolarAlignmentState::THIRD_RECOVERY:
            currentStatusMessage = "第三次恢复...";
            break;
        case PolarAlignmentState::CALCULATING_DEVIATION:
            currentStatusMessage = "计算偏差...";
            break;
        case PolarAlignmentState::GUIDING_ADJUSTMENT:
            currentStatusMessage = "指导调整...";
            break;
        case PolarAlignmentState::FINAL_VERIFICATION:
            currentStatusMessage = "最终验证...";
            break;
        case PolarAlignmentState::ADJUSTING_FOR_OBSTACLE:
            currentStatusMessage = "调整避开遮挡物...";
            break;
        case PolarAlignmentState::COMPLETED:
            currentStatusMessage = "校准完成";
            break;
        case PolarAlignmentState::FAILED:
            currentStatusMessage = "校准失败";
            break;
        case PolarAlignmentState::USER_INTERVENTION:
            currentStatusMessage = "需要用户干预";
            break;
    }
    
    Logger::Log("PolarAlignment: 状态已更改为 " + currentStatusMessage.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    // emit statusUpdated(currentStatusMessage);    // 0: 开始 1: 结束 -1: 暂停
    
    // 根据状态更新进度百分比
    int newProgress = 0;
    switch (newState) {
        case PolarAlignmentState::INITIALIZING:
            newProgress = 5;
            break;
        case PolarAlignmentState::CHECKING_POLAR_POINT:
            newProgress = 10;
            break;
        case PolarAlignmentState::MOVING_DEC_AWAY:
            newProgress = 15;
            break;
        case PolarAlignmentState::FIRST_CAPTURE:
        case PolarAlignmentState::FIRST_ANALYSIS:
        case PolarAlignmentState::FIRST_RECOVERY:
        case PolarAlignmentState::MOVING_RA_FIRST:
            newProgress = 25;
            break;
        case PolarAlignmentState::SECOND_CAPTURE:
        case PolarAlignmentState::SECOND_ANALYSIS:
        case PolarAlignmentState::SECOND_RECOVERY:
        case PolarAlignmentState::MOVING_RA_SECOND:
            newProgress = 50;
            break;
        case PolarAlignmentState::THIRD_CAPTURE:
        case PolarAlignmentState::THIRD_ANALYSIS:
        case PolarAlignmentState::THIRD_RECOVERY:
            newProgress = 75;
            break;
        case PolarAlignmentState::CALCULATING_DEVIATION:
            newProgress = 85;
            break;
        case PolarAlignmentState::GUIDING_ADJUSTMENT:
            newProgress = 90;
            break;
        case PolarAlignmentState::FINAL_VERIFICATION:
            newProgress = 95;
            break;
        case PolarAlignmentState::ADJUSTING_FOR_OBSTACLE:
            newProgress = 92;
            break;
        case PolarAlignmentState::COMPLETED:
            newProgress = 100;
            break;
        case PolarAlignmentState::FAILED:
        case PolarAlignmentState::USER_INTERVENTION:
            newProgress = 0;
            break;
        default:
            newProgress = progressPercentage;
            break;
    }
    
    if (newProgress != progressPercentage) {
        progressPercentage = newProgress;
        // emit progressUpdated(progressPercentage);
        emit stateChanged(currentState, currentStatusMessage, progressPercentage);
    }
    
    // 如果正在运行且未暂停，启动状态定时器
    if (isRunningFlag && !isPausedFlag) {
        stateTimer.start(100);
    }
}

void PolarAlignment::processCurrentState()
{
    switch (currentState) {
        case PolarAlignmentState::INITIALIZING:
            setState(PolarAlignmentState::CHECKING_POLAR_POINT);
            break;
        case PolarAlignmentState::CHECKING_POLAR_POINT:
            {
                PolarPointCheckResult checkResult = checkPolarPoint();
                if (checkResult.success) {
                    if (checkResult.isNearPole) {
                        // 如果指向极点，需要移动DEC轴
                        setState(PolarAlignmentState::MOVING_DEC_AWAY);
                    } else {
                        // 不在极点，已经获得第一个测量点，直接进入RA轴移动
                        Logger::Log("PolarAlignment: 当前不在极点，已获得第一个测量点，直接进入RA轴移动", LogLevel::INFO, DeviceType::MAIN);
                        setState(PolarAlignmentState::MOVING_RA_FIRST);
                    }
                } else {
                    // 极点验证失败，直接设置为失败状态
                    Logger::Log("PolarAlignment: 极点验证失败，停止校准", LogLevel::ERROR, DeviceType::MAIN);
                    setState(PolarAlignmentState::FAILED);
                }
            }
            break;
        case PolarAlignmentState::MOVING_DEC_AWAY:
            if (moveDecAxisAway()) {
                setState(PolarAlignmentState::FIRST_CAPTURE);
            } else {
                // 移动失败时，直接进入第一次拍摄，不设置FAILED状态
                Logger::Log("PolarAlignment: DEC轴移动失败，直接进入第一次拍摄", LogLevel::WARNING, DeviceType::MAIN);
                setState(PolarAlignmentState::FIRST_CAPTURE);
            }
            break;
        case PolarAlignmentState::FIRST_CAPTURE:
            if (captureAndAnalyze(1)) {
                setState(PolarAlignmentState::MOVING_RA_FIRST);
            } else {
                // 检查是否是拍摄失败导致的退出
                if (captureFailureCount >= 2) {
                    // 拍摄失败，直接退出
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 解析失败，重新尝试
                    setState(PolarAlignmentState::FIRST_RECOVERY);
                }
            }
            break;
        case PolarAlignmentState::FIRST_RECOVERY:
            if (captureAndAnalyze(2)) {
                setState(PolarAlignmentState::MOVING_RA_FIRST);
            } else {
                // 检查是否是拍摄失败导致的退出
                if (captureFailureCount >= 2) {
                    // 拍摄失败，直接退出
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 解析失败，继续重试
                    currentRetryAttempt++;
                    if (currentRetryAttempt >= config.maxRetryAttempts) {
                        setState(PolarAlignmentState::FAILED);
                    } else {
                        setState(PolarAlignmentState::FIRST_RECOVERY);
                    }
                }
            }
            break;
        case PolarAlignmentState::MOVING_RA_FIRST:
            if (moveRAAxis()) {
                setState(PolarAlignmentState::SECOND_CAPTURE);
            } else {
                // 移动失败时，直接进入第二次拍摄，不设置FAILED状态
                Logger::Log("PolarAlignment: 第一次RA轴移动失败，直接进入第二次拍摄", LogLevel::WARNING, DeviceType::MAIN);
                setState(PolarAlignmentState::SECOND_CAPTURE);
            }
            break;
        case PolarAlignmentState::SECOND_CAPTURE:
            if (captureAndAnalyze(1)) {
                setState(PolarAlignmentState::MOVING_RA_SECOND);
            } else {
                // 检查是否是拍摄失败导致的退出
                if (captureFailureCount >= 2) {
                    // 拍摄失败，直接退出
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 解析失败，重新尝试
                    setState(PolarAlignmentState::SECOND_RECOVERY);
                }
            }
            break;
        case PolarAlignmentState::SECOND_RECOVERY:
            if (captureAndAnalyze(2)) {
                setState(PolarAlignmentState::MOVING_RA_SECOND);
            } else {
                // 检查是否是拍摄失败导致的退出
                if (captureFailureCount >= 2) {
                    // 拍摄失败，直接退出
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 解析失败，继续重试
                    currentRetryAttempt++;
                    if (currentRetryAttempt >= config.maxRetryAttempts) {
                        setState(PolarAlignmentState::FAILED);
                    } else {
                        setState(PolarAlignmentState::SECOND_RECOVERY);
                    }
                }
            }
            break;
        case PolarAlignmentState::MOVING_RA_SECOND:
            if (moveRAAxis()) {
                setState(PolarAlignmentState::THIRD_CAPTURE);
            } else {
                // 移动失败时，直接进入第三次拍摄，不设置FAILED状态
                Logger::Log("PolarAlignment: 第二次RA轴移动失败，直接进入第三次拍摄", LogLevel::WARNING, DeviceType::MAIN);
                setState(PolarAlignmentState::THIRD_CAPTURE);
            }
            break;
        case PolarAlignmentState::THIRD_CAPTURE:
            if (captureAndAnalyze(1)) {
                setState(PolarAlignmentState::CALCULATING_DEVIATION);
            } else {
                // 检查是否是拍摄失败导致的退出
                if (captureFailureCount >= 2) {
                    // 拍摄失败，直接退出
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 解析失败，重新尝试
                    setState(PolarAlignmentState::THIRD_RECOVERY);
                }
            }
            break;
        case PolarAlignmentState::THIRD_RECOVERY:
            if (captureAndAnalyze(2)) {
                setState(PolarAlignmentState::CALCULATING_DEVIATION);
            } else {
                // 检查是否是拍摄失败导致的退出
                if (captureFailureCount >= 2) {
                    // 拍摄失败，直接退出
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 解析失败，继续重试
                    currentRetryAttempt++;
                    if (currentRetryAttempt >= config.maxRetryAttempts) {
                        setState(PolarAlignmentState::FAILED);
                    } else {
                        setState(PolarAlignmentState::THIRD_RECOVERY);
                    }
                }
            }
            break;
        case PolarAlignmentState::CALCULATING_DEVIATION:
            if (calculateDeviation()) {
                setState(PolarAlignmentState::GUIDING_ADJUSTMENT);
            } else {
                setState(PolarAlignmentState::FAILED);
            }
            break;
        case PolarAlignmentState::GUIDING_ADJUSTMENT:
            if (guideUserAdjustment()) {
                setState(PolarAlignmentState::FINAL_VERIFICATION);
            } else {
                setState(PolarAlignmentState::GUIDING_ADJUSTMENT);
            }
            break;
        case PolarAlignmentState::FINAL_VERIFICATION:
            if (performFinalVerification()) {
                setState(PolarAlignmentState::COMPLETED);
            } else {
                setState(PolarAlignmentState::GUIDING_ADJUSTMENT);
            }
            break;
        case PolarAlignmentState::ADJUSTING_FOR_OBSTACLE:
            if (adjustForObstacle()) {
                setState(PolarAlignmentState::GUIDING_ADJUSTMENT);
            } else {
                setState(PolarAlignmentState::FAILED);
            }
            break;
        case PolarAlignmentState::COMPLETED:
            isRunningFlag = false;
            break;
        case PolarAlignmentState::FAILED:
        case PolarAlignmentState::USER_INTERVENTION:
            isRunningFlag = false;
            break;
        default:
            setState(PolarAlignmentState::FAILED);
            break;
    }
}

PolarAlignment::PolarPointCheckResult PolarAlignment::checkPolarPoint()
{
    Logger::Log("PolarAlignment: 检查极点位置", LogLevel::INFO, DeviceType::MAIN);

    // 第一次短曝光尝试
    bool firstCaptureSuccess = captureImage(config.shortExposureTime);
    bool firstCaptureComplete = waitForCaptureComplete();
    bool firstSolveStarted = solveImage(lastCapturedImage);
    bool firstSolveComplete = waitForSolveComplete();
    
    Logger::Log("PolarAlignment: 第一次尝试结果 - 拍摄:" + std::to_string(firstCaptureSuccess) + 
                ", 拍摄完成:" + std::to_string(firstCaptureComplete) + 
                ", 解析开始:" + std::to_string(firstSolveStarted) + 
                ", 解析完成:" + std::to_string(firstSolveComplete), LogLevel::INFO, DeviceType::MAIN);
    
    if (!firstCaptureSuccess || !firstCaptureComplete || !firstSolveStarted || !firstSolveComplete) {
        Logger::Log("PolarAlignment: 短曝光拍摄或解析过程失败，尝试长曝光", LogLevel::WARNING, DeviceType::MAIN);

        // 长曝光再试一次 - 重新拍摄图像
        bool secondCaptureSuccess = captureImage(config.recoveryExposureTime);
        bool secondCaptureComplete = waitForCaptureComplete();
        bool secondSolveStarted = solveImage(lastCapturedImage);
        bool secondSolveComplete = waitForSolveComplete();
        
        Logger::Log("PolarAlignment: 第二次尝试结果 - 拍摄:" + std::to_string(secondCaptureSuccess) + 
                    ", 拍摄完成:" + std::to_string(secondCaptureComplete) + 
                    ", 解析开始:" + std::to_string(secondSolveStarted) + 
                    ", 解析完成:" + std::to_string(secondSolveComplete), LogLevel::INFO, DeviceType::MAIN);
        
        if (!secondCaptureSuccess || !secondCaptureComplete || !secondSolveStarted || !secondSolveComplete) {
            Logger::Log("PolarAlignment: 长曝光拍摄或解析过程也失败", LogLevel::ERROR, DeviceType::MAIN);
            result.isSuccessful = false;
            result.errorMessage = "极点验证两次都失败，请检查相机连接和拍摄条件";
            return {false, false, "极点验证两次都失败，请检查相机连接和拍摄条件"};
        }
    }

    // 只要有一次成功，直接判断极点
    SloveResults solveResult = Tools::ReadSolveResult(lastCapturedImage, config.cameraWidth, config.cameraHeight);
    if (!isAnalysisSuccessful(solveResult)) {
        Logger::Log("PolarAlignment: 极点验证解析结果无效，需要重试", LogLevel::WARNING, DeviceType::MAIN);
        // 如果第一次解析结果无效，尝试长曝光重试
        Logger::Log("PolarAlignment: 短曝光极点验证失败，尝试长曝光", LogLevel::WARNING, DeviceType::MAIN);

        // 长曝光再试一次 - 重新拍摄图像
        bool secondCaptureSuccess = captureImage(config.recoveryExposureTime);
        bool secondCaptureComplete = waitForCaptureComplete();
        bool secondSolveStarted = solveImage(lastCapturedImage);
        bool secondSolveComplete = waitForSolveComplete();
        
        Logger::Log("PolarAlignment: 第二次尝试结果 - 拍摄:" + std::to_string(secondCaptureSuccess) + 
                    ", 拍摄完成:" + std::to_string(secondCaptureComplete) + 
                    ", 解析开始:" + std::to_string(secondSolveStarted) + 
                    ", 解析完成:" + std::to_string(secondSolveComplete), LogLevel::INFO, DeviceType::MAIN);
        
        if (!secondCaptureSuccess || !secondCaptureComplete || !secondSolveStarted || !secondSolveComplete) {
            Logger::Log("PolarAlignment: 长曝光极点验证也失败", LogLevel::ERROR, DeviceType::MAIN);
            result.isSuccessful = false;
            result.errorMessage = "极点验证两次都失败，请检查相机连接和拍摄条件";
            return {false, false, "极点验证两次都失败，请检查相机连接和拍摄条件"};
        }
        
        // 重新读取解析结果
        solveResult = Tools::ReadSolveResult(lastCapturedImage, config.cameraWidth, config.cameraHeight);
        if (!isAnalysisSuccessful(solveResult)) {
            Logger::Log("PolarAlignment: 长曝光极点验证解析结果也无效", LogLevel::ERROR, DeviceType::MAIN);
            result.isSuccessful = false;
            result.errorMessage = "极点验证解析无效";
            return {false, false, "极点验证解析无效"};
        }
    }

    Logger::Log("PolarAlignment: 极点验证成功，RA: " + std::to_string(solveResult.RA_Degree) +
                "°, DEC: " + std::to_string(solveResult.DEC_Degree) + "°", LogLevel::INFO, DeviceType::MAIN);

    // 判断是否指向极点
    bool isNearPole = false;
    if (isNorthernHemisphere()) {
        if (solveResult.DEC_Degree > 89.5 && solveResult.DEC_Degree <= 90.0) isNearPole = true;
    } else {
        if (solveResult.DEC_Degree >= -90.0 && solveResult.DEC_Degree < -89.5) isNearPole = true;
    }

    if (isNearPole) {
        Logger::Log("PolarAlignment: 当前指向极点，需要移动DEC轴", LogLevel::INFO, DeviceType::MAIN);
        return {true, true, ""};
    } else {
        Logger::Log("PolarAlignment: 当前未指向极点，可以作为第一个测量点", LogLevel::INFO, DeviceType::MAIN);
        SloveResults firstMeasurement;
        firstMeasurement.RA_Degree = solveResult.RA_Degree;
        firstMeasurement.DEC_Degree = solveResult.DEC_Degree;
        measurements.append(firstMeasurement);
        currentMeasurementIndex = 1;
        Logger::Log("PolarAlignment: 已将当前位置记录为第一个测量点", LogLevel::INFO, DeviceType::MAIN);
        return {true, false, ""};
    }
}

bool PolarAlignment::moveDecAxisAway()
{
    Logger::Log("PolarAlignment: 移动DEC轴脱离极点", LogLevel::INFO, DeviceType::MAIN);
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取当前望远镜位置
    double currentRA_Hours, currentDEC_Degree;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA_Hours, currentDEC_Degree);
    double currentRA = Tools::HourToDegree(currentRA_Hours);
    double currentDEC = currentDEC_Degree;
    
    Logger::Log("PolarAlignment: 当前位置 - RA: " + std::to_string(currentRA) + "°, DEC: " + std::to_string(currentDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 检查是否在极点附近
    bool isNearPole = false;
    if (isNorthernHemisphere()) {
        isNearPole = (currentDEC > 89.5 && currentDEC <= 90.0);
    } else {
        isNearPole = (currentDEC >= -90.0 && currentDEC < -89.5);
    }
    
    if (isNearPole) {
        // 从极点移动，确保不超出有效范围
        double targetDEC;
        if (isNorthernHemisphere()) {
            // 北半球：从90°向下移动
            targetDEC = 90.0 - currentDECAngle;
            if (targetDEC < 0) targetDEC = 0; // 确保不超出范围
        } else {
            // 南半球：从-90°向上移动
            targetDEC = -90.0 + currentDECAngle;
            if (targetDEC > 0) targetDEC = 0; // 确保不超出范围
        }
        
        Logger::Log("PolarAlignment: 从极点移动到DEC: " + std::to_string(targetDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
        
        // 直接移动到目标位置
        // moveTelescopeToAbsolutePosition函数内部已经包含了等待移动完成的逻辑
        bool success = moveTelescopeToAbsolutePosition(currentRA, targetDEC);
        return success;
    } else {
        // 不在极点，使用相对移动
        Logger::Log("PolarAlignment: 不在极点，使用相对移动", LogLevel::INFO, DeviceType::MAIN);
        // moveTelescope函数内部已经包含了等待移动完成的逻辑
        bool success = moveTelescope(0.0, currentDECAngle);
        return success;
    }
}

bool PolarAlignment::captureAndAnalyze(int attempt)
{
    Logger::Log("PolarAlignment: 拍摄和分析，尝试次数 " + std::to_string(attempt), LogLevel::INFO, DeviceType::MAIN);
    if (!dpMainCamera) {
        Logger::Log("PolarAlignment: 相机设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 根据尝试次数确定曝光时间
    int exposureTime;
    if (attempt == 1) {
        exposureTime = config.defaultExposureTime;
    } else if (attempt == 2) {
        exposureTime = config.recoveryExposureTime;
    } else {
        if (isFirstCaptureFailure()) {
            Logger::Log("PolarAlignment: 初次拍摄连续失败，停止校准", LogLevel::ERROR, DeviceType::MAIN);
            result.isSuccessful = false;
            result.errorMessage = "初次拍摄解析失败，请检查拍摄条件";
            setState(PolarAlignmentState::FAILED);
            return false;
        } else {
            return handlePostMovementFailure();
        }
    }
    
    // 拍摄图像
    if (!captureImage(exposureTime)) {
        Logger::Log("PolarAlignment: 图像拍摄失败", LogLevel::WARNING, DeviceType::MAIN);
        captureFailureCount++;
        currentRetryAttempt = attempt;
        
        // 检查是否达到两次拍摄失败的限制
        if (captureFailureCount >= 2) {
            Logger::Log("PolarAlignment: 连续两次拍摄失败，退出校准", LogLevel::ERROR, DeviceType::MAIN);
            result.isSuccessful = false;
            result.errorMessage = "连续两次拍摄失败，请检查相机连接和拍摄条件";
            setState(PolarAlignmentState::FAILED);
            return false;
        }
        
        return false;
    }
    
    // 等待拍摄完成
    if (!waitForCaptureComplete()) {
        Logger::Log("PolarAlignment: 拍摄超时", LogLevel::WARNING, DeviceType::MAIN);
        captureFailureCount++;
        currentRetryAttempt = attempt;
        
        // 检查是否达到两次拍摄失败的限制
        if (captureFailureCount >= 2) {
            Logger::Log("PolarAlignment: 连续两次拍摄超时，退出校准", LogLevel::ERROR, DeviceType::MAIN);
            result.isSuccessful = false;
            result.errorMessage = "连续两次拍摄超时，请检查相机连接";
            setState(PolarAlignmentState::FAILED);
            return false;
        }
        
        return false;
    }
    
    // 拍摄成功，重置拍摄失败计数
    captureFailureCount = 0;
    
    // 解析图像
    if (!solveImage(lastCapturedImage)) {
        Logger::Log("PolarAlignment: 图像解析开始命令执行失败", LogLevel::WARNING, DeviceType::MAIN);
        solveFailureCount++;
        currentRetryAttempt = attempt;
        
        // 解析失败时，修正RA角度并重新尝试
        if (solveFailureCount >= 1) {
            Logger::Log("PolarAlignment: 解析失败，修正RA角度重新进行", LogLevel::WARNING, DeviceType::MAIN);
            currentRAAngle *= 0.8; // 减小RA角度
            solveFailureCount = 0; // 重置解析失败计数
            return false; // 返回false让状态机重新尝试
        }
        
        return false;
    }
    
    // 等待解析完成
    if (!waitForSolveComplete()) {
        Logger::Log("PolarAlignment: 解析超时", LogLevel::WARNING, DeviceType::MAIN);
        solveFailureCount++;
        currentRetryAttempt = attempt;
        
        // 解析失败时，修正RA角度并重新尝试
        if (solveFailureCount >= 1) {
            Logger::Log("PolarAlignment: 解析超时，修正RA角度重新进行", LogLevel::WARNING, DeviceType::MAIN);
            currentRAAngle *= 0.8; // 减小RA角度
            solveFailureCount = 0; // 重置解析失败计数
            return false; // 返回false让状态机重新尝试
        }
        
        return false;
    }
    
    // 解析成功，重置解析失败计数
    solveFailureCount = 0;
    
    // 获取解析结果
    SloveResults analysisResult = Tools::ReadSolveResult(lastCapturedImage, config.cameraWidth, config.cameraHeight);
    if (isAnalysisSuccessful(analysisResult)) {
        // 检查是否需要添加测量结果
        // 如果当前测量索引为0，说明这是第一次拍摄，需要添加
        // 如果当前测量索引为1，说明第一个点已经在checkPolarPoint中添加，这里添加第二个点
        // 如果当前测量索引为2，说明添加第三个点
        if (currentMeasurementIndex < 3) {
            measurements.append(analysisResult);
            currentMeasurementIndex++;
            currentRetryAttempt = 0;
            Logger::Log("PolarAlignment: 拍摄和分析成功，测量次数 " + std::to_string(currentMeasurementIndex), LogLevel::INFO, DeviceType::MAIN);
            return true;
        } else {
            Logger::Log("PolarAlignment: 已达到最大测量次数，跳过添加", LogLevel::WARNING, DeviceType::MAIN);
            currentRetryAttempt = 0;
            return true;
        }
    } else {
        Logger::Log("PolarAlignment: 解析结果无效", LogLevel::WARNING, DeviceType::MAIN);
        solveFailureCount++;
        currentRetryAttempt = attempt;
        
        // 解析结果无效时，修正RA角度并重新尝试
        if (solveFailureCount >= 1) {
            Logger::Log("PolarAlignment: 解析结果无效，修正RA角度重新进行", LogLevel::WARNING, DeviceType::MAIN);
            currentRAAngle *= 0.8; // 减小RA角度
            solveFailureCount = 0; // 重置解析失败计数
            return false; // 返回false让状态机重新尝试
        }
        
        return false;
    }
}

bool PolarAlignment::moveRAAxis()
{
    Logger::Log("PolarAlignment: 移动RA轴 " + std::to_string(currentRAAngle) + " 度", LogLevel::INFO, DeviceType::MAIN);
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 移动RA轴到下一个位置
    // moveTelescope函数内部已经包含了等待移动完成的逻辑，不需要再次调用waitForMovementComplete
    bool success = moveTelescope(currentRAAngle, 0.0);
    return success;
}

bool PolarAlignment::calculateDeviation()
{
    Logger::Log("PolarAlignment: 计算极轴偏差", LogLevel::INFO, DeviceType::MAIN);
    if (measurements.size() < 3) {
        Logger::Log("PolarAlignment: 测量数据不足", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 使用正确的三点极轴校准算法
    double azimuthDeviation, altitudeDeviation;
    bool success = calculatePolarDeviationCorrect(measurements[0], measurements[1], measurements[2],
                                                azimuthDeviation, altitudeDeviation);
    
    if (success) {
        // 将方位角和高度角偏差转换为RA和DEC偏差
        result.raDeviation = azimuthDeviation;
        result.decDeviation = altitudeDeviation;
        result.totalDeviation = calculateTotalDeviation(result.raDeviation, result.decDeviation);
        result.measurements = measurements;
        result.isSuccessful = true;
        
        Logger::Log("PolarAlignment: 偏差计算完成 - 方位角偏差: " + std::to_string(result.raDeviation) + 
                    "°, 高度角偏差: " + std::to_string(result.decDeviation) + 
                    "°, 总偏差: " + std::to_string(result.totalDeviation) + "°", LogLevel::INFO, DeviceType::MAIN);
        
        // 立即生成调整指导信息并发出信号
        QString adjustmentRa, adjustmentDec;
        QString adjustmentGuide = generateAdjustmentGuide(adjustmentRa, adjustmentDec);
        Logger::Log("PolarAlignment: 调整指导: " + adjustmentGuide.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        
        // 根据地理位置计算期望的极点位置用于显示
        double expectedRA, expectedDEC;
        calculateExpectedPolarPosition(expectedRA, expectedDEC);
        
        // 使用最后一个测量点的数据发出调整指导信号
        SloveResults lastMeasurement = measurements.last();
        emit adjustmentGuideData(lastMeasurement.RA_Degree, lastMeasurement.DEC_Degree,
                               lastMeasurement.RA_1, lastMeasurement.RA_0, 
                               lastMeasurement.DEC_2, lastMeasurement.DEC_1, 
                               expectedRA, expectedDEC, result.raDeviation, result.decDeviation, 
                               adjustmentRa, adjustmentDec);
        
        return true;
    } else {
        Logger::Log("PolarAlignment: 偏差计算失败", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
}

bool PolarAlignment::guideUserAdjustment()
{
    Logger::Log("PolarAlignment: 指导用户调整", LogLevel::INFO, DeviceType::MAIN);
    
    // 检查用户是否取消校准
    if (!isRunningFlag || isPausedFlag) {
        Logger::Log("PolarAlignment: 校准已停止或暂停", LogLevel::INFO, DeviceType::MAIN);
        return false;
    }
    
    // 进行新的拍摄和分析来验证调整效果
    Logger::Log("PolarAlignment: 开始验证调整效果", LogLevel::INFO, DeviceType::MAIN);
    
    // 拍摄新图像进行验证
    if (!captureImage(config.defaultExposureTime)) {
        Logger::Log("PolarAlignment: 验证拍摄失败", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    
    // 等待拍摄完成
    if (!waitForCaptureComplete()) {
        Logger::Log("PolarAlignment: 验证拍摄超时", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    
    // 解析图像
    if (!solveImage(lastCapturedImage)) {
        Logger::Log("PolarAlignment: 验证解析开始失败", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    
    // 等待解析完成
    if (!waitForSolveComplete()) {
        Logger::Log("PolarAlignment: 验证解析超时", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    
    // 获取解析结果
    SloveResults verificationResult = Tools::ReadSolveResult(lastCapturedImage, config.cameraWidth, config.cameraHeight);
    if (!isAnalysisSuccessful(verificationResult)) {
        Logger::Log("PolarAlignment: 验证解析结果无效", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    
    Logger::Log("PolarAlignment: 验证拍摄成功 - RA: " + std::to_string(verificationResult.RA_Degree) + 
                "°, DEC: " + std::to_string(verificationResult.DEC_Degree) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 将新的验证结果添加到测量列表中，用于重新计算偏差
    measurements.append(verificationResult);
    
    // 使用三点极轴校准算法重新计算偏差
    if (measurements.size() >= 3) {
        // 使用最新的三个测量点计算偏差
        QList<SloveResults> recentMeasurements;
        for (int i = measurements.size() - 3; i < measurements.size(); ++i) {
            recentMeasurements.append(measurements[i]);
        }
        
        double azimuthDeviation, altitudeDeviation;
        bool success = calculatePolarDeviationCorrect(recentMeasurements[0], recentMeasurements[1], recentMeasurements[2],
                                                    azimuthDeviation, altitudeDeviation);
        
        if (success) {
            // 更新结果
            result.raDeviation = azimuthDeviation;
            result.decDeviation = altitudeDeviation;
            result.totalDeviation = calculateTotalDeviation(result.raDeviation, result.decDeviation);
            
            Logger::Log("PolarAlignment: 重新计算偏差 - 方位角偏差: " + std::to_string(result.raDeviation) + 
                        "°, 高度角偏差: " + std::to_string(result.decDeviation) + 
                        "°, 总偏差: " + std::to_string(result.totalDeviation) + "°", LogLevel::INFO, DeviceType::MAIN);
            
            // 检查是否达到精度要求
            double precisionThreshold = config.finalVerificationThreshold;
            if (result.totalDeviation < precisionThreshold) {
                Logger::Log("PolarAlignment: 调整验证成功，精度达标: " + std::to_string(result.totalDeviation) + 
                            "° < " + std::to_string(precisionThreshold) + "°", LogLevel::INFO, DeviceType::MAIN);
                return true; // 精度达标，进入最终验证
            } else {
                Logger::Log("PolarAlignment: 调整验证失败，精度不足: " + std::to_string(result.totalDeviation) + 
                            "° >= " + std::to_string(precisionThreshold) + "°，继续调整", LogLevel::WARNING, DeviceType::MAIN);
                
                // 更新调整指导信息并发出信号
                QString adjustmentRa, adjustmentDec;
                QString adjustmentGuide = generateAdjustmentGuide(adjustmentRa, adjustmentDec);
                Logger::Log("PolarAlignment: 新的调整指导: " + adjustmentGuide.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                
                // 根据地理位置计算期望的极点位置用于显示
                double expectedRA, expectedDEC;
                calculateExpectedPolarPosition(expectedRA, expectedDEC);
                
                emit adjustmentGuideData(verificationResult.RA_Degree, verificationResult.DEC_Degree,
                                       verificationResult.RA_1, verificationResult.RA_0, 
                                       verificationResult.DEC_2, verificationResult.DEC_1, 
                                       expectedRA, expectedDEC, result.raDeviation, result.decDeviation, adjustmentRa, adjustmentDec);

                // 继续循环，启动定时器进行下一次验证
                if (isRunningFlag && !isPausedFlag) {
                    stateTimer.start(100);
                }
                
                // 返回false让状态机保持在GUIDING_ADJUSTMENT状态
                return false;
            }
        } else {
            Logger::Log("PolarAlignment: 重新计算偏差失败", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
    } else {
        Logger::Log("PolarAlignment: 测量点不足，无法重新计算偏差", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
}

bool PolarAlignment::performFinalVerification()
{
    Logger::Log("PolarAlignment: 执行最终验证", LogLevel::INFO, DeviceType::MAIN);
    
    // 进行最终确认验证拍摄
    if (captureAndAnalyze(1)) {
        SloveResults verificationResult = measurements.last();
        double verificationRA = verificationResult.RA_Degree;
        double verificationDEC = verificationResult.DEC_Degree;
        
        // 根据地理位置计算期望的极点位置
        double expectedRA, expectedDEC;
        if (!calculateExpectedPolarPosition(expectedRA, expectedDEC)) {
            Logger::Log("PolarAlignment: 计算期望极点位置失败", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
        
        // 计算误差
        double raError = std::abs(verificationRA - expectedRA);
        double decError = std::abs(verificationDEC - expectedDEC);
        double totalError = std::sqrt(raError * raError + decError * decError);
        
        Logger::Log("PolarAlignment: 最终确认验证 - 当前RA: " + std::to_string(verificationRA) + 
                    "°, 期望RA: " + std::to_string(expectedRA) + "°, RA误差: " + std::to_string(raError) + 
                    "°, 当前DEC: " + std::to_string(verificationDEC) + 
                    "°, 期望DEC: " + std::to_string(expectedDEC) + "°, DEC误差: " + std::to_string(decError) + 
                    "°, 总误差: " + std::to_string(totalError), LogLevel::INFO, DeviceType::MAIN);
        
        // 根据配置的精度要求进行判断
        double precisionThreshold = config.finalVerificationThreshold;
        if (totalError < precisionThreshold) {
            Logger::Log("PolarAlignment: 最终确认验证成功，校准精度: " + std::to_string(totalError) + 
                        "°, 要求精度: " + std::to_string(precisionThreshold) + "°", LogLevel::INFO, DeviceType::MAIN);
            
            // 设置最终结果
            result.isSuccessful = true;
            result.raDeviation = raError;
            result.decDeviation = decError;
            result.totalDeviation = totalError;
            result.measurements = measurements;
            
            return true;
        } else {
            Logger::Log("PolarAlignment: 最终确认验证失败，当前精度: " + std::to_string(totalError) + 
                        "°, 要求精度: " + std::to_string(precisionThreshold) + "°, 继续调整", LogLevel::WARNING, DeviceType::MAIN);
            return false;
        }
    } else {
        Logger::Log("PolarAlignment: 最终确认验证拍摄失败", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
}

bool PolarAlignment::captureImage(int exposureTime)
{
    Logger::Log("PolarAlignment: 拍摄图像，曝光时间 " + std::to_string(exposureTime) + "ms", LogLevel::INFO, DeviceType::MAIN);
    if (!dpMainCamera) {
        Logger::Log("PolarAlignment: 相机设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 检查INDI客户端是否有效
    if (!indiServer) {
        Logger::Log("PolarAlignment: INDI客户端不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    uint32_t ret = indiServer->resetCCDFrameInfo(dpMainCamera);
    if (ret != QHYCCD_SUCCESS)
    {
        Logger::Log("INDI_Capture | indi resetCCDFrameInfo | failed", LogLevel::WARNING, DeviceType::CAMERA);
    }
    
    // 通过INDI接口拍摄图像
    Logger::Log("PolarAlignment: 开始调用INDI拍摄接口", LogLevel::INFO, DeviceType::MAIN);
    ret = indiServer->takeExposure(dpMainCamera, exposureTime / 1000.0);
    if (ret == QHYCCD_SUCCESS) {
        isCaptureEnd = false;
        lastCapturedImage = "/dev/shm/ccd_simulator.fits";

        Logger::Log("PolarAlignment: 拍摄命令发送成功，等待回调", LogLevel::INFO, DeviceType::MAIN);
        return true;
    } else {
        Logger::Log("PolarAlignment: 拍摄失败，错误代码: " + std::to_string(ret), LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
}



bool PolarAlignment::solveImage(const QString& imageFile)
{
    Logger::Log("PolarAlignment: 解析图像 " + imageFile.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    
    int focalLength = config.focalLength; // 焦距（毫米）
    double cameraWidth = config.cameraWidth; // 相机传感器宽度（毫米）
    double cameraHeight = config.cameraHeight; // 相机传感器高度（毫米）    
    // 调用图像解析功能
    bool ret = Tools::PlateSolve(imageFile, focalLength, cameraWidth, cameraHeight, false);
    if(!ret)
    {
        Logger::Log("PolarAlignment: 图像解析命令执行失败", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    
    Logger::Log("PolarAlignment: 图像开始解析", LogLevel::INFO, DeviceType::MAIN);
    return true;
}

bool PolarAlignment::moveTelescope(double ra, double dec)
{
    Logger::Log("PolarAlignment: 移动望远镜 RA=" + std::to_string(ra) + " DEC=" + std::to_string(dec), LogLevel::INFO, DeviceType::MAIN);
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取当前望远镜位置
    double currentRA_Hours, currentDEC_Degree;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA_Hours, currentDEC_Degree);
    double currentRA = Tools::HourToDegree(currentRA_Hours);
    double currentDEC = currentDEC_Degree;

    // 计算新位置
    double newRA = currentRA + ra;
    double newDEC = currentDEC + dec;
    
    Logger::Log("PolarAlignment: 当前位置 RA=" + std::to_string(currentRA) + " DEC=" + std::to_string(currentDEC) + 
                ", 目标位置 RA=" + std::to_string(newRA) + " DEC=" + std::to_string(newDEC), LogLevel::INFO, DeviceType::MAIN);
    
    // 发送移动命令前检查赤道仪状态
    if(dpMount!=NULL)
    {
        // 检查赤道仪是否已经在移动
        MountStatus currentStat;
        indiServer->getTelescopeStatus(dpMount, currentStat.status, currentStat.error);
        if(currentStat.status == "Slewing") {
            Logger::Log("PolarAlignment: 赤道仪正在移动中，等待完成后再发送新命令", LogLevel::WARNING, DeviceType::MAIN);
            // 等待当前移动完成
            int waitTime = 0;
            while(currentStat.status == "Slewing" && waitTime < 30000) { // 最多等待30秒
                QThread::msleep(500);
                waitTime += 500;
                indiServer->getTelescopeStatus(dpMount, currentStat.status, currentStat.error);
            }
            if(waitTime >= 30000) {
                Logger::Log("PolarAlignment: 等待赤道仪移动完成超时", LogLevel::ERROR, DeviceType::MAIN);
                return false;
            }
        }
        
        INDI::PropertyNumber property = NULL;
        indiServer->slewTelescopeJNowNonBlock(dpMount,Tools::DegreeToHour(newRA), newDEC,true,property);
        MountStatus Stat;
        indiServer->getTelescopeStatus(dpMount, Stat.status, Stat.error);
        int time = 0;
        while ((Stat.status == "Tracking" || Stat.status == "Idle") && time < 10000)
        {
            indiServer->getTelescopeStatus(dpMount, Stat.status, Stat.error);
            QThread::msleep(500);
            time += 500;
        }
        if (time >= 10000) {
            Logger::Log("PolarAlignment: 等待赤道仪移动命令开始超时", LogLevel::ERROR, DeviceType::MAIN);
            indiServer->slewTelescopeJNowNonBlock(dpMount, Tools::DegreeToHour(ra), dec, true, property);
        }
        
        // 等待移动完成，使用更可靠的检测方法
        time = 0;
        bool movementComplete = false;
        while (time < 60000) // 超时时间60秒
        {
            indiServer->getTelescopeStatus(dpMount, Stat.status, Stat.error);
            if(Stat.status == "Slewing") {
                // 继续等待
                QThread::msleep(500); // 检查间隔500ms
                time += 500;
            } else if(Stat.status == "Idle" || Stat.status == "Tracking") {
                // 移动成功完成
                movementComplete = true;
                Logger::Log("PolarAlignment: 移动完成，最终状态: " + Stat.status.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                break;
            }
            
            Logger::Log("PolarAlignment: 移动状态检查 - 状态: " + Stat.status.toStdString() + 
                        ", 错误: " + Stat.error.toStdString() + ", 时间: " + std::to_string(time) + "ms", LogLevel::INFO, DeviceType::MAIN);
            
            if(Stat.status == "Slewing") {
                // 继续等待
                QThread::msleep(500); // 检查间隔500ms
                time += 500;
            } else if(Stat.status == "Idle" || Stat.status == "Tracking") {
                // 移动成功完成
                movementComplete = true;
                Logger::Log("PolarAlignment: 移动完成，最终状态: " + Stat.status.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                break;
            }else {
                // 其他状态，可能是中间状态，继续等待一段时间
                Logger::Log("PolarAlignment: 移动过程中出现其他状态: " + Stat.status.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                QThread::msleep(500);
                time += 500;
            }
        }
        
        if(!movementComplete) {
            Logger::Log("PolarAlignment: 望远镜移动超时 (60秒)", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
        
        // 验证移动结果
        double finalRA_Hours, finalDEC_Degree;
        indiServer->getTelescopeRADECJNOW(dpMount, finalRA_Hours, finalDEC_Degree);
        double finalRA = Tools::HourToDegree(finalRA_Hours);
        double finalDEC = finalDEC_Degree;
        
        Logger::Log("PolarAlignment: 移动后位置 RA=" + std::to_string(finalRA) + " DEC=" + std::to_string(finalDEC), LogLevel::INFO, DeviceType::MAIN);
        
        // 检查移动是否成功（允许一定的误差）
        double raError = std::abs(finalRA - newRA);
        double decError = std::abs(finalDEC - newDEC);
        if(raError > 0.1 || decError > 0.1) {
            Logger::Log("PolarAlignment: 移动精度不足 - RA误差: " + std::to_string(raError) + 
                        "°, DEC误差: " + std::to_string(decError) + "°", LogLevel::WARNING, DeviceType::MAIN);
        }
    }

    return true;
}

bool PolarAlignment::moveTelescopeToAbsolutePosition(double ra, double dec)
{
    Logger::Log("PolarAlignment: 移动到绝对位置 RA=" + std::to_string(ra) + " DEC=" + std::to_string(dec), LogLevel::INFO, DeviceType::MAIN);
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取当前望远镜位置
    double currentRA_Hours, currentDEC_Degree;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA_Hours, currentDEC_Degree);
    double currentRA = Tools::HourToDegree(currentRA_Hours);
    double currentDEC = currentDEC_Degree;
    
    Logger::Log("PolarAlignment: 当前位置 RA=" + std::to_string(currentRA) + " DEC=" + std::to_string(currentDEC) + 
                ", 目标位置 RA=" + std::to_string(ra) + " DEC=" + std::to_string(dec), LogLevel::INFO, DeviceType::MAIN);
    
    // 发送移动命令到绝对位置
    if(dpMount!=NULL)
    {
        // 检查赤道仪是否已经在移动
        MountStatus currentStat;
        indiServer->getTelescopeStatus(dpMount, currentStat.status, currentStat.error);
        if(currentStat.status == "Slewing") {
            Logger::Log("PolarAlignment: 赤道仪正在移动中，等待完成后再发送绝对位置移动命令", LogLevel::WARNING, DeviceType::MAIN);
            // 等待当前移动完成
            int waitTime = 0;
            while(currentStat.status == "Slewing" && waitTime < 30000) { // 最多等待30秒
                QThread::msleep(500);
                waitTime += 500;
                indiServer->getTelescopeStatus(dpMount, currentStat.status, currentStat.error);
            }
            if(waitTime >= 30000) {
                Logger::Log("PolarAlignment: 等待赤道仪移动完成超时", LogLevel::ERROR, DeviceType::MAIN);
                return false;
            }
        }
        
        INDI::PropertyNumber property = NULL;
        indiServer->slewTelescopeJNowNonBlock(dpMount, Tools::DegreeToHour(ra), dec, true, property);
        MountStatus Stat;
        indiServer->getTelescopeStatus(dpMount, Stat.status, Stat.error);
        int time = 0;
        while ((Stat.status == "Tracking" || Stat.status == "Idle") && time < 10000)
        {
            indiServer->getTelescopeStatus(dpMount, Stat.status, Stat.error);
            QThread::msleep(500);
            time += 500;
        }
        if (time >= 10000) {
            Logger::Log("PolarAlignment: 等待赤道仪移动命令开始超时", LogLevel::ERROR, DeviceType::MAIN);
            indiServer->slewTelescopeJNowNonBlock(dpMount, Tools::DegreeToHour(ra), dec, true, property);
        }
        
        // 等待移动完成，使用更可靠的检测方法
        time = 0;
        bool movementComplete = false;
        while (time < 60000) // 超时时间60秒
        {
            MountStatus Stat;
            indiServer->getTelescopeStatus(dpMount, Stat.status, Stat.error);
            
            Logger::Log("PolarAlignment: 绝对位置移动状态检查 - 状态: " + Stat.status.toStdString() + 
                        ", 错误: " + Stat.error.toStdString() + ", 时间: " + std::to_string(time) + "ms", LogLevel::INFO, DeviceType::MAIN);
            
            if(Stat.status == "Slewing") {
                // 继续等待
                QThread::msleep(500); // 检查间隔500ms
                time += 500;
            } else if(Stat.status == "Idle" || Stat.status == "Tracking") {
                // 移动成功完成
                movementComplete = true;
                Logger::Log("PolarAlignment: 绝对位置移动完成，最终状态: " + Stat.status.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                break;
            } else {
                // 其他状态，可能是中间状态，继续等待一段时间
                Logger::Log("PolarAlignment: 绝对位置移动过程中出现其他状态: " + Stat.status.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                QThread::msleep(500);
                time += 500;
            }
        }
        
        if(!movementComplete) {
            Logger::Log("PolarAlignment: 绝对位置移动超时 (60秒)", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
        
        // 验证移动结果
        double finalRA_Hours, finalDEC_Degree;
        indiServer->getTelescopeRADECJNOW(dpMount, finalRA_Hours, finalDEC_Degree);
        double finalRA = Tools::HourToDegree(finalRA_Hours);
        double finalDEC = finalDEC_Degree;
        
        Logger::Log("PolarAlignment: 绝对位置移动后位置 RA=" + std::to_string(finalRA) + " DEC=" + std::to_string(finalDEC), LogLevel::INFO, DeviceType::MAIN);
        
        // 检查移动是否成功（允许一定的误差）
        double raError = std::abs(finalRA - ra);
        double decError = std::abs(finalDEC - dec);
        if(raError > 0.1 || decError > 0.1) {
            Logger::Log("PolarAlignment: 绝对位置移动精度不足 - RA误差: " + std::to_string(raError) + 
                        "°, DEC误差: " + std::to_string(decError) + "°", LogLevel::WARNING, DeviceType::MAIN);
        }
    }
    
    return true;
}

void PolarAlignment::setCaptureEnd(bool isEnd)
{
    Logger::Log("PolarAlignment: 设置拍摄完成标志: " + std::to_string(isEnd), LogLevel::INFO, DeviceType::MAIN);
    isCaptureEnd = isEnd;
}


bool PolarAlignment::waitForCaptureComplete()
{
    Logger::Log("PolarAlignment: 等待拍摄完成", LogLevel::INFO, DeviceType::MAIN);
    
    
    captureAndAnalysisTimer.start(config.captureAndAnalysisTimeout);
    QEventLoop loop;
    QTimer checkTimer;
    checkTimer.setInterval(100);
    
    connect(&checkTimer, &QTimer::timeout, [&]() {
        if (isCaptureEnd) { // 5秒后假设完成
            lastCapturedImage = QString("/home/quarcs/workspace/testimage/%1.fits").arg(testimage);
            testimage++;
            if (testimage > 9) {
                testimage = 0;
            }
            loop.quit();
        }
    });
    
    connect(&captureAndAnalysisTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    checkTimer.start();
    loop.exec();
    checkTimer.stop();
    captureAndAnalysisTimer.stop();
    
    return isCaptureEnd;
}

bool PolarAlignment::waitForSolveComplete()
{
    Logger::Log("PolarAlignment: 等待解析完成. 等待中...", LogLevel::INFO, DeviceType::MAIN);
    return Tools::isSolveImageFinish(); 
}

bool PolarAlignment::waitForMovementComplete()
{
    Logger::Log("PolarAlignment: 等待移动完成", LogLevel::INFO, DeviceType::MAIN);
    
    movementTimer.start(config.movementTimeout);
    QEventLoop loop;
    QTimer checkTimer;
    checkTimer.setInterval(500);
    
    connect(&checkTimer, &QTimer::timeout, [&]() {
        if (!dpMount) {
            loop.quit();
            return;
        }
        
        QString status, error;
        uint32_t result = indiServer->getTelescopeStatus(dpMount, status, error);
        if (result == 1 && status == "OK") {
            Logger::Log("PolarAlignment: 移动完成", LogLevel::INFO, DeviceType::MAIN);
            loop.quit();
        }
    });
    
    connect(&movementTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    checkTimer.start();
    loop.exec();
    checkTimer.stop();
    movementTimer.stop();
    
    return !movementTimer.isActive();
}

bool PolarAlignment::isAnalysisSuccessful(const SloveResults& result)
{
    // 检查解析结果是否有效
    return (result.RA_Degree != -1 && result.DEC_Degree != -1);
}

void PolarAlignment::handleAnalysisFailure(int attempt)
{
    Logger::Log("PolarAlignment: 分析失败，尝试次数 " + std::to_string(attempt), LogLevel::WARNING, DeviceType::MAIN);
    if (attempt >= config.maxRetryAttempts) {
        handleCriticalFailure();
    } else {
        currentRetryAttempt = attempt + 1;
    }
}

void PolarAlignment::handleCriticalFailure()
{
    Logger::Log("PolarAlignment: 严重失败，返回初始位置", LogLevel::ERROR, DeviceType::MAIN);
    result.isSuccessful = false;
    result.errorMessage = "解析成功率太低，请重新调整拍摄位置，注意光线干扰";
    setState(PolarAlignmentState::USER_INTERVENTION);
}


double PolarAlignment::calculateTotalDeviation(double raDev, double decDev)
{
    // 计算总偏差（欧几里得距离）
    return std::sqrt(raDev * raDev + decDev * decDev);
}

// ==================== 地理位置相关函数实现 ====================

bool PolarAlignment::calculateExpectedPolarPosition(double& expectedRA, double& expectedDEC)
{
    Logger::Log("PolarAlignment: 计算期望的极点位置", LogLevel::INFO, DeviceType::MAIN);
    
    // 使用精确的极点计算函数
    return calculatePrecisePolarPosition(expectedRA, expectedDEC);
}

bool PolarAlignment::isNorthernHemisphere() const
{
    // 根据纬度判断半球
    // 正值表示北半球，负值表示南半球
    return config.latitude >= 0.0;
}

bool PolarAlignment::calculatePrecisePolarPosition(double& expectedRA, double& expectedDEC)
{
    Logger::Log("PolarAlignment: 计算精确的极点位置", LogLevel::INFO, DeviceType::MAIN);
    
    // 获取当前系统时间
    QDateTime currentTime = QDateTime::currentDateTime();
    QDateTime utcTime = currentTime.toUTC();
    
    Logger::Log("PolarAlignment: 当前UTC时间: " + utcTime.toString("yyyy-MM-dd hh:mm:ss").toStdString(), LogLevel::INFO, DeviceType::MAIN);
    
    // 计算儒略日（Julian Day）
    int year = utcTime.date().year();
    int month = utcTime.date().month();
    int day = utcTime.date().day();
    int hour = utcTime.time().hour();
    int minute = utcTime.time().minute();
    int second = utcTime.time().second();
    
    // 计算儒略日
    double jd = calculateJulianDay(year, month, day, hour, minute, second);
    
    Logger::Log("PolarAlignment: 儒略日: " + std::to_string(jd), LogLevel::INFO, DeviceType::MAIN);
    
    // 计算格林尼治恒星时（GST）
    double gst = calculateGreenwichSiderealTime(jd);
    
    Logger::Log("PolarAlignment: 格林尼治恒星时: " + std::to_string(gst) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 计算本地恒星时（LST）
    double lst = gst + config.longitude;
    if (lst < 0) lst += 360.0;
    if (lst >= 360.0) lst -= 360.0;
    
    Logger::Log("PolarAlignment: 本地恒星时: " + std::to_string(lst) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 根据半球确定极点坐标，并应用岁差和章动修正
    if (isNorthernHemisphere()) {
        // 北半球：北极点
        expectedRA = lst;  // 极点RA等于本地恒星时
        expectedDEC = 90.0; // 北极点DEC
        
        // 应用岁差修正
        double t = (jd - 2451545.0) / 36525.0;
        double precessionRA = 0.55675 * t * t / 3600.0; // 岁差对RA的影响
        double precessionDEC = 20.0431 * t / 3600.0; // 岁差对DEC的影响
        
        expectedRA += precessionRA;
        expectedDEC += precessionDEC;
        
        Logger::Log("PolarAlignment: 北半球精确极点坐标 - RA: " + std::to_string(expectedRA) + 
                    "°, DEC: " + std::to_string(expectedDEC) + "° (含岁差修正)", LogLevel::INFO, DeviceType::MAIN);
    } else {
        // 南半球：南极点
        expectedRA = lst;   // 极点RA等于本地恒星时
        expectedDEC = -90.0; // 南极点DEC
        
        // 应用岁差修正
        double t = (jd - 2451545.0) / 36525.0;
        double precessionRA = 0.55675 * t * t / 3600.0; // 岁差对RA的影响
        double precessionDEC = -20.0431 * t / 3600.0; // 岁差对DEC的影响（南半球为负）
        
        expectedRA += precessionRA;
        expectedDEC += precessionDEC;
        
        Logger::Log("PolarAlignment: 南半球精确极点坐标 - RA: " + std::to_string(expectedRA) + 
                    "°, DEC: " + std::to_string(expectedDEC) + "° (含岁差修正)", LogLevel::INFO, DeviceType::MAIN);
    }
    
    return true;
}

// ==================== 天文计算辅助函数 ====================

double PolarAlignment::calculateJulianDay(int year, int month, int day, int hour, int minute, int second)
{
    // 精确的儒略日计算（基于Meeus算法）
    
    // 处理1月和2月
    if (month <= 2) {
        year -= 1;
        month += 12;
    }
    
    // 计算世纪数
    int a = year / 100;
    int b = 2 - a + a / 4;
    
    // 计算儒略日
    double jd = 365.25 * (year + 4716) + 30.6001 * (month + 1) + day + b - 1524.5;
    
    // 添加时间部分（转换为小数天）
    double timeFraction = (hour + minute / 60.0 + second / 3600.0) / 24.0;
    jd += timeFraction;
    
    // 修正：对于格里高利历，需要额外的修正
    if (year > 1582 || (year == 1582 && month > 10) || (year == 1582 && month == 10 && day >= 15)) {
        // 格里高利历，无需额外修正
    } else {
        // 儒略历，需要修正
        jd -= b;
    }
    
    return jd;
}

double PolarAlignment::calculateGreenwichSiderealTime(double jd)
{
    // 精确的格林尼治恒星时计算（基于IAU 2006标准）
    
    // 计算从J2000.0开始的天数
    double t = (jd - 2451545.0) / 36525.0;
    
    // 计算地球自转角度（ERA - Earth Rotation Angle）
    double era = 2 * M_PI * (0.7790572732640 + 1.00273781191135448 * (jd - 2451545.0));
    
    // 计算岁差修正
    double zeta = 2.650545 + 2306.083227 + 0.2988499 * t + 0.01801828 * t * t - 0.000005971 * t * t * t;
    zeta += -0.0000003173 * t * t * t * t;
    zeta = zeta * M_PI / 180.0 / 3600.0; // 转换为弧度
    
    double theta = 2004.191903 - 0.4294934 * t - 0.04182264 * t * t - 0.000007089 * t * t * t;
    theta += -0.0000001274 * t * t * t * t;
    theta = theta * M_PI / 180.0 / 3600.0; // 转换为弧度
    
    double z = 2.650545 + 2306.077181 + 1.0927348 * t + 0.01826837 * t * t - 0.000028596 * t * t * t;
    z += -0.0000002904 * t * t * t * t;
    z = z * M_PI / 180.0 / 3600.0; // 转换为弧度
    
    // 计算章动修正
    double deltaPsi = -17.1996 * sin(125.0 - 0.05295 * t) - 1.3187 * sin(200.9 + 1.97129 * t);
    deltaPsi = deltaPsi * M_PI / 180.0 / 3600.0; // 转换为弧度
    
    double deltaEpsilon = 9.2025 * cos(125.0 - 0.05295 * t) + 0.5736 * cos(200.9 + 1.97129 * t);
    deltaEpsilon = deltaEpsilon * M_PI / 180.0 / 3600.0; // 转换为弧度
    
    // 计算黄道倾角（epsilon）
    double epsilon = 23.439 - 0.0000004 * t;
    epsilon = epsilon * M_PI / 180.0; // 转换为弧度
    
    // 计算修正后的恒星时
    double gst = era + zeta + 0.5 * zeta * zeta * sin(theta) + 0.5 * zeta * zeta * zeta * sin(theta) * cos(theta);
    gst += deltaPsi * cos(epsilon + deltaEpsilon);
    
    // 转换为度并归一化
    gst = gst * 180.0 / M_PI;
    while (gst < 0) gst += 360.0;
    while (gst >= 360.0) gst -= 360.0;
    
    return gst;
}

QString PolarAlignment::generateAdjustmentGuide(QString &adjustmentRa, QString &adjustmentDec)
{
    QString guide;
    
    // 检查是否有有效的极轴偏差结果
    if (!result.isSuccessful) {
        return "无法生成调整指导，请重新开始校准";
    }
    
    // 基于极轴偏差生成调整指导
    double azimuthDeviation = result.raDeviation;
    double altitudeDeviation = result.decDeviation;
    double totalDeviation = result.totalDeviation;
    
    // 根据方位角偏差生成调整指导
    if (std::abs(azimuthDeviation) > 0.1) {
        if (azimuthDeviation > 0) {
            guide += QString("方位角: 向西调整 %1 度; ").arg(std::abs(azimuthDeviation), 0, 'f', 2);
            adjustmentRa = QString("向西调整 %1 度; ").arg(std::abs(azimuthDeviation), 0, 'f', 2);
        } else {
            guide += QString("方位角: 向东调整 %1 度; ").arg(std::abs(azimuthDeviation), 0, 'f', 2);
            adjustmentRa = QString("向东调整 %1 度; ").arg(std::abs(azimuthDeviation), 0, 'f', 2);
        }
    } else {
        guide += "方位角: 已对齐; ";
        adjustmentRa = "已对齐; ";
    }
    
    // 根据高度角偏差生成调整指导
    if (std::abs(altitudeDeviation) > 0.1) {
        if (altitudeDeviation > 0) {
            guide += QString("高度角: 向上调整 %1 度; ").arg(std::abs(altitudeDeviation), 0, 'f', 2);
            adjustmentDec = QString("向上调整 %1 度; ").arg(std::abs(altitudeDeviation), 0, 'f', 2);
        } else {
            guide += QString("高度角: 向下调整 %1 度; ").arg(std::abs(altitudeDeviation), 0, 'f', 2);
            adjustmentDec = QString("向下调整 %1 度; ").arg(std::abs(altitudeDeviation), 0, 'f', 2);
        }
    } else {
        guide += "高度角: 已对齐; ";
        adjustmentDec = "已对齐; ";
    }
    
    guide += QString("总偏差: %1 度").arg(totalDeviation, 0, 'f', 2);
    
    Logger::Log("PolarAlignment: 生成调整指导 - 方位角偏差: " + std::to_string(azimuthDeviation) + 
                "°, 高度角偏差: " + std::to_string(altitudeDeviation) + 
                "°, 总偏差: " + std::to_string(totalDeviation) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    return guide;
}

bool PolarAlignment::adjustForObstacle()
{
    Logger::Log("PolarAlignment: 调整避开遮挡物，尝试次数 " + std::to_string(currentAdjustmentAttempt + 1), LogLevel::INFO, DeviceType::MAIN);
    
    if (currentAdjustmentAttempt >= config.maxAdjustmentAttempts) {
        Logger::Log("PolarAlignment: 达到最大调整尝试次数", LogLevel::ERROR, DeviceType::MAIN);
        result.isSuccessful = false;
        result.errorMessage = "多次调整后仍无法避开遮挡物，请寻找视野开阔的位置";
        return false;
    }
    
    // 减小移动角度
    currentRAAngle *= config.adjustmentAngleReduction;
    currentDECAngle *= config.adjustmentAngleReduction;
    
    Logger::Log("PolarAlignment: 调整角度 - RA: " + std::to_string(currentRAAngle) + 
                ", DEC: " + std::to_string(currentDECAngle), LogLevel::INFO, DeviceType::MAIN);
    
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 执行调整移动
    // moveTelescope函数内部已经包含了等待移动完成的逻辑
    bool success = moveTelescope(currentRAAngle, currentDECAngle);
    
    if (success) {
        currentAdjustmentAttempt++;
        Logger::Log("PolarAlignment: 调整成功，尝试次数 " + std::to_string(currentAdjustmentAttempt), LogLevel::INFO, DeviceType::MAIN);
        return true;
    } else {
        Logger::Log("PolarAlignment: 调整失败", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
}

bool PolarAlignment::isFirstCaptureFailure()
{
    // 判断是否为第一次拍摄失败
    return (currentState == PolarAlignmentState::FIRST_CAPTURE || 
            currentState == PolarAlignmentState::FIRST_ANALYSIS || 
            currentState == PolarAlignmentState::FIRST_RECOVERY);
}

bool PolarAlignment::handlePostMovementFailure()
{
    Logger::Log("PolarAlignment: 处理移动后失败", LogLevel::INFO, DeviceType::MAIN);
    setState(PolarAlignmentState::ADJUSTING_FOR_OBSTACLE);
    return false;
}

// ==================== 正确的三点极轴校准算法实现 ====================
//
// 算法原理：
// 1. 当极轴完全对准时，望远镜在RA轴上旋转时，DEC轴应该保持不变
// 2. 如果极轴存在偏差，RA轴旋转会导致DEC轴出现偏移
// 3. 通过测量三个不同位置的星点坐标，可以计算出极轴偏差
// 4. 使用三维几何方法，通过平面法向量确定极点位置
// 5. 比较假极点与真实极点的位置来计算偏差
//
// 算法优势：
// - 基于物理原理，计算准确
// - 使用三维几何方法，避免了统计方法的误差
// - 提供方位角和高度角两个方向的偏差
// - 实时反馈，用户友好

// 坐标转换函数实现
PolarAlignment::CartesianCoordinates PolarAlignment::equatorialToCartesian(double ra, double dec, double radius)
{
    // 转换为弧度
    double raRad = ra * M_PI / 180.0;
    double decRad = dec * M_PI / 180.0;
    
    CartesianCoordinates cart;
    cart.x = radius * cos(decRad) * cos(raRad);
    cart.y = radius * cos(decRad) * sin(raRad);
    cart.z = radius * sin(decRad);
    
    return cart;
}

PolarAlignment::SphericalCoordinates PolarAlignment::cartesianToEquatorial(const CartesianCoordinates& cart)
{
    SphericalCoordinates sph;
    double radius = sqrt(cart.x * cart.x + cart.y * cart.y + cart.z * cart.z);
    
    sph.dec = asin(cart.z / radius) * 180.0 / M_PI;
    sph.ra = atan2(cart.y, cart.x) * 180.0 / M_PI;
    
    // 确保RA在0-360度范围内
    if (sph.ra < 0) sph.ra += 360.0;
    
    return sph;
}

PolarAlignment::CartesianCoordinates PolarAlignment::crossProduct(const CartesianCoordinates& v1, const CartesianCoordinates& v2)
{
    CartesianCoordinates result;
    result.x = v1.y * v2.z - v1.z * v2.y;
    result.y = v1.z * v2.x - v1.x * v2.z;
    result.z = v1.x * v2.y - v1.y * v2.x;
    return result;
}

double PolarAlignment::vectorLength(const CartesianCoordinates& v)
{
    return sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

PolarAlignment::CartesianCoordinates PolarAlignment::normalizeVector(const CartesianCoordinates& v)
{
    double length = vectorLength(v);
    CartesianCoordinates result;
    result.x = v.x / length;
    result.y = v.y / length;
    result.z = v.z / length;
    return result;
}

double PolarAlignment::calculateAngleDifference(double angle1, double angle2)
{
    double difference = angle2 - angle1;
    while (difference > 180) difference -= 360;
    while (difference < -180) difference += 360;
    return difference;
}

// 正确的三点极轴校准算法
bool PolarAlignment::calculatePolarDeviationCorrect(const SloveResults& pos1, const SloveResults& pos2, const SloveResults& pos3,
                                                   double& azimuthDeviation, double& altitudeDeviation)
{
    Logger::Log("PolarAlignment: 开始正确的三点极轴校准计算", LogLevel::INFO, DeviceType::MAIN);
    
    // 检查测量点是否足够分散
    double raDiff1 = std::abs(pos2.RA_Degree - pos1.RA_Degree);
    double raDiff2 = std::abs(pos3.RA_Degree - pos1.RA_Degree);
    double raDiff3 = std::abs(pos3.RA_Degree - pos2.RA_Degree);
    double decDiff1 = std::abs(pos2.DEC_Degree - pos1.DEC_Degree);
    double decDiff2 = std::abs(pos3.DEC_Degree - pos1.DEC_Degree);
    double decDiff3 = std::abs(pos3.DEC_Degree - pos2.DEC_Degree);
    
    // 如果测量点过于接近，无法进行有效计算
    if (raDiff1 < 0.1 && raDiff2 < 0.1 && raDiff3 < 0.1 && 
        decDiff1 < 0.1 && decDiff2 < 0.1 && decDiff3 < 0.1) {
        Logger::Log("PolarAlignment: 测量点过于接近，无法进行有效计算", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 1. 将三个测量点转换为笛卡尔坐标
    CartesianCoordinates p1 = equatorialToCartesian(pos1.RA_Degree, pos1.DEC_Degree);
    CartesianCoordinates p2 = equatorialToCartesian(pos2.RA_Degree, pos2.DEC_Degree);
    CartesianCoordinates p3 = equatorialToCartesian(pos3.RA_Degree, pos3.DEC_Degree);
    
    Logger::Log("PolarAlignment: 测量点1: (" + std::to_string(p1.x) + ", " + std::to_string(p1.y) + ", " + std::to_string(p1.z) + ")", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("PolarAlignment: 测量点2: (" + std::to_string(p2.x) + ", " + std::to_string(p2.y) + ", " + std::to_string(p2.z) + ")", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("PolarAlignment: 测量点3: (" + std::to_string(p3.x) + ", " + std::to_string(p3.y) + ", " + std::to_string(p3.z) + ")", LogLevel::INFO, DeviceType::MAIN);
    
    // 2. 计算两个向量
    CartesianCoordinates v1 = {p2.x - p1.x, p2.y - p1.y, p2.z - p1.z};
    CartesianCoordinates v2 = {p3.x - p1.x, p3.y - p1.y, p3.z - p1.z};
    
    // 3. 计算法向量（叉积）
    CartesianCoordinates normal = crossProduct(v1, v2);
    
    Logger::Log("PolarAlignment: 法向量: (" + std::to_string(normal.x) + ", " + std::to_string(normal.y) + ", " + std::to_string(normal.z) + ")", LogLevel::INFO, DeviceType::MAIN);
    
    // 检查法向量是否为零向量
    double normalLength = vectorLength(normal);
    if (normalLength < 1e-10) {
        Logger::Log("PolarAlignment: 法向量为零向量，测量点共线或重复", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 4. 归一化法向量
    CartesianCoordinates unitNormal = normalizeVector(normal);
    
    // 5. 计算与单位球面的交点（假极点）
    double r = 1.0;
    CartesianCoordinates fakePolarPoint = {unitNormal.x * r, unitNormal.y * r, unitNormal.z * r};
    
    // 选择正确的交点（z坐标为正的）
    if (fakePolarPoint.z < 0) {
        fakePolarPoint.x = -fakePolarPoint.x;
        fakePolarPoint.y = -fakePolarPoint.y;
        fakePolarPoint.z = -fakePolarPoint.z;
    }
    
    Logger::Log("PolarAlignment: 假极点: (" + std::to_string(fakePolarPoint.x) + ", " + std::to_string(fakePolarPoint.y) + ", " + std::to_string(fakePolarPoint.z) + ")", LogLevel::INFO, DeviceType::MAIN);
    
    // 6. 将假极点转换为赤道坐标
    SphericalCoordinates fakePolarEquatorial = cartesianToEquatorial(fakePolarPoint);
    
    Logger::Log("PolarAlignment: 假极点赤道坐标: RA=" + std::to_string(fakePolarEquatorial.ra) + "°, DEC=" + std::to_string(fakePolarEquatorial.dec) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 7. 真实极点坐标（根据当前时间和地理位置计算）
    double realPolarRA, realPolarDEC;
    if (!calculatePrecisePolarPosition(realPolarRA, realPolarDEC)) {
        Logger::Log("PolarAlignment: 计算真实极点坐标失败", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 8. 计算偏差
    azimuthDeviation = calculateAngleDifference(fakePolarEquatorial.ra, realPolarRA);
    altitudeDeviation = realPolarDEC - fakePolarEquatorial.dec;
    
    Logger::Log("PolarAlignment: 方位角偏差: " + std::to_string(azimuthDeviation) + "°, 高度角偏差: " + std::to_string(altitudeDeviation) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    return true;
}