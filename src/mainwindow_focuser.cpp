#include "mainwindow_command_support.h"

void MainWindow::getFocuserAbsoluteRange(int &absoluteMin, int &absoluteMax) const
{
    absoluteMin = -100000;
    absoluteMax = 100000;
    if (dpFocuser != nullptr)
    {
        int min = -1, max = -1, step = 0, value = 0;
        indi_Client->getFocuserRange(dpFocuser, min, max, step, value);
        if (min != -1 && max != -1 && min < max)
        {
            absoluteMin = min;
            absoluteMax = max;
        }
    }
}

bool MainWindow::maybeExpandFocuserLimitForCalibration(bool isInward, int currentPosition)
{
    if (!focuserManualCalibrationMode)
    {
        return false;
    }

    int absoluteMin = -100000;
    int absoluteMax = 100000;
    getFocuserAbsoluteRange(absoluteMin, absoluteMax);

    const bool reachedPhysicalLimit = isInward ? (currentPosition <= absoluteMin) : (currentPosition >= absoluteMax);
    if (!reachedPhysicalLimit)
    {
        emit wsThread->sendMessageToClient("FocusMoveToLimit:Not at focuser physical limit yet. Calibration move stopped.");
        return false;
    }

    const int desiredDir = isInward ? 1 : -1;
    if (focuserCalibrationExpandedDir != 0 && focuserCalibrationExpandedDir != desiredDir)
    {
        emit wsThread->sendMessageToClient(
            "FocusMoveToLimit:Reached physical limit again immediately after reverse direction during calibration. Movement stopped, please check mechanics.");
        return false;
    }

    focuserCalibrationExpandedDir = desiredDir;
    emit wsThread->sendMessageToClient(
        QString("FocusMoveToLimit:Reached focuser physical %1 limit (%2). Movement stopped.")
            .arg(isInward ? "inner" : "outer")
            .arg(isInward ? absoluteMin : absoluteMax));
    return false;
}

