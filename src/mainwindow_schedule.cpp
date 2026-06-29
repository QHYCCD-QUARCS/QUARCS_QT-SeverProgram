#include "mainwindow_command_support.h"

void MainWindow::ScheduleTabelData(QString message)
{
    ScheduleTargetNames.clear();
    m_scheduList.clear();
    // 每次接收到新的任务计划表时，从第一条任务开始执行
    schedule_currentNum = 0;
    schedule_currentShootNum = 0;
    // 新任务计划表开始时，重置 Refocus 触发记录，避免旧任务残留导致本次不触发
    schedule_refocusTriggeredIndex = -1;
    QStringList ColDataList = message.split('[');
    for (int i = 1; i < ColDataList.size(); ++i)
    {
        QString ColData = ColDataList[i];
        ScheduleData rowData;
        rowData.exposureDelay = 0;
        qDebug() << "ColData[" << i << "]:" << ColData;

        QStringList RowDataList = ColData.split(',');
        if (RowDataList.size() <= 10)
        {
            Logger::Log(QString("ScheduleTabelData | row %1 has insufficient columns: %2").arg(i).arg(RowDataList.size()).toStdString(),
                        LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }

        for (int j = 1; j <= 10; ++j)
        {
            if (j >= RowDataList.size())
            {
                Logger::Log(QString("ScheduleTabelData | row %1 column index %2 out of range (size=%3)")
                                .arg(i).arg(j).arg(RowDataList.size()).toStdString(),
                            LogLevel::WARNING, DeviceType::MAIN);
                break;
            }

            if (j == 1)
            {
                rowData.shootTarget = RowDataList[j];
                rowData.shootType = RowDataList[j + 7];
                qDebug() << "Target:" << rowData.shootTarget;
                qDebug() << "Type:" << rowData.shootType;
                if (!ScheduleTargetNames.isEmpty())
                {
                    ScheduleTargetNames += ",";
                }
                ScheduleTargetNames += rowData.shootTarget + "," + rowData.shootType;
            }
            else if (j == 2)
            {
                QStringList parts = RowDataList[j].split(':');
                if (parts.size() >= 2)
                {
                    rowData.targetRa = Tools::RadToHour(parts[1].toDouble());
                }
                else
                {
                    rowData.targetRa = 0;
                    Logger::Log(QString("ScheduleTabelData | row %1 invalid RA field: %2").arg(i).arg(RowDataList[j]).toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                }
                qDebug() << "Ra:" << rowData.targetRa;
            }
            else if (j == 3)
            {
                QStringList parts = RowDataList[j].split(':');
                if (parts.size() >= 2)
                {
                    rowData.targetDec = Tools::RadToDegree(parts[1].toDouble());
                }
                else
                {
                    rowData.targetDec = 0;
                    Logger::Log(QString("ScheduleTabelData | row %1 invalid Dec field: %2").arg(i).arg(RowDataList[j]).toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                }
                qDebug() << "Dec:" << rowData.targetDec;
            }
            else if (j == 4)
            {
                rowData.shootTime = RowDataList[j];
                qDebug() << "Time:" << rowData.shootTime;
            }
            else if (j == 5)
            {
                QStringList parts = RowDataList[j].split(' ');
                if (parts.isEmpty())
                {
                    rowData.exposureTime = 1000;
                    Logger::Log(QString("ScheduleTabelData | row %1 invalid exposure field: %2, fallback to 1000ms")
                                    .arg(i).arg(RowDataList[j]).toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                    qDebug() << "Exptime error, Exptime = 1000 ms";
                    continue;
                }

                QString value = parts[0];
                QString unit = (parts.size() > 1) ? parts[1] : "s";
                if (unit == "s")
                    rowData.exposureTime = value.toInt() * 1000;
                else if (unit == "ms")
                    rowData.exposureTime = value.toInt();
                if (rowData.exposureTime == 0)
                {
                    rowData.exposureTime = 1000;
                    qDebug() << "Exptime error, Exptime = 1000 ms";
                }
                qDebug() << "Exptime:" << rowData.exposureTime;
            }
            else if (j == 6)
            {
                rowData.filterNumber = RowDataList[j];
                qDebug() << "CFW:" << rowData.filterNumber;
            }
            else if (j == 7)
            {
                if (RowDataList[j] == "")
                {
                    rowData.repeatNumber = 1;
                    qDebug() << "Repeat error, Repeat = 1";
                }
                else
                {
                    rowData.repeatNumber = RowDataList[j].toInt();
                }
                qDebug() << "Repeat:" << rowData.repeatNumber;
            }
            else if (j == 9)
            {
                rowData.resetFocusing = (RowDataList[j] == "ON");
                qDebug() << "Focus:" << rowData.resetFocusing;
            }
            else if (j == 10)
            {
                QStringList parts = RowDataList[j].split(' ');
                if (parts.isEmpty())
                {
                    rowData.exposureDelay = 0;
                    Logger::Log(QString("ScheduleTabelData | row %1 invalid exposure delay field: %2, fallback to 0")
                                    .arg(i).arg(RowDataList[j]).toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                    qDebug() << "Exposure Delay error, use 0 ms";
                    continue;
                }

                QString value = parts[0];
                QString unit = parts.size() > 1 ? parts[1] : "s";
                if (unit == "s")
                    rowData.exposureDelay = value.toInt() * 1000;
                else if (unit == "ms")
                    rowData.exposureDelay = value.toInt();
                else
                    rowData.exposureDelay = 0;
                qDebug() << "Exposure Delay:" << rowData.exposureDelay << "ms";
            }
        }
        rowData.progress = 0;
        m_scheduList.append(rowData);
    }

    if (message.startsWith("ScheduleTabelData:"))
    {
        QString stagingMessage = message;
        stagingMessage.replace(0,
                               QString("ScheduleTabelData:").length(),
                               "StagingScheduleData:");
        isStagingScheduleData = true;
        StagingScheduleData = stagingMessage;
    }

    startSchedule();
}

void MainWindow::startSchedule()
{
    createScheduleDirectory();
    if (schedule_currentNum >= 0 && schedule_currentNum < m_scheduList.size())
    {
        qDebug() << "startSchedule...";
        schedule_ExpTime = m_scheduList[schedule_currentNum].exposureTime;
        schedule_RepeatNum = m_scheduList[schedule_currentNum].repeatNumber;
        schedule_CFWpos = m_scheduList[schedule_currentNum].filterNumber.toInt();
        schedule_ExposureDelay = m_scheduList[schedule_currentNum].exposureDelay;
        StopSchedule = false;
        isScheduleRunning = true;
        emit wsThread->sendMessageToClient("ScheduleRunning:true");
        startTimeWaiting();
    }
    else
    {
        qDebug() << "Index out of range, Schedule is complete!";
        StopSchedule = true;
        isScheduleRunning = false;
        schedule_currentNum = 0;
        GuidingHasStarted = false;
        emit wsThread->sendMessageToClient("ScheduleComplete");
        emit wsThread->sendMessageToClient("ScheduleRunning:false");
    }
}

void MainWindow::nextSchedule()
{
    schedule_currentNum++;
    qDebug() << "next schedule...";
    startSchedule();
}

void MainWindow::startTimeWaiting()
{
    qDebug() << "startTimeWaiting...";
    timewaitingTimer.stop();
    timewaitingTimer.disconnect();
    timewaitingTimer.setSingleShot(true);
    connect(&timewaitingTimer, &QTimer::timeout, [this]()
            {
        if (StopSchedule)
        {
            StopSchedule = false;
            qDebug("Schedule is stop!");
            return;
        }

        if (WaitForTimeToComplete())
        {
            timewaitingTimer.stop();
            qDebug() << "Time Waiting Complete...";
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(1, 1.0);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "wait:" +
                "0:" +
                "0:" +
                "100");

            startMountGoto(m_scheduList[schedule_currentNum].targetRa, m_scheduList[schedule_currentNum].targetDec);
        }
        else
        {
            timewaitingTimer.start(1000);
        } });
    timewaitingTimer.start(1000);
}

void MainWindow::startMountGoto(double ra, double dec)
{
    if (dpMount == NULL)
    {
        m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(2, 1.0);
        emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
        emit wsThread->sendMessageToClient(
            "ScheduleStepState:" +
            QString::number(schedule_currentNum) + ":" +
            "mount:" +
            "0:" +
            "0:" +
            "100");
        Logger::Log("startMountGoto | dpMount is NULL,goto failed!Skip to set CFW.", LogLevel::ERROR, DeviceType::MAIN);
        startSetCFW(schedule_CFWpos);
        return;
    }

    qDebug() << "startMountGoto...";
    telescopeTimer.stop();
    telescopeTimer.disconnect();

    qDebug() << "Mount Goto:" << ra << "," << dec;
    MountGotoError = false;

    auto now = std::chrono::system_clock::now();
    double lst = computeLST(observatorylongitude, now);

    double RA_HOURS, DEC_DEGREE;
    indi_Client->getTelescopeRADECJNOW(dpMount, RA_HOURS, DEC_DEGREE);
    double CurrentDEC_Degree = DEC_DEGREE;

    pauseGuidingBeforeMountMove();

    performObservation(
        lst, CurrentDEC_Degree,
        ra, dec,
        observatorylongitude, observatorylatitude);

    sleep(2);

    m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(2, 0.5);
    emit wsThread->sendMessageToClient(
        "UpdateScheduleProcess:" +
        QString::number(schedule_currentNum) + ":" +
        QString::number(m_scheduList[schedule_currentNum].progress));
    emit wsThread->sendMessageToClient(
        "ScheduleStepState:" +
        QString::number(schedule_currentNum) + ":" +
        "mount:" +
        "0:" +
        "0:" +
        "0");

    telescopeTimer.setSingleShot(true);

    connect(&telescopeTimer, &QTimer::timeout, [this]()
            {
        if (StopSchedule)
        {
            StopSchedule = false;
            qDebug("Schedule is stop!");

            if (dpMount != NULL)
            {
                indi_Client->setTelescopeAbortMotion(dpMount);
            }

            return;
        }
        if (WaitForTelescopeToComplete())
        {
            telescopeTimer.stop();
            qDebug() << "Mount Goto Complete!";

            if (MountGotoError) {
                MountGotoError = false;
                nextSchedule();
                return;
            }

            resumeGuidingAfterMountMove();

            qDebug() << "Mount Goto Complete...";
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(2, 1.0);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "mount:" +
                "0:" +
                "0:" +
                "100");
            startSetCFW(schedule_CFWpos);
        }
        else
        {
            telescopeTimer.start(1000);
        } });

    telescopeTimer.start(1000);
}

bool MainWindow::needsMeridianFlip(double lst, double targetRA)
{
    double hourAngle = lst - targetRA;
    hourAngle = fmod(hourAngle + 24.0, 24.0);
    if (hourAngle > 12.0)
        hourAngle -= 24.0;
    return (hourAngle > 0);
}

void MainWindow::performObservation(
    double lst, double currentDec,
    double targetRA, double targetDec,
    double observatoryLongitude, double observatoryLatitude)
{
    std::cout << "No flip needed. Moving directly." << std::endl;
    TelescopeControl_Goto(targetRA, targetDec);
}

double MainWindow::getJulianDate(const std::chrono::system_clock::time_point &utc_time)
{
    auto duration = utc_time.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    return 2440587.5 + seconds / 86400.0;
}

double MainWindow::computeGMST(const std::chrono::system_clock::time_point &utc_time)
{
    const double JD = getJulianDate(utc_time);
    const double T = (JD - 2451545.0) / 36525.0;

    double GMST = 280.46061837 + 360.98564736629 * (JD - 2451545.0) + 0.000387933 * T * T - T * T * T / 38710000.0;

    GMST = fmod(GMST, 360.0);
    if (GMST < 0)
        GMST += 360.0;

    return GMST / 15.0;
}

double MainWindow::computeLST(double longitude_east, const std::chrono::system_clock::time_point &utc_time)
{
    double GMST_hours = computeGMST(utc_time);
    double LST = GMST_hours + longitude_east / 15.0;
    return fmod(LST + 24.0, 24.0);
}

void MainWindow::startGuiding()
{
    qDebug() << "startGuiding...";
    GuidingHasStarted = false;
    startSetCFW(schedule_CFWpos);
}

void MainWindow::startSetCFW(int pos)
{
    qDebug() << "startSetCFW...";

    if (schedule_currentNum >= 0 && schedule_currentNum < m_scheduList.size() &&
        m_scheduList[schedule_currentNum].resetFocusing &&
        schedule_refocusTriggeredIndex != schedule_currentNum)
    {
        qDebug() << "Refocus is ON, starting autofocus before setting CFW...";
        Logger::Log("计划任务表: Refocus为ON，在执行拍摄前先执行自动对焦（仅最后一步精调）", LogLevel::INFO, DeviceType::MAIN);
        schedule_refocusTriggeredIndex = schedule_currentNum;
        startScheduleAutoFocus();
        return;
    }

    if (isFilterOnCamera)
    {
        if (!isMainCameraSDK() && dpMainCamera != NULL)
        {
            qDebug() << "schedule CFW pos:" << pos;
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 0.5);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "50");
            indi_Client->setCFWPosition(dpMainCamera, pos);
            qDebug() << "CFW Goto Complete...";
            startCapture(schedule_ExpTime);
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 1.0);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "100");
        }
        else if (isMainCameraSDK() && sdkMainCameraHandle != nullptr)
        {
            qDebug() << "schedule CFW pos (SDK 1-based):" << pos;
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 0.5);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "50");

            int target0 = toSdkCfwPos0(pos);
            std::string err;
            bool ok = sdkSetCfwPosition0AndWait(sdkMainCameraHandle, target0, 10000, &err);
            if (!ok)
            {
                Logger::Log("startSetCFW | SDK set CFW failed: " + err + " (continue capture)", LogLevel::WARNING, DeviceType::MAIN);
            }
            qDebug() << "CFW Goto Complete (SDK)...";
            startCapture(schedule_ExpTime);

            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 1.0);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "100");
        }
        else
        {
            Logger::Log("startSetCFW | dpMainCamera is NULL,set CFW failed!", LogLevel::ERROR, DeviceType::MAIN);
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 1.0);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            startCapture(schedule_ExpTime);
        }
    }
    else
    {
        if (dpCFW != NULL)
        {
            qDebug() << "schedule CFW pos:" << pos;
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 0.5);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "50");
            indi_Client->setCFWPosition(dpCFW, pos);
            qDebug() << "CFW Goto Complete...";
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 1.0);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "100");
            startCapture(schedule_ExpTime);
        }
        else
        {
            Logger::Log("startSetCFW | dpCFW is NULL,set CFW failed!", LogLevel::ERROR, DeviceType::MAIN);
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 1.0);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            startCapture(schedule_ExpTime);
        }
    }
}