void MainWindow::HandleFocuserMovementDataPeriodically()
{
    Logger::Log("HandleFocuserMovementDataPeriodically | Entry, isFocusMoveDone: " + std::to_string(isFocusMoveDone), LogLevel::DEBUG, DeviceType::FOCUSER);

    if (!isFocusMoveDone)
    {
        focusMoveTimer->stop();
        return;
    }

    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);

    Logger::Log("HandleFocuserMovementDataPeriodically | dpFocuser: " + std::string(dpFocuser ? "Valid" : "NULL") +
                    ", focuserSdkReady: " + std::to_string(focuserSdkReady),
                LogLevel::DEBUG, DeviceType::FOCUSER);

    if (dpFocuser == NULL && !focuserSdkReady)
    {
        focusMoveTimer->stop();
        return;
    }

    if (dpFocuser == NULL && focuserSdkReady)
    {
        Logger::Log("HandleFocuserMovementDataPeriodically | Entering SDK Mode", LogLevel::DEBUG, DeviceType::FOCUSER);
        if (!sdkFocuserExec || !sdkFocuserExec->isRunning())
        {
            focusMoveTimer->stop();
            return;
        }

        if (sdkFocuserPeriodicTaskInFlight.exchange(true))
            return;

        const SdkDeviceHandle handleSnap = sdkFocuserHandle;
        const bool isInwardSnap = this->currentDirection;
        const int targetSnap = this->TargetPosition;
        int minSnap = this->focuserMinPosition;
        int maxSnap = this->focuserMaxPosition;
        if (focuserManualCalibrationMode)
        {
            getFocuserAbsoluteRange(minSnap, maxSnap);
        }
        const int startPosSnap = this->startPosition;
        const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);

        sdkFocuserExec->post([this, handleSnap, isInwardSnap, targetSnap, minSnap, maxSnap, startPosSnap, epochSnap]() {
            bool ok = false;
            int curPos = INT_MIN;
            std::string err;

            if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
            {
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        sdkFocuserPeriodicTaskInFlight = false;
                    },
                    Qt::QueuedConnection);
                return;
            }

            SdkCommand getPosCmd;
            getPosCmd.type = SdkCommandType::Custom;
            getPosCmd.name = "GetPosition";
            getPosCmd.payload = std::any();
            SdkResult posRes = SdkManager::instance().callByHandle(handleSnap, getPosCmd);
            if (posRes.success && posRes.payload.has_value())
            {
                try
                {
                    curPos = std::any_cast<int>(posRes.payload);
                    ok = true;
                }
                catch (const std::bad_any_cast &)
                {
                    err = "SDK GetPosition bad_any_cast";
                }
            }
            else
            {
                err = posRes.message;
            }

            bool issuedMove = false;
            int newTarget = targetSnap;
            int steps = 0;
            bool limitReached = false;
            std::string limitMsg;

            const bool isFirstSegment = (targetSnap == startPosSnap);
            if (ok && (curPos == targetSnap || isFirstSegment))
            {
                if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                {
                    QMetaObject::invokeMethod(
                        this,
                        [this]() {
                            sdkFocuserPeriodicTaskInFlight = false;
                        },
                        Qt::QueuedConnection);
                    return;
                }

                if (isInwardSnap)
                {
                    steps = curPos - minSnap;
                    if (steps > 60000)
                    {
                        steps = 60000;
                        newTarget = curPos - steps;
                    }
                    else
                    {
                        newTarget = minSnap;
                    }
                    if (steps <= 0)
                    {
                        maybeExpandFocuserLimitForCalibration(true, curPos);
                        limitReached = true;
                        limitMsg.clear();
                    }
                }
                else
                {
                    steps = maxSnap - curPos;
                    if (steps > 60000)
                    {
                        steps = 60000;
                        newTarget = curPos + steps;
                    }
                    else
                    {
                        newTarget = maxSnap;
                    }
                    if (steps <= 0)
                    {
                        maybeExpandFocuserLimitForCalibration(false, curPos);
                        limitReached = true;
                        limitMsg.clear();
                    }
                }

                if (!limitReached && steps > 0)
                {
                    if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this]() {
                                sdkFocuserPeriodicTaskInFlight = false;
                            },
                            Qt::QueuedConnection);
                        return;
                    }

                    SdkFocuserRelMoveParam p;
                    p.outward = !isInwardSnap;
                    p.steps = steps;
                    SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                    SdkResult mvRes = SdkManager::instance().callByHandle(handleSnap, moveCmd);
                    if (!mvRes.success)
                    {
                        err = "SDK MoveRelative failed: " + mvRes.message;
                    }
                    else
                    {
                        issuedMove = true;
                    }
                }
            }

            QMetaObject::invokeMethod(
                this,
                [this, ok, curPos, err, issuedMove, newTarget, isInwardSnap, steps, limitReached, limitMsg]() {
                    sdkFocuserPeriodicTaskInFlight = false;

                    if (sdkFocuserHandle == nullptr)
                        return;
                    if (wsThread == nullptr)
                        return;

                    if (!ok)
                    {
                        CurrentPosition = INT_MIN;
                        Logger::Log("HandleFocuserMovementDataPeriodically | SDK get current position failed: " + err,
                                    LogLevel::WARNING, DeviceType::FOCUSER);
                    }
                    else
                    {
                        CurrentPosition = curPos;
                        sdkFocuserPosCache = curPos;
                        sdkFocuserPosValid = true;
                        if (wsThread != nullptr)
                        {
                            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
                        }
                    }

                    if (limitReached)
                    {
                        if (wsThread != nullptr && !limitMsg.empty())
                        {
                            emit wsThread->sendMessageToClient(QString::fromStdString(limitMsg));
                        }
                        return;
                    }

                    if (issuedMove)
                    {
                        TargetPosition = newTarget;
                        Logger::Log("HandleFocuserMovementDataPeriodically | CurrentPosition: " + std::to_string(CurrentPosition) +
                                        " ,set move steps " + std::to_string(steps) +
                                        (isInwardSnap ? " ,backward inward" : " ,backward outward"),
                                    LogLevel::DEBUG, DeviceType::FOCUSER);
                    }

                    if (focusMoveTimer == nullptr)
                        return;
                    if (!focusMoveTimer->isActive())
                        return;

                    focusMoveEndTime -= 0.5;
                    CheckFocuserMoveOrder();
                    if (focusMoveEndTime <= 0)
                    {
                        FocuserControlStop();
                    }
                },
                Qt::QueuedConnection);
        });

        return;
    }

    Logger::Log("HandleFocuserMovementDataPeriodically | INDI Mode", LogLevel::DEBUG, DeviceType::FOCUSER);
    CurrentPosition = FocuserControl_getPosition();
    if (focuserIndiNeedResyncTarget)
    {
        TargetPosition = CurrentPosition;
        focuserIndiNeedResyncTarget = false;
    }
    if (CurrentPosition != TargetPosition)
    {
        if (focuserIndiHaveLastPollPos && focuserIndiLastPollPos == CurrentPosition)
            focuserIndiStallCount++;
        else
            focuserIndiStallCount = 0;
        focuserIndiLastPollPos = CurrentPosition;
        focuserIndiHaveLastPollPos = true;
        if (focuserIndiStallCount >= 2)
        {
            TargetPosition = CurrentPosition;
            focuserIndiStallCount = 0;
        }
    }
    else
    {
        focuserIndiStallCount = 0;
        focuserIndiHaveLastPollPos = false;
    }
    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));

    const bool isInward = this->currentDirection;
    Logger::Log("HandleFocuserMovementDataPeriodically | CurrentPosition: " + std::to_string(CurrentPosition) +
                    ", TargetPosition: " + std::to_string(TargetPosition) +
                    ", isInward: " + std::to_string(isInward),
                LogLevel::DEBUG, DeviceType::FOCUSER);

    int moveMin = focuserMinPosition;
    int moveMax = focuserMaxPosition;
    if (focuserManualCalibrationMode)
    {
        getFocuserAbsoluteRange(moveMin, moveMax);
    }

    if (isInward)
    {
        if (CurrentPosition == TargetPosition)
        {
            int steps = CurrentPosition - moveMin;
            if (steps > kIndiFocuserRelMoveChunkMax)
            {
                steps = kIndiFocuserRelMoveChunkMax;
                TargetPosition = CurrentPosition - steps;
            }
            else
            {
                TargetPosition = moveMin;
            }
            if (steps <= 0)
            {
                maybeExpandFocuserLimitForCalibration(true, CurrentPosition);
                return;
            }
            Logger::Log("HandleFocuserMovementDataPeriodically | INDI move inward, steps: " + std::to_string(steps), LogLevel::DEBUG, DeviceType::FOCUSER);
            indi_Client->setFocuserMoveDiretion(dpFocuser, isInward);
            indi_Client->moveFocuserSteps(dpFocuser, steps);
        }
        else
        {
            Logger::Log("HandleFocuserMovementDataPeriodically | INDI skip move (inward), CurrentPosition != TargetPosition", LogLevel::DEBUG, DeviceType::FOCUSER);
        }
    }
    else
    {
        if (TargetPosition == CurrentPosition)
        {
            int steps = moveMax - CurrentPosition;
            if (steps > kIndiFocuserRelMoveChunkMax)
            {
                steps = kIndiFocuserRelMoveChunkMax;
                TargetPosition = CurrentPosition + steps;
            }
            else
            {
                TargetPosition = moveMax;
            }
            if (steps <= 0)
            {
                maybeExpandFocuserLimitForCalibration(false, CurrentPosition);
                return;
            }
            Logger::Log("HandleFocuserMovementDataPeriodically | INDI move outward, steps: " + std::to_string(steps), LogLevel::DEBUG, DeviceType::FOCUSER);
            indi_Client->setFocuserMoveDiretion(dpFocuser, isInward);
            indi_Client->moveFocuserSteps(dpFocuser, steps);
        }
        else
        {
            Logger::Log("HandleFocuserMovementDataPeriodically | INDI skip move (outward), TargetPosition != CurrentPosition", LogLevel::DEBUG, DeviceType::FOCUSER);
        }
    }

    focusMoveEndTime -= 0.5;
    CheckFocuserMoveOrder();
    if (focusMoveEndTime <= 0)
    {
        FocuserControlStop();
    }
}

void MainWindow::FocuserControlMove(bool isInward)
{
    this->currentDirection = isInward;
    sdkFocuserOpEpoch.fetch_add(1, std::memory_order_relaxed);
    cancelStepMoveIfAny();
    sdkFocuserPeriodicTaskInFlight = false;
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (dpFocuser == NULL && !focuserSdkReady)
    {
        Logger::Log("FocuserControlMove | focuser not available (both INDI and SDK are NULL)", LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("FocusMoveDone:0:0");
        return;
    }

    if (updatePositionTimer != nullptr)
    {
        updatePositionTimer->stop();
        updatePositionTimer->deleteLater();
        updatePositionTimer = nullptr;
    }

    if (focuserSdkReady && sdkFocuserPosTaskInFlight.load())
    {
        Logger::Log("FocuserControlMove | Cancel SDK position task to avoid blocking move command", LogLevel::DEBUG, DeviceType::FOCUSER);
        sdkFocuserPosTaskInFlight = false;
    }

    focusMoveEndTime = 6;
    isFocusMoveDone = true;

    if (dpFocuser != NULL)
    {
        CurrentPosition = FocuserControl_getPosition();
    }
    else if (focuserSdkReady)
    {
        if (sdkFocuserPosValid.load())
        {
            CurrentPosition = sdkFocuserPosCache.load();
        }
        else
        {
            CurrentPosition = 0;
        }
    }
    TargetPosition = CurrentPosition;
    startPosition = CurrentPosition;
    if (dpFocuser != nullptr)
    {
        focuserIndiNeedResyncTarget = true;
        focuserIndiStallCount = 0;
        focuserIndiHaveLastPollPos = false;
    }

    Logger::Log("FocuserControlMove | Init Position - CurrentPosition: " + std::to_string(CurrentPosition) +
                    ", TargetPosition: " + std::to_string(TargetPosition) +
                    ", isInward: " + std::to_string(isInward),
                LogLevel::DEBUG, DeviceType::FOCUSER);
    if (!focuserManualCalibrationMode && CurrentPosition >= focuserMaxPosition && !isInward)
    {
        focuserIndiNeedResyncTarget = false;
        emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the outer limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
        focusMoveTimer->stop();
        return;
    }
    else if (!focuserManualCalibrationMode && CurrentPosition <= focuserMinPosition && isInward)
    {
        focuserIndiNeedResyncTarget = false;
        emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the inner limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
        focusMoveTimer->stop();
        return;
    }
    HandleFocuserMovementDataPeriodically();
    focusMoveTimer->start(1000);
}

void MainWindow::FocuserControlStop(bool isClickMove, bool silent)
{
    Q_UNUSED(isClickMove);

    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (dpFocuser == NULL && !focuserSdkReady)
    {
        Logger::Log("focusMoveStop | focuser not available (both INDI and SDK are NULL)", LogLevel::WARNING, DeviceType::FOCUSER);
        return;
    }
    sdkFocuserOpEpoch.fetch_add(1, std::memory_order_relaxed);
    cancelStepMoveIfAny();
    sdkFocuserPeriodicTaskInFlight = false;
    Logger::Log("focusMoveStop | Stop Focuser Move", LogLevel::INFO, DeviceType::FOCUSER);

    if (dpFocuser != NULL)
    {
        CurrentPosition = FocuserControl_getPosition();
    }
    else if (focuserSdkReady)
    {
        if (sdkFocuserPosValid.load())
        {
            CurrentPosition = sdkFocuserPosCache.load();
        }
        else
        {
            if (!sdkFocuserPosTaskInFlight.load())
            {
                requestSdkFocuserPositionUpdate(false);
            }
            CurrentPosition = sdkFocuserPosValid.load() ? sdkFocuserPosCache.load() : 0;
        }
    }

    if (dpFocuser != NULL)
    {
        indi_Client->abortFocuserMove(dpFocuser);
    }
    else if (focuserSdkReady)
    {
        if (sdkFocuserPosTaskInFlight.load())
        {
            Logger::Log("focusMoveStop | Cancel position task to ensure Abort command executes immediately", LogLevel::DEBUG, DeviceType::FOCUSER);
            sdkFocuserPosTaskInFlight = false;
        }

        if (sdkFocuserExec && sdkFocuserExec->isRunning())
        {
            const SdkDeviceHandle handleSnap = sdkFocuserHandle;
            sdkFocuserExec->post([handleSnap]() {
                SdkCommand abortCmd;
                abortCmd.type = SdkCommandType::Custom;
                abortCmd.name = "Abort";
                abortCmd.payload = std::any();
                SdkManager::instance().callByHandle(handleSnap, abortCmd);
            });
        }
    }

    if (focusMoveTimer->isActive())
    {
        focusMoveTimer->stop();
    }

    isFocusMoveDone = false;
    if (!silent)
    {
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        if (updatePositionTimer != nullptr)
        {
            updatePositionTimer->stop();
            updatePositionTimer->deleteLater();
            updatePositionTimer = nullptr;
        }
        updatePositionTimer = new QTimer(this);
        updatePositionTimer->setInterval(1000);
        updateCount = 0;

        connect(updatePositionTimer, &QTimer::timeout, [this, sdkSkipCount = 0]() mutable {
            if (isFocusMoveDone || updateCount >= 3)
            {
                updatePositionTimer->stop();
                updatePositionTimer->deleteLater();
                updatePositionTimer = nullptr;
                Logger::Log("focusMoveStop | Timer manually released", LogLevel::INFO, DeviceType::FOCUSER);
                return;
            }

            if (focusMoveTimer && focusMoveTimer->isActive())
            {
                updatePositionTimer->stop();
                updatePositionTimer->deleteLater();
                updatePositionTimer = nullptr;
                Logger::Log("focusMoveStop | Timer stopped due to focuser moving", LogLevel::INFO, DeviceType::FOCUSER);
                return;
            }

            if (sdkFocuserPosTaskInFlight.load())
            {
                sdkSkipCount++;
                if (sdkSkipCount >= 10)
                {
                    updatePositionTimer->stop();
                    updatePositionTimer->deleteLater();
                    updatePositionTimer = nullptr;
                    Logger::Log("focusMoveStop | Timer released after too many SDK in-flight skips", LogLevel::WARNING, DeviceType::FOCUSER);
                }
                return;
            }

            const bool focuserSdkReady =
                (systemdevicelist.system_devices.size() > 22 &&
                 systemdevicelist.system_devices[22].isSDKConnect &&
                 systemdevicelist.system_devices[22].isBind &&
                 sdkFocuserHandle != nullptr);

            if (dpFocuser == NULL && focuserSdkReady)
            {
                if (sdkFocuserPosValid.load())
                {
                    CurrentPosition = sdkFocuserPosCache.load();
                    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
                    Logger::Log("focusMoveStop | Current Focuser Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::FOCUSER);
                    sdkSkipCount = 0;
                    updateCount++;
                    return;
                }
                if (!sdkFocuserPosTaskInFlight.load())
                {
                    requestSdkFocuserPositionUpdate(false);
                    sdkSkipCount++;
                    if (sdkSkipCount >= 10)
                    {
                        updatePositionTimer->stop();
                        updatePositionTimer->deleteLater();
                        updatePositionTimer = nullptr;
                        Logger::Log("focusMoveStop | Timer released after SDK cache remained invalid", LogLevel::WARNING, DeviceType::FOCUSER);
                    }
                    return;
                }
            }
            else
            {
                CurrentPosition = FocuserControl_getPosition();
                sdkSkipCount = 0;
            }

            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
            Logger::Log("focusMoveStop | Current Focuser Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::FOCUSER);
            updateCount++;
        });
        updatePositionTimer->start();
    }
    else
    {
        if (updatePositionTimer != nullptr)
        {
            updatePositionTimer->stop();
            updatePositionTimer->deleteLater();
            updatePositionTimer = nullptr;
        }
    }

    Logger::Log("focusMoveStop | Current Focuser Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::FOCUSER);
}

void MainWindow::CheckFocuserMoveOrder()
{
    emit wsThread->sendMessageToClient("getFocuserMoveState");
}

void MainWindow::FocuserControlMoveStep(bool isInward, int steps)
{
    Logger::Log("FocuserControlMoveStep start ...", LogLevel::INFO, DeviceType::FOCUSER);
    if (isStepMoving)
    {
        Logger::Log("FocuserControlMoveStep | isStepMoving is true, return", LogLevel::INFO, DeviceType::FOCUSER);
        return;
    }

    sdkFocuserOpEpoch.fetch_add(1, std::memory_order_relaxed);
    sdkFocuserPeriodicTaskInFlight = false;

    if (updatePositionTimer != nullptr)
    {
        updatePositionTimer->stop();
        updatePositionTimer->deleteLater();
        updatePositionTimer = nullptr;
    }

    if (sdkFocuserPosTaskInFlight.load())
    {
        Logger::Log("FocuserControlMoveStep | Cancel position task to avoid blocking move command", LogLevel::DEBUG, DeviceType::FOCUSER);
        sdkFocuserPosTaskInFlight = false;
    }

    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (dpFocuser != NULL || focuserSdkReady)
    {
        cancelStepMoveIfAny();

        if (sdkFocuserPosValid.load())
        {
            CurrentPosition = sdkFocuserPosCache.load();
        }
        else
        {
            requestSdkFocuserPositionUpdate(false);
            CurrentPosition = sdkFocuserPosValid.load() ? sdkFocuserPosCache.load() : 0;
        }

        if (dpFocuser != NULL)
        {
            CurrentPosition = FocuserControl_getPosition();
        }

        if (isInward == false)
        {
            TargetPosition = CurrentPosition + steps;
        }
        else
        {
            TargetPosition = CurrentPosition - steps;
        }
        Logger::Log("FocuserControlMoveStep | Target Position: " + std::to_string(TargetPosition), LogLevel::INFO, DeviceType::FOCUSER);

        if (!focuserManualCalibrationMode && TargetPosition > focuserMaxPosition)
        {
            TargetPosition = focuserMaxPosition;
        }
        else if (!focuserManualCalibrationMode && TargetPosition < focuserMinPosition)
        {
            TargetPosition = focuserMinPosition;
        }
        steps = std::abs(TargetPosition - CurrentPosition);
        if (steps <= 0 && !isInward)
        {
            maybeExpandFocuserLimitForCalibration(false, CurrentPosition);
            return;
        }
        else if (steps <= 0 && isInward)
        {
            maybeExpandFocuserLimitForCalibration(true, CurrentPosition);
            return;
        }
        if (steps <= 0)
        {
            return;
        }
        isStepMoving = true;
        constexpr int kStepPollMs = 250;
        constexpr int kStepTimeoutMs = 1000;
        stepMoveOutTime = std::max(1, kStepTimeoutMs / kStepPollMs);
        if (dpFocuser != NULL)
        {
            indi_Client->setFocuserMoveDiretion(dpFocuser, isInward);
            indi_Client->moveFocuserSteps(dpFocuser, steps);
        }
        else if (focuserSdkReady)
        {
            SdkFocuserRelMoveParam p;
            p.outward = !isInward;
            p.steps = steps;
            if (sdkFocuserExec && sdkFocuserExec->isRunning())
            {
                const SdkDeviceHandle handleSnap = sdkFocuserHandle;
                const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
                sdkFocuserExec->post([this, handleSnap, p, epochSnap]() {
                    if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                        return;
                    SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                    SdkResult mvRes = SdkManager::instance().callByHandle(handleSnap, moveCmd);
                    if (!mvRes.success)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, msg = mvRes.message]() {
                                Logger::Log("FocuserControlMoveStep | SDK MoveRelative failed: " + msg,
                                            LogLevel::ERROR, DeviceType::FOCUSER);
                                isStepMoving = false;
                                emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(CurrentPosition));
                            },
                            Qt::QueuedConnection);
                    }
                });
            }
        }

        focusTimer.setSingleShot(true);
        disconnect(&focusTimer, &QTimer::timeout, this, nullptr);

        connect(&focusTimer, &QTimer::timeout, this, [this]() {
            stepMoveOutTime--;
            if (dpFocuser != NULL)
            {
                CurrentPosition = FocuserControl_getPosition();
                emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
            }
            else
            {
                if (sdkFocuserPosValid.load())
                {
                    CurrentPosition = sdkFocuserPosCache.load();
                    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
                }
                requestSdkFocuserPositionUpdate(true);
            }

            const bool hitSavedLimit = (CurrentPosition <= focuserMinPosition || CurrentPosition >= focuserMaxPosition);
            if ((!focuserManualCalibrationMode && hitSavedLimit) || stepMoveOutTime <= 0 || CurrentPosition == TargetPosition)
            {
                focusTimer.stop();
                disconnect(&focusTimer, &QTimer::timeout, this, nullptr);
                isStepMoving = false;
                Logger::Log("FocuserControlMoveStep | Focuser Move Complete!", LogLevel::INFO, DeviceType::FOCUSER);
                emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(CurrentPosition));
            }
            else
            {
                focusTimer.start(250);
            }
        });

        focusTimer.start(250);
    }
    else
    {
        Logger::Log("FocuserControlMoveStep | focuser not available (both INDI and SDK are NULL)", LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(0));
    }
    Logger::Log("FocuserControlMoveStep finish!", LogLevel::INFO, DeviceType::FOCUSER);
}