void MainWindow::startExposureDelay()
{
    qDebug() << "startExposureDelay...";
    Logger::Log(("Waiting exposure delay: " + QString::number(schedule_ExposureDelay) + " ms before next capture").toStdString(), LogLevel::INFO, DeviceType::MAIN);
    qDebug() << "Waiting exposure delay:" << schedule_ExposureDelay << "ms before next capture";

    exposureDelayTimer.stop();
    exposureDelayTimer.disconnect();
    exposureDelayElapsed_ms = 0;

    exposureDelayTimer.setSingleShot(false);
    emit wsThread->sendMessageToClient(
        "ScheduleStepState:" +
        QString::number(schedule_currentNum) + ":" +
        "delay:" +
        "0:" +
        "0:" +
        "0");
    connect(&exposureDelayTimer, &QTimer::timeout, [this]() {
        if (StopSchedule)
        {
            exposureDelayTimer.stop();
            exposureDelayTimer.disconnect();
            Logger::Log(("Exposure delay interrupted: Schedule stopped during delay wait (elapsed: " + QString::number(exposureDelayElapsed_ms) + " ms)").toStdString(), LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Exposure delay interrupted: Schedule stopped during delay wait (elapsed:" << exposureDelayElapsed_ms << "ms)";
            exposureDelayElapsed_ms = 0;
            return;
        }

        if (!exposureDelayTimer.isActive())
        {
            qDebug() << "Exposure delay timer is not active, returning";
            return;
        }

        exposureDelayElapsed_ms += 100;

        if (StopSchedule)
        {
            exposureDelayTimer.stop();
            exposureDelayTimer.disconnect();
            Logger::Log(("Exposure delay interrupted: Schedule stopped during delay wait (elapsed: " + QString::number(exposureDelayElapsed_ms) + " ms)").toStdString(), LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Exposure delay interrupted: Schedule stopped during delay wait (elapsed:" << exposureDelayElapsed_ms << "ms)";
            exposureDelayElapsed_ms = 0;
            return;
        }

        if (schedule_ExposureDelay > 0)
        {
            int progress = static_cast<int>(
                qMin(100.0, exposureDelayElapsed_ms * 100.0 / static_cast<double>(schedule_ExposureDelay)));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "delay:" +
                "0:" +
                "0:" +
                QString::number(progress));
        }

        if (exposureDelayElapsed_ms >= schedule_ExposureDelay)
        {
            exposureDelayTimer.stop();
            exposureDelayTimer.disconnect();
            qDebug() << "Exposure delay complete, starting next capture";
            exposureDelayElapsed_ms = 0;
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "delay:" +
                "0:" +
                "0:" +
                "100");
            startCapture(schedule_ExpTime);
        }
    });

    exposureDelayTimer.start(100);
}