void MainWindow::cancelStepMoveIfAny()
{
    if (focusTimer.isActive())
        focusTimer.stop();
    disconnect(&focusTimer, &QTimer::timeout, this, nullptr);
    isStepMoving = false;
}

int MainWindow::FocuserControl_setSpeed(int speed)
{
    Logger::Log("FocuserControl_setSpeed start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
    if (dpFocuser != NULL)
    {
        int value, min, max, step;
        indi_Client->setFocuserSpeed(dpFocuser, speed);
        indi_Client->getFocuserSpeed(dpFocuser, value, min, max, step);
        Logger::Log("FocuserControl_setSpeed | Focuser Speed: " + std::to_string(value) + "," + std::to_string(min) + "," + std::to_string(max) + "," + std::to_string(step), LogLevel::INFO, DeviceType::FOCUSER);
        return value;
    }
    else if (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[22].isSDKConnect &&
             systemdevicelist.system_devices[22].isBind &&
             sdkFocuserHandle != nullptr)
    {
        if (sdkFocuserExec && sdkFocuserExec->isRunning())
        {
            const SdkDeviceHandle handleSnap = sdkFocuserHandle;
            const int speedSnap = speed;
            sdkFocuserExec->post([handleSnap, speedSnap]() {
                SdkCommand setCmd;
                setCmd.type = SdkCommandType::Custom;
                setCmd.name = "SetSpeed";
                setCmd.payload = speedSnap;
                SdkManager::instance().callByHandle(handleSnap, setCmd);
            });
        }
        return speed;
    }
    Logger::Log("FocuserControl_setSpeed finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    return speed;
}

int MainWindow::FocuserControl_getSpeed()
{
    Logger::Log("FocuserControl_getSpeed start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
    if (dpFocuser != NULL)
    {
        int value, min, max, step;
        indi_Client->getFocuserSpeed(dpFocuser, value, min, max, step);
        Logger::Log("FocuserControl_getSpeed | Focuser Speed: " + std::to_string(value) + "," + std::to_string(min) + "," + std::to_string(max) + "," + std::to_string(step), LogLevel::INFO, DeviceType::FOCUSER);
        return value;
    }
    else if (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[22].isSDKConnect &&
             systemdevicelist.system_devices[22].isBind &&
             sdkFocuserHandle != nullptr)
    {
        return currentSpeed;
    }
    Logger::Log("FocuserControl_getSpeed finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    return 0;
}

void MainWindow::requestSdkFocuserPositionUpdate(bool emitWs)
{
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);

    if (!focuserSdkReady)
        return;

    if (!sdkFocuserExec || !sdkFocuserExec->isRunning())
        return;

    if (sdkFocuserPosTaskInFlight.exchange(true))
        return;

    const SdkDeviceHandle handleSnap = sdkFocuserHandle;
    const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
    sdkFocuserExec->post([this, handleSnap, emitWs, epochSnap]() {
        if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
        {
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    sdkFocuserPosTaskInFlight = false;
                },
                Qt::QueuedConnection);
            return;
        }

        int pos = 0;
        std::string err;
        bool ok = false;

        SdkCommand getPosCmd;
        getPosCmd.type = SdkCommandType::Custom;
        getPosCmd.name = "GetPosition";
        getPosCmd.payload = std::any();
        SdkResult res = SdkManager::instance().callByHandle(handleSnap, getPosCmd);
        if (res.success && res.payload.has_value())
        {
            try
            {
                pos = std::any_cast<int>(res.payload);
                ok = true;
            }
            catch (const std::bad_any_cast &)
            {
                err = "SDK GetPosition bad_any_cast";
            }
        }
        else
        {
            err = res.message;
        }

        QMetaObject::invokeMethod(
            this,
            [this, ok, pos, err, emitWs]() {
                sdkFocuserPosTaskInFlight = false;

                if (sdkFocuserHandle == nullptr)
                    return;
                if (wsThread == nullptr)
                    return;

                if (!ok)
                {
                    Logger::Log("requestSdkFocuserPositionUpdate | SDK GetPosition failed: " + err,
                                LogLevel::DEBUG, DeviceType::FOCUSER);
                    return;
                }

                sdkFocuserPosCache = pos;
                sdkFocuserPosValid = true;
                CurrentPosition = pos;

                if (emitWs && wsThread != nullptr)
                {
                    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(pos) + ":" + QString::number(pos));
                }
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::requestSdkFocuserVersionUpdate(bool emitWs)
{
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (!focuserSdkReady)
        return;

    if (!sdkFocuserExec || !sdkFocuserExec->isRunning())
        return;

    const SdkDeviceHandle handleSnap = sdkFocuserHandle;
    const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
    sdkFocuserExec->post([this, handleSnap, emitWs, epochSnap]() {
        if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
            return;

        SdkCommand verCmd;
        verCmd.type = SdkCommandType::Custom;
        verCmd.name = "GetVersion";
        verCmd.payload = std::any();
        SdkResult verRes = SdkManager::instance().callByHandle(handleSnap, verCmd);

        QMetaObject::invokeMethod(
            this,
            [this, verRes, emitWs]() {
                if (!sdkFocuserHandle || wsThread == nullptr)
                    return;

                if (verRes.success && verRes.payload.has_value())
                {
                    try
                    {
                        SdkFocuserVersion ver = std::any_cast<SdkFocuserVersion>(verRes.payload);
                        if (emitWs)
                            emit wsThread->sendMessageToClient("getSDKVersion:Focuser:" + QString::number(ver.version));
                        Logger::Log("requestSdkFocuserVersionUpdate | SDK Focuser version: " + std::to_string(ver.version),
                                    LogLevel::DEBUG, DeviceType::FOCUSER);
                    }
                    catch (const std::bad_any_cast &)
                    {
                        Logger::Log("requestSdkFocuserVersionUpdate | bad_any_cast for SdkFocuserVersion",
                                    LogLevel::WARNING, DeviceType::FOCUSER);
                    }
                }
                else
                {
                    Logger::Log("requestSdkFocuserVersionUpdate | SDK GetVersion failed: " + verRes.message,
                                LogLevel::DEBUG, DeviceType::FOCUSER);
                }
            },
            Qt::QueuedConnection);
    });
}

int MainWindow::FocuserControl_getPosition()
{
    Logger::Log("FocuserControl_getPosition start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
    if (dpFocuser != NULL)
    {
        int value;
        indi_Client->getFocuserAbsolutePosition(dpFocuser, value);
        Logger::Log("FocuserControl_getPosition | Focuser Position: " + std::to_string(value), LogLevel::DEBUG, DeviceType::FOCUSER);
        return value;
    }
    else if (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[22].isSDKConnect &&
             systemdevicelist.system_devices[22].isBind &&
             sdkFocuserHandle != nullptr)
    {
        if (sdkFocuserPosValid.load())
        {
            const int pos = sdkFocuserPosCache.load();
            Logger::Log("FocuserControl_getPosition | SDK cached position: " + std::to_string(pos),
                        LogLevel::DEBUG, DeviceType::FOCUSER);
            return pos;
        }

        if (!sdkFocuserPosTaskInFlight.load())
        {
            requestSdkFocuserPositionUpdate(false);
        }
        Logger::Log("FocuserControl_getPosition | SDK cache invalid, requested async update, returning 0",
                    LogLevel::DEBUG, DeviceType::FOCUSER);
        return 0;
    }
    else
    {
        Logger::Log("FocuserControl_getPosition | dpFocuser is NULL", LogLevel::WARNING, DeviceType::FOCUSER);
        return 0;
    }
}

bool MainWindow::tryReadStableFocuserPosition(int &stablePosition,
                                              int timeoutMs,
                                              int sampleIntervalMs,
                                              int requiredStableSamples,
                                              int toleranceSteps)
{
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    const bool focuserConnected = (dpFocuser != nullptr || focuserSdkReady);
    if (!focuserConnected)
    {
        Logger::Log("tryReadStableFocuserPosition | focuser not connected", LogLevel::WARNING, DeviceType::FOCUSER);
        return false;
    }

    timeoutMs = std::max(timeoutMs, 800);
    sampleIntervalMs = std::max(sampleIntervalMs, 80);
    requiredStableSamples = std::max(requiredStableSamples, 2);
    toleranceSteps = std::max(toleranceSteps, 1);

    QElapsedTimer timer;
    timer.start();
    QVector<int> recentSamples;

    auto readPositionSample = [&]() -> bool {
        if (dpFocuser == nullptr && focuserSdkReady)
        {
            requestSdkFocuserPositionUpdate(false);
            int waited = 0;
            while (sdkFocuserPosTaskInFlight.load() && waited < 600)
            {
                QThread::msleep(20);
                QCoreApplication::processEvents();
                waited += 20;
            }

            if (!sdkFocuserPosValid.load())
            {
                return false;
            }
        }

        const int current = FocuserControl_getPosition();
        recentSamples.push_back(current);
        if (recentSamples.size() > requiredStableSamples)
        {
            recentSamples.remove(0);
        }
        return true;
    };

    while (timer.elapsed() < timeoutMs)
    {
        if (!readPositionSample())
        {
            recentSamples.clear();
            QThread::msleep(sampleIntervalMs);
            continue;
        }

        if (recentSamples.size() >= requiredStableSamples)
        {
            auto minMaxIt = std::minmax_element(recentSamples.begin(), recentSamples.end());
            auto minIt = minMaxIt.first;
            auto maxIt = minMaxIt.second;
            if ((*maxIt - *minIt) <= toleranceSteps)
            {
                QVector<int> sorted = recentSamples;
                std::sort(sorted.begin(), sorted.end());
                stablePosition = sorted[sorted.size() / 2];
                Logger::Log("tryReadStableFocuserPosition | stable position=" + std::to_string(stablePosition) +
                                ", samples=" + std::to_string(sorted[0]) + "," +
                                std::to_string(sorted[sorted.size() - 1]),
                            LogLevel::INFO, DeviceType::FOCUSER);
                return true;
            }
        }

        QThread::msleep(sampleIntervalMs);
    }

    Logger::Log("tryReadStableFocuserPosition | timeout, latest samples count=" +
                    std::to_string(recentSamples.size()),
                LogLevel::WARNING, DeviceType::FOCUSER);
    return false;
}

void MainWindow::focusMoveToMin()
{
    emit wsThread->sendMessageToClient("focusMoveToMinStarted");
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (dpFocuser == nullptr && !focuserSdkReady)
    {
        Logger::Log("focusMoveToMin | focuser is not connected (both INDI and SDK are NULL)", LogLevel::ERROR, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("focusMoveFailed:focuser is not connected");
        return;
    }

    int min = focuserMinPosition;
    int max = focuserMaxPosition;
    int step = 0;
    int value = 0;
    if (dpFocuser != nullptr)
    {
        indi_Client->getFocuserRange(dpFocuser, min, max, step, value);
        indi_Client->syncFocuserPosition(dpFocuser, (max + min) / 2);
    }
    if (min == -1 && max == -1)
    {
        min = -64000;
        max = 64000;
    }

    CurrentPosition = FocuserControl_getPosition();
    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));

    int steps = std::min(std::max(0, CurrentPosition - min), kIndiFocuserRelMoveChunkMax);
    Logger::Log("focusMoveToMin | Moving to minimum position: " + std::to_string(min) + " with steps: " + std::to_string(steps), LogLevel::INFO, DeviceType::FOCUSER);
    if (steps <= 0)
    {
        emit wsThread->sendMessageToClient("focusMoveFailed:already at minimum or invalid range");
        return;
    }

    if (dpFocuser != nullptr)
    {
        indi_Client->setFocuserMoveDiretion(dpFocuser, true);
        indi_Client->moveFocuserSteps(dpFocuser, steps);
    }
    else if (focuserSdkReady)
    {
        SdkFocuserRelMoveParam p;
        p.outward = false;
        p.steps = steps;
        if (sdkFocuserExec && sdkFocuserExec->isRunning())
        {
            const SdkDeviceHandle handleSnap = sdkFocuserHandle;
            const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
            sdkFocuserExec->post([handleSnap, p, this, epochSnap]() {
                if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                    return;
                SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                SdkManager::instance().callByHandle(handleSnap, moveCmd);
            });
        }
    }
    TargetPosition = CurrentPosition - steps;
    focusMoveToMaxorMinTimer = new QTimer(this);
    CurrentPosition = FocuserControl_getPosition();
    lastPosition = CurrentPosition;
    std::shared_ptr<int> noChangeCount = std::make_shared<int>(0);
    connect(focusMoveToMaxorMinTimer, &QTimer::timeout, this, [this, min, focuserSdkReady, noChangeCount]() {
        if (dpFocuser == nullptr && focuserSdkReady)
        {
            if (!sdkFocuserPosTaskInFlight.load())
            {
                requestSdkFocuserPositionUpdate(false);
            }
        }
        CurrentPosition = FocuserControl_getPosition();
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        if (CurrentPosition == TargetPosition)
        {
            int steps = std::min(std::max(0, CurrentPosition - min), kIndiFocuserRelMoveChunkMax);
            if (steps <= 0)
            {
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the inner limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
                return;
            }

            if (dpFocuser != nullptr)
            {
                indi_Client->moveFocuserSteps(dpFocuser, steps);
            }
            else if (focuserSdkReady)
            {
                SdkFocuserRelMoveParam p;
                p.outward = false;
                p.steps = steps;
                if (sdkFocuserExec && sdkFocuserExec->isRunning())
                {
                    const SdkDeviceHandle handleSnap = sdkFocuserHandle;
                    const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
                    sdkFocuserExec->post([handleSnap, p, this, epochSnap]() {
                        if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                            return;
                        SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                        SdkManager::instance().callByHandle(handleSnap, moveCmd);
                    });
                }
            }
            TargetPosition = CurrentPosition - steps;
            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
            *noChangeCount = 0;
            return;
        }
        if (CurrentPosition == lastPosition)
        {
            (*noChangeCount)++;

            if (CurrentPosition <= min)
            {
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the inner limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
                return;
            }

            if (*noChangeCount >= 5)
            {
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("focusMoveFailed:focuser appears to be stuck - position not changing for 3 seconds and not at physical limit");
                return;
            }
            return;
        }

        *noChangeCount = 0;
        lastPosition = CurrentPosition;
    });
    focusMoveToMaxorMinTimer->start(1000);
    Logger::Log("focusMoveToMin | Started moving to minimum position: " + std::to_string(min), LogLevel::INFO, DeviceType::FOCUSER);
}

void MainWindow::focusMoveToMax()
{
    emit wsThread->sendMessageToClient("focusMoveToMaxStarted");
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (dpFocuser == nullptr && !focuserSdkReady)
    {
        Logger::Log("focusMoveToMax | focuser is not connected (both INDI and SDK are NULL)", LogLevel::ERROR, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("focusMoveFailed:focuser is not connected");
        return;
    }

    FocuserControlStop();
    if (dpFocuser == nullptr && focuserSdkReady)
    {
        sleep(1);
        requestSdkFocuserPositionUpdate(true);
        sleep(1);
        requestSdkFocuserPositionUpdate(true);
        sleep(1);
        requestSdkFocuserPositionUpdate(true);
        sleep(1);
    }
    else
    {
        int timeout = 0;
        while (timeout <= 3)
        {
            CurrentPosition = FocuserControl_getPosition();
            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
            sleep(1);
            timeout++;
        }
    }
    CurrentPosition = FocuserControl_getPosition();
    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
    sleep(1);
    CurrentPosition = FocuserControl_getPosition();
    focuserMinPosition = CurrentPosition;

    sleep(1);
    int finalPosition = FocuserControl_getPosition();
    if (finalPosition < focuserMinPosition)
    {
        Logger::Log("focusMoveToMax | Position drifted due to inertia, adjusting min limit from " +
                        std::to_string(focuserMinPosition) + " to " + std::to_string(finalPosition),
                    LogLevel::WARNING, DeviceType::FOCUSER);
        focuserMinPosition = finalPosition;
        CurrentPosition = finalPosition;
    }

    Tools::saveParameter("Focuser", "focuserMinPosition", QString::number(focuserMinPosition));
    Logger::Log("focusMoveToMax | Set min position to: " + std::to_string(focuserMinPosition),
                LogLevel::INFO, DeviceType::FOCUSER);

    int min = focuserMinPosition;
    int max = focuserMaxPosition;
    int step = 0;
    int value = 0;
    if (dpFocuser != nullptr)
        indi_Client->getFocuserRange(dpFocuser, min, max, step, value);
    if (min == -1 && max == -1)
    {
        min = -64000;
        max = 64000;
    }

    int steps = std::min(std::max(0, max - CurrentPosition), kIndiFocuserRelMoveChunkMax);
    if (steps <= 0)
    {
        emit wsThread->sendMessageToClient("focusMoveFailed:already at maximum or invalid range");
        return;
    }

    if (dpFocuser != nullptr)
    {
        indi_Client->setFocuserMoveDiretion(dpFocuser, false);
        indi_Client->moveFocuserSteps(dpFocuser, steps);
    }
    else if (focuserSdkReady)
    {
        SdkFocuserRelMoveParam p;
        p.outward = true;
        p.steps = steps;
        if (sdkFocuserExec && sdkFocuserExec->isRunning())
        {
            const SdkDeviceHandle handleSnap = sdkFocuserHandle;
            const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
            sdkFocuserExec->post([handleSnap, p, this, epochSnap]() {
                if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                    return;
                SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                SdkManager::instance().callByHandle(handleSnap, moveCmd);
            });
        }
    }
    TargetPosition = CurrentPosition + steps;
    if (focusMoveToMaxorMinTimer != nullptr)
    {
        focusMoveToMaxorMinTimer->stop();
        focusMoveToMaxorMinTimer->deleteLater();
        focusMoveToMaxorMinTimer = nullptr;
    }
    focusMoveToMaxorMinTimer = new QTimer(this);
    CurrentPosition = FocuserControl_getPosition();
    lastPosition = CurrentPosition;
    std::shared_ptr<int> noChangeCount = std::make_shared<int>(0);
    connect(focusMoveToMaxorMinTimer, &QTimer::timeout, this, [this, max, focuserSdkReady, noChangeCount]() {
        if (dpFocuser == nullptr && focuserSdkReady)
        {
            if (!sdkFocuserPosTaskInFlight.load())
            {
                requestSdkFocuserPositionUpdate(false);
            }
        }
        CurrentPosition = FocuserControl_getPosition();
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        if (CurrentPosition == TargetPosition)
        {
            int steps = std::min(std::max(0, max - CurrentPosition), kIndiFocuserRelMoveChunkMax);
            if (steps <= 0)
            {
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the outer limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
                return;
            }
            if (dpFocuser != nullptr)
            {
                indi_Client->moveFocuserSteps(dpFocuser, steps);
            }
            else if (focuserSdkReady)
            {
                SdkFocuserRelMoveParam p;
                p.outward = true;
                p.steps = steps;
                if (sdkFocuserExec && sdkFocuserExec->isRunning())
                {
                    const SdkDeviceHandle handleSnap = sdkFocuserHandle;
                    const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
                    sdkFocuserExec->post([handleSnap, p, this, epochSnap]() {
                        if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                            return;
                        SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                        SdkManager::instance().callByHandle(handleSnap, moveCmd);
                    });
                }
            }
            TargetPosition = CurrentPosition + steps;
            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
            *noChangeCount = 0;
            return;
        }
        if (CurrentPosition == lastPosition)
        {
            (*noChangeCount)++;

            if (CurrentPosition >= max)
            {
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the outer limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
                return;
            }

            if (*noChangeCount >= 5)
            {
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("focusMoveFailed:focuser appears to be stuck - position not changing for 3 seconds and not at physical limit");
                return;
            }
            return;
        }

        *noChangeCount = 0;
        lastPosition = CurrentPosition;
    });
    focusMoveToMaxorMinTimer->start(1000);
    Logger::Log("focusMoveToMax | Started moving to maximum position: " + std::to_string(max), LogLevel::INFO, DeviceType::FOCUSER);
}

void MainWindow::focusSetTravelRange()
{
    emit wsThread->sendMessageToClient("focusSetTravelRangeStarted");
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);

    if (dpFocuser == nullptr && !focuserSdkReady)
    {
        Logger::Log("focusSetTravelRange | focuser is not connected (both INDI and SDK are NULL)",
                    LogLevel::ERROR, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("focusMoveFailed:focuser is not connected");
        return;
    }

    if (focusMoveToMaxorMinTimer != nullptr)
    {
        focusMoveToMaxorMinTimer->stop();
        focusMoveToMaxorMinTimer->deleteLater();
        focusMoveToMaxorMinTimer = nullptr;
    }

    FocuserControlStop();

    auto waitSdkPositionTaskDone = [this]() {
        int waitCount = 0;
        while (sdkFocuserPosTaskInFlight.load() && waitCount < 100)
        {
            QThread::msleep(50);
            QCoreApplication::processEvents();
            waitCount++;
        }
        QThread::msleep(200);
        QCoreApplication::processEvents();
    };

    auto updateSdkPositionOnce = [&]() {
        requestSdkFocuserPositionUpdate(true);
        waitSdkPositionTaskDone();
    };

    auto readPositionOnce = [&]() -> int {
        if (dpFocuser == nullptr && focuserSdkReady)
        {
            updateSdkPositionOnce();
        }
        return FocuserControl_getPosition();
    };

    int stablePosition = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (i == 0)
        {
            sleep(1);
        }

        stablePosition = readPositionOnce();
        CurrentPosition = stablePosition;

        emit wsThread->sendMessageToClient(
            "FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));

        if (i < 2)
        {
            sleep(1);
        }
    }

    focuserMaxPosition = stablePosition;

    emit wsThread->sendMessageToClient("focusSetTravelRangeSuccess");
    emit wsThread->sendMessageToClient(
        "FocuserLimit:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition));
    emit wsThread->sendMessageToClient(
        "FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));

    Tools::saveParameter("Focuser", "focuserMaxPosition", QString::number(focuserMaxPosition));
    Tools::saveParameter("Focuser", "focuserMinPosition", QString::number(focuserMinPosition));

    Logger::Log("focusSetTravelRange | Calibration complete - Min: " + std::to_string(focuserMinPosition) +
                    ", Max: " + std::to_string(focuserMaxPosition) +
                    ", Current: " + std::to_string(CurrentPosition),
                LogLevel::INFO, DeviceType::FOCUSER);
}

void MainWindow::getFocuserParameters()
{
    QMap<QString, QString> parameters = Tools::readParameters("Focuser");
    if (parameters.contains("focuserMaxPosition") && parameters.contains("focuserMinPosition"))
    {
        focuserMaxPosition = parameters["focuserMaxPosition"].toInt();
        focuserMinPosition = parameters["focuserMinPosition"].toInt();
    }
    else
    {
        focuserMaxPosition = -1;
        focuserMinPosition = -1;
    }
    Logger::Log("Focuser Max Position: " + std::to_string(focuserMaxPosition) + ", Min Position: " + std::to_string(focuserMinPosition), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("Focuser Current Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::MAIN);

    const bool focuserAvailable = (dpFocuser != nullptr || isFocuserSDK());
    if (focuserAvailable)
    {
        int stablePosition = 0;
        if (tryReadStableFocuserPosition(stablePosition, 1800, 160, 2, 6))
        {
            CurrentPosition = stablePosition;
        }
        else
        {
            CurrentPosition = FocuserControl_getPosition();
            Logger::Log("getFocuserParameters | stable position read timeout, fallback position=" + std::to_string(CurrentPosition),
                        LogLevel::WARNING, DeviceType::FOCUSER);
        }
    }

    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
    emit wsThread->sendMessageToClient("FocuserLimit:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition));

    int emptyStep = parameters.contains("Backlash") ? parameters["Backlash"].toInt() : 0;
    autofocusBacklashCompensation = emptyStep;

    int coarseDivisions = parameters.contains("coarseStepDivisions") ? parameters["coarseStepDivisions"].toInt() : 10;
    if (coarseDivisions <= 0)
    {
        coarseDivisions = 10;
    }
    autoFocusCoarseDivisions = coarseDivisions;

    int stepsPerClick = parameters.contains("StepsPerClick") ? parameters["StepsPerClick"].toInt() : 50;
    if (stepsPerClick <= 0)
    {
        stepsPerClick = 50;
    }

    emit wsThread->sendMessageToClient(
        "FocuserParameters:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition) + ":" +
        QString::number(emptyStep) + ":" + QString::number(autoFocusCoarseDivisions) + ":" + QString::number(stepsPerClick));

    sendSerialPortOptions("Focuser");
}

void MainWindow::getFocuserState()
{
    QString state = isAutoFocus ? "true" : "false";
    emit wsThread->sendMessageToClient("updateAutoFocuserState:" + state);

    if (isAutoFocus && autoFocus != nullptr)
    {
        emit wsThread->sendMessageToClient("AutoFocusStarted:自动对焦已开始");
        autoFocus->getAutoFocusStep();
    }

    if (isAutoFocus && autoFocus != nullptr)
    {
        autoFocus->getAutoFocusData();
    }
}