void MainWindow::startCapture(int ExpTime)
{
    qDebug() << "startCapture...";
    captureTimer.stop();
    captureTimer.disconnect();
    exposureDelayTimer.stop();
    exposureDelayTimer.disconnect();

    ShootStatus = "InProgress";
    qDebug() << "ShootStatus: " << ShootStatus;
    startMainCameraCapture(ExpTime);
    schedule_currentShootNum++;

    captureTimer.setSingleShot(true);
    expTime_ms = 0;

    connect(&captureTimer, &QTimer::timeout, [this]()
            {
        if (StopSchedule)
        {
            StopSchedule = false;
            abortMainCameraCapture();
            qDebug("Schedule is stop!");
            return;
        }

        if (WaitForShootToComplete())
        {
            captureTimer.stop();
            qDebug() << "Capture" << schedule_currentShootNum << "Complete!";
            ScheduleImageSave(m_scheduList[schedule_currentNum].shootTarget, schedule_currentShootNum);

            int currentStep = 3 + schedule_currentShootNum;
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(currentStep, 1.0);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "exposure:" +
                QString::number(schedule_currentShootNum) + ":" +
                QString::number(schedule_RepeatNum) + ":" +
                "100");

            if (schedule_RepeatNum > 0)
            {
                int loopProgress = static_cast<int>(
                    qMin(100.0, schedule_currentShootNum * 100.0 / static_cast<double>(schedule_RepeatNum)));
                emit wsThread->sendMessageToClient(
                    "ScheduleLoopState:" +
                    QString::number(schedule_currentNum) + ":" +
                    QString::number(schedule_currentShootNum) + ":" +
                    QString::number(schedule_RepeatNum) + ":" +
                    QString::number(loopProgress));
            }

            if (schedule_currentShootNum < schedule_RepeatNum)
            {
                if (schedule_ExposureDelay > 0)
                {
                    startExposureDelay();
                }
                else
                {
                    startCapture(schedule_ExpTime);
                }
            }
            else
            {
                schedule_currentShootNum = 0;
                m_scheduList[schedule_currentNum].progress = 100;
                emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
                qDebug() << "Capture Goto Complete...";
                nextSchedule();
            }

        }
        else
        {
            expTime_ms += 1000;

            int maxTimeout = schedule_ExpTime + 60000;
            if (expTime_ms > maxTimeout)
            {
                captureTimer.stop();
                abortMainCameraCapture();
                Logger::Log(QString("计划任务表拍摄超时: 当前拍摄时间 %1ms, 超过最大超时时间 %2ms (曝光时间 %3ms + 1分钟)").arg(expTime_ms).arg(maxTimeout).arg(schedule_ExpTime).toStdString(),
                           LogLevel::WARNING, DeviceType::MAIN);
                Logger::Log("Capture timeout! expTime_ms:" + std::to_string(expTime_ms) + ", maxTimeout:" + std::to_string(maxTimeout) + ", schedule_ExpTime:" + std::to_string(schedule_ExpTime), LogLevel::WARNING, DeviceType::MAIN);

                if (schedule_currentShootNum < schedule_RepeatNum)
                {
                    qDebug() << "Skip current capture, continue to next capture...";
                    if (schedule_ExposureDelay > 0)
                    {
                        startExposureDelay();
                    }
                    else
                    {
                        startCapture(schedule_ExpTime);
                    }
                }
                else
                {
                    schedule_currentShootNum = 0;
                    qDebug() << "All captures completed or timeout, move to next schedule...";
                    m_scheduList[schedule_currentNum].progress = 100;
                    emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
                    emit wsThread->sendMessageToClient(
                        "ScheduleLoopState:" +
                        QString::number(schedule_currentNum) + ":" +
                        QString::number(schedule_RepeatNum) + ":" +
                        QString::number(schedule_RepeatNum) + ":" +
                        "100");
                    nextSchedule();
                }
                return;
            }

            int currentStep = 3 + schedule_currentShootNum;
            double shotProgress = qMin(expTime_ms / (double)schedule_ExpTime, 1.0);
            Logger::Log("expTime_ms:" + std::to_string(expTime_ms) + ", schedule_ExpTime:" + std::to_string(schedule_ExpTime) + ", currentShootNum:" + std::to_string(schedule_currentShootNum) + ", RepeatNum:" + std::to_string(schedule_RepeatNum) + ", currentStep:" + std::to_string(currentStep) + ", shotProgress:" + std::to_string(shotProgress), LogLevel::INFO, DeviceType::MAIN);

            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "exposure:" +
                QString::number(schedule_currentShootNum) + ":" +
                QString::number(schedule_RepeatNum) + ":" +
                QString::number(static_cast<int>(shotProgress * 100.0)));
            captureTimer.start(1000);
        } });

    captureTimer.start(1000);
}

bool MainWindow::WaitForTelescopeToComplete()
{
    return (TelescopeControl_Status() != "Moving");
}

bool MainWindow::WaitForShootToComplete()
{
    Logger::Log("Wait For Shoot To Complete...", LogLevel::INFO, DeviceType::MAIN);
    return (ShootStatus != "InProgress");
}

bool MainWindow::WaitForGuidingToComplete()
{
    Logger::Log("Wait For Guiding To Complete..." + std::to_string(InGuiding), LogLevel::INFO, DeviceType::MAIN);
    return InGuiding;
}

bool MainWindow::WaitForTimeToComplete()
{
    Logger::Log("Wait For Time To Complete...", LogLevel::INFO, DeviceType::MAIN);
    QString TargetTime = m_scheduList[schedule_currentNum].shootTime;

    if (TargetTime.length() != 5 || TargetTime[2] != ':')
        return true;

    QTime currentTime = QTime::currentTime();
    QTime targetTime = QTime::fromString(TargetTime, "hh:mm");

    Logger::Log("currentTime:" + currentTime.toString("hh:mm").toStdString() + ", targetTime:" + targetTime.toString("hh:mm").toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if (targetTime > currentTime)
        return false;

    return true;
}

int MainWindow::calculateScheduleProgress(int stepNumber, double stepProgress)
{
    int totalSteps = 3 + schedule_RepeatNum;
    if (totalSteps <= 0)
    {
        return 100;
    }

    double progressPerStep = 100.0 / totalSteps;
    double currentProgress = stepNumber * progressPerStep * stepProgress;

    if (currentProgress > 100.0)
    {
        currentProgress = 100.0;
    }

    return static_cast<int>(currentProgress);
}

int MainWindow::ScheduleImageSave(QString name, int num)
{
    const QString sourcePath = latestMainCaptureFitsPath();

    if (sourcePath.isEmpty())
    {
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
        return 1;
    }

    name.replace(' ', '_');

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo);

    QString destinationDirectory = ImageSaveBaseDirectory + "/ScheduleImage";
    QString dirPath = destinationDirectory + "/" + QString(buffer) + " " + QTime::currentTime().toString("hh") + "h (" + ScheduleTargetNames + ")";
    bool isUSBSave = (saveMode != "local");

    QString dirPathToCreate = isUSBSave ? dirPath : QString();
    int checkResult = checkStorageSpaceAndCreateDirectory(
        sourcePath,
        destinationDirectory,
        dirPathToCreate,
        "ScheduleImageSave",
        isUSBSave,
        [this]() { createScheduleDirectory(); }
    );
    if (checkResult != 0)
    {
        return checkResult;
    }

    int actualNum = num;
    QString resultFileName;
    QString destinationPath;

    int maxAttempts = 1000;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        resultFileName = QString("%1-%2.fits").arg(name).arg(actualNum);
        destinationPath = dirPath + "/" + resultFileName;

        if (!QFile::exists(destinationPath))
        {
            break;
        }

        actualNum++;

        if (attempt == 0)
        {
            Logger::Log("ScheduleImageSave | File already exists, incrementing sequence number: " +
                       (dirPath + "/" + QString("%1-%2.fits").arg(name).arg(num)).toStdString() +
                       " -> trying next number",
                       LogLevel::INFO, DeviceType::MAIN);
        }
    }

    if (actualNum != num)
    {
        Logger::Log("ScheduleImageSave | Using sequence number " + QString::number(actualNum).toStdString() +
                   " instead of " + QString::number(num).toStdString() + " to avoid overwriting",
                   LogLevel::INFO, DeviceType::MAIN);
    }

    int saveResult = saveImageFile(sourcePath, destinationPath, "ScheduleImageSave", isUSBSave);
    if (saveResult != 0)
    {
        return saveResult;
    }

    Logger::Log("ScheduleImageSave | File saved successfully: " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Success");
    return 0;
}

bool MainWindow::createScheduleDirectory()
{
    Logger::Log("createScheduleDirectory start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/ScheduleImage";

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo);
    std::string folderName = basePath + "/" + buffer + " " + QTime::currentTime().toString("hh").toStdString() + "h (" + ScheduleTargetNames.toStdString() + ")";

    if (!std::filesystem::exists(folderName))
    {
        if (std::filesystem::create_directories(folderName))
        {
            Logger::Log("createScheduleDirectory | Folder created successfully: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("createScheduleDirectory | An error occurred while creating the folder.", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
    }
    else
    {
        Logger::Log("createScheduleDirectory | The folder already exists: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
    }
    return true;
}
