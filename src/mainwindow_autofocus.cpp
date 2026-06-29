#include "mainwindow_command_support.h"

void MainWindow::startAutoFocus()
{
    // 检查电调是否连接（支持SDK和INDI模式）
    const bool useSdkMainCamera = isMainCameraSDK();   // 单设备：主相机是否走 SDK
    const bool useSdkFocuser    = isFocuserSDK();      // 单设备：电调是否走 SDK
    bool focuserConnected = (dpFocuser != nullptr) || useSdkFocuser;

    if (!focuserConnected || !isMainCameraConnected())
    {
        Logger::Log("AutoFocus | 调焦器或相机未连接", LogLevel::WARNING, DeviceType::FOCUSER);
        isAutoFocus = false;
        emit wsThread->sendMessageToClient("AutoFocusOver:false");
        return;
    }

    // 注意：SDK 是“单设备连接”——主相机/电调可分别走 SDK/INDI。
    // AutoFocus 内部已按 useSdkMainCamera/useSdkFocuser 分流：
    // - 主相机 SDK：通过 requestCapture/requestAbortCapture 走 MainWindow::startMainCameraCapture/abortMainCameraCapture
    // - 电调 SDK：通过 SdkManager::callByHandle( sdkFocuserHandle ) 执行移动/读位置/Abort
    // 预处理：统一清理自动对焦相关定时器与信号连接，避免残留或重复
    cleanupAutoFocusConnections();

    if (autoFocus == nullptr)
    {
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }
    else
    {
        // 停止旧对象并清理信号连接
        autoFocus->stopAutoFocus();
        cleanupAutoFocusConnections();
        autoFocus->deleteLater();
        autoFocus = nullptr;
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }

    // SDK/INDI 统一拍摄入口：由 AutoFocus 发起 requestCapture/requestAbortCapture，MainWindow 调用统一入口执行
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestCapture,
                                           this, [this](int exposureMs) { this->startMainCameraCapture(exposureMs); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestAbortCapture,
                                           this, [this]() { this->abortMainCameraCapture(); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::coarseRetryPromptRequested,
                                           this, [this](int totalDivisions, const QString &message)
                                           {
        Logger::Log(QString("请求前端弹出粗调补扫确认框：totalDivisions=%1, message=%2")
                        .arg(totalDivisions).arg(message).toStdString(),
                    LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(
            QString("AutoFocusCoarseRetryPrompt:%1:%2").arg(totalDivisions).arg(message));
    }));
    autoFocus->setFocuserMinPosition(focuserMinPosition);
    autoFocus->setFocuserMaxPosition(focuserMaxPosition);
    autoFocus->setCoarseDivisionCount(autoFocusCoarseDivisions);
    autoFocus->setScheduleTriggered(isScheduleTriggeredAutoFocus);
    autoFocus->setDefaultExposureTime(autoFocusExposureTime); // 自动对焦曝光时间（仅作用于自动对焦）
    autoFocus->setUseVirtualData(false);      // 使用虚拟数据

    // 设置空程补偿
    if (autofocusBacklashCompensation > 0) {
        autoFocus->setBacklashCompensation(autofocusBacklashCompensation, autofocusBacklashCompensation);
        autoFocus->setUseBacklashCompensation(true);
        Logger::Log(QString("设置自动对焦空程补偿: %1步").arg(autofocusBacklashCompensation).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
    } else {
        autoFocus->setUseBacklashCompensation(false);
        Logger::Log("自动对焦不使用空程补偿", LogLevel::INFO, DeviceType::FOCUSER);
    }
   for (int i = 1; i <= 11; i++) {
    std::string filename = "/home/quarcs/test_fits/coarse/" + std::to_string(i) + ".fits";
    autoFocus->setCaptureComplete(filename.c_str());
    }
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::roiInfoChanged, this, [this](const QRect &roi)
            {
        if (roi.width() == 0 && roi.height() == 0){
            roiAndFocuserInfo["ROI_x"] = 0;
            roiAndFocuserInfo["ROI_y"] = 0;
            roiAndFocuserInfo["BoxSideLength"] = 300;
            autoFocuserIsROI = false;
        }else{
            roiAndFocuserInfo["ROI_x"] = roi.x();
            roiAndFocuserInfo["ROI_y"] = roi.y();
            roiAndFocuserInfo["BoxSideLength"] = roi.width();
            autoFocuserIsROI = true;
        } }));

    // 连接二次拟合结果信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusFitUpdated, this, [this](double a, double b, double c, double bestPosition, double minFWHM)
            {
        Logger::Log(QString("接收到focusFitUpdated信号: a=%1, b=%2, c=%3, bestPosition=%4, minFWHM=%5")
                   .arg(a).arg(b).arg(c).arg(bestPosition).arg(minFWHM).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        // 发送二次曲线数据到前端
        QString curveData = QString("fitQuadraticCurve:%1:%2:%3:%4:%5")
                           .arg(a, 0, 'g', 15)  // 使用科学计数法，保留15位有效数字，确保小系数不被截断
                           .arg(b, 0, 'g', 15)
                           .arg(c, 0, 'g', 15)
                           .arg(bestPosition, 0, 'f', 2)
                           .arg(minFWHM, 0, 'f', 3);

        Logger::Log(QString("发送二次曲线数据: %1").arg(curveData).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(curveData);

        // 发送最佳位置点数据
        QString minPointData = QString("fitQuadraticCurve_minPoint:%1:%2")
                              .arg(bestPosition, 0, 'f', 2)
                              .arg(minFWHM, 0, 'f', 3);

        Logger::Log(QString("发送最小点数据: %1").arg(minPointData).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(minPointData);

        Logger::Log(QString("二次拟合结果发送完成").toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
    }));

    // 连接数据点信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusDataPointReady, this, [this](int position, double fwhm, const QString &stage)
            {
        Logger::Log(QString("接收到数据点: position=%1, fwhm=%2, stage=%3")
                   .arg(position).arg(fwhm).arg(stage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        // 发送数据点到前端
        QString dataPointMessage = QString("FocusMoveDone:%1:%2")
                                 .arg(position).arg(fwhm);

        Logger::Log(QString("发送数据点: %1").arg(dataPointMessage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(dataPointMessage);
    }));

    // 连接自动对焦失败信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autofocusFailed, this, [this]()
            {
        Logger::Log("自动对焦失败，发送提示消息到前端", LogLevel::ERROR, DeviceType::FOCUSER);

        // 检查是否是由计划任务表触发的自动对焦
        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos; // 保存当前任务的滤镜位置

        isAutoFocus = false;
        // 如果是计划触发的自动对焦，向前端报告步骤完成（失败）
        if (wasScheduleTriggered)
        {
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "autofocus:" +
                "0:" +
                "0:" +
                "100");
        }
        isScheduleTriggeredAutoFocus = false; // 重置标志

        emit wsThread->sendMessageToClient("FitResult:Failed:拟合结果为水平线，未找到最佳焦点");

        // 如果是由计划任务表触发的自动对焦失败，仍然继续执行拍摄任务
        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦失败，但仍继续执行拍摄任务", LogLevel::WARNING, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus failed, but continuing with CFW setup...";

            // 检查是否已停止计划任务表
            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }

            // 继续执行设置滤镜和拍摄
            startSetCFW(savedCFWpos);
        }
    }));

    // 连接星点识别结果信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::starDetectionResult, this, [this](bool detected, double fwhm)
            {
        if (detected) {
            Logger::Log(QString("识别到星点，FWHM为: %1").arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient(QString("StarDetectionResult:true:%1").arg(fwhm));
        } else {
            Logger::Log("未识别到星点", LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("StarDetectionResult:false:0");
        }
    }));

    // 连接自动对焦模式变化信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusModeChanged, this, [this](const QString &mode, double fwhm)
            {
        Logger::Log(QString("自动对焦模式变化: %1, FWHM: %2").arg(mode).arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusModeChanged:%1:%2").arg(mode).arg(fwhm));
    }));

    // 连接自动对焦步骤变化信号 - [AUTO_FOCUS_UI_ENHANCEMENT]
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusStepChanged, this, [this](int step, const QString &description)
            {
        Logger::Log(QString("自动对焦步骤变化: 步骤%1 - %2").arg(step).arg(description).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusStepChanged:%1:%2").arg(step).arg(description));
    }));

  // 连接自动对焦拍摄进度信号：将各阶段拍摄进度转发到前端
  autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::captureProgressChanged,
                                         this, [this](const QString &stage, int current, int total)
                                         {
    Logger::Log(QString("自动对焦拍摄进度: 阶段=%1, 当前=%2, 总数=%3")
                .arg(stage).arg(current).arg(total).toStdString(),
                LogLevel::INFO, DeviceType::FOCUSER);
    emit wsThread->sendMessageToClient(
          QString("AutoFocusCaptureProgress:%1:%2:%3").arg(stage).arg(current).arg(total));
  }));

    // 连接自动对焦完成信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusCompleted, this, [this](bool success, double bestPosition, double minHFR)
            {
        Logger::Log(QString("自动对焦完成: success=%1, bestPosition=%2, minHFR=%3")
                   .arg(success).arg(bestPosition).arg(minHFR).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        // 检查是否是由计划任务表触发的自动对焦
        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos; // 保存当前任务的滤镜位置

        // 结束阶段：统一清理自动对焦相关定时器与信号连接
        cleanupAutoFocusConnections();

        // 如果是计划触发的自动对焦，向前端报告步骤完成
        if (wasScheduleTriggered)
        {
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "autofocus:" +
                "0:" +
                "0:" +
                "100");
        }

        // 重置计划任务表触发标志
        isScheduleTriggeredAutoFocus = false;

        // 发送自动对焦完成消息到前端
        QString completeMessage = QString("AutoFocusOver:%1:%2:%3")
                                .arg(success ? "true" : "false")
                                .arg(bestPosition, 0, 'f', 2)
                                .arg(minHFR, 0, 'f', 3);

        Logger::Log(QString("发送自动对焦完成消息: %1").arg(completeMessage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(completeMessage);
        isAutoFocus = false;

        // 发送自动对焦结束事件到前端 - [AUTO_FOCUS_UI_ENHANCEMENT]
        emit wsThread->sendMessageToClient("AutoFocusEnded:自动对焦已结束");

        // 如果是由计划任务表触发的自动对焦，完成后继续执行拍摄任务
        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦已完成，继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus completed, continuing with CFW setup...";

            // 检查是否已停止计划任务表
            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }

            // 继续执行设置滤镜和拍摄
            startSetCFW(savedCFWpos);
        }
    }));

    autoFocus->startAutoFocus();
    isAutoFocus = true;
    autoFocusStep = 0;
}

void MainWindow::startAutoFocusFineHFROnly()
{
    // 检查电调/相机连接（支持SDK和INDI模式）
    const bool useSdkMainCamera = isMainCameraSDK();
    const bool useSdkFocuser    = isFocuserSDK();
    bool focuserConnected = (dpFocuser != nullptr) || useSdkFocuser;
    if (!focuserConnected || !isMainCameraConnected())
    {
        Logger::Log("AutoFocus (fine-HFR only) | 调焦器或相机未连接", LogLevel::WARNING, DeviceType::FOCUSER);
        isAutoFocus = false;
        emit wsThread->sendMessageToClient("AutoFocusOver:false");
        return;
    }

    // 预处理：统一清理自动对焦相关定时器与信号连接，避免残留或重复
    cleanupAutoFocusConnections();

    if (autoFocus == nullptr)
    {
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }
    else
    {
        // 停止旧对象并清理信号连接
        autoFocus->stopAutoFocus();
        cleanupAutoFocusConnections();
        autoFocus->deleteLater();
        autoFocus = nullptr;
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }

    // SDK/INDI 统一拍摄入口（同 startAutoFocus）
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestCapture,
                                           this, [this](int exposureMs) { this->startMainCameraCapture(exposureMs); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestAbortCapture,
                                           this, [this]() { this->abortMainCameraCapture(); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::coarseRetryPromptRequested,
                                           this, [this](int totalDivisions, const QString &message)
                                           {
        Logger::Log(QString("请求前端弹出粗调补扫确认框：totalDivisions=%1, message=%2")
                        .arg(totalDivisions).arg(message).toStdString(),
                    LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(
            QString("AutoFocusCoarseRetryPrompt:%1:%2").arg(totalDivisions).arg(message));
    }));

    // 与常规自动对焦保持一致的参数配置
    autoFocus->setFocuserMinPosition(focuserMinPosition);
    autoFocus->setFocuserMaxPosition(focuserMaxPosition);
    autoFocus->setCoarseDivisionCount(autoFocusCoarseDivisions);
    autoFocus->setScheduleTriggered(isScheduleTriggeredAutoFocus);
    autoFocus->setDefaultExposureTime(autoFocusExposureTime); // 自动对焦曝光时间（仅作用于自动对焦）
    autoFocus->setUseVirtualData(false);      // 使用实时数据

    // 设置空程补偿
    if (autofocusBacklashCompensation > 0) {
        autoFocus->setBacklashCompensation(autofocusBacklashCompensation, autofocusBacklashCompensation);
        autoFocus->setUseBacklashCompensation(true);
        Logger::Log(QString("设置自动对焦空程补偿: %1步").arg(autofocusBacklashCompensation).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
    } else {
        autoFocus->setUseBacklashCompensation(false);
        Logger::Log("自动对焦不使用空程补偿", LogLevel::INFO, DeviceType::FOCUSER);
    }

    // 复用与 startAutoFocus / startAutoFocusSuperFineOnly 相同的信号连接，确保前端表现一致
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::roiInfoChanged, this, [this](const QRect &roi)
            {
        if (roi.width() == 0 && roi.height() == 0){
            roiAndFocuserInfo["ROI_x"] = 0;
            roiAndFocuserInfo["ROI_y"] = 0;
            roiAndFocuserInfo["BoxSideLength"] = 300;
            autoFocuserIsROI = false;
        }else{
            roiAndFocuserInfo["ROI_x"] = roi.x();
            roiAndFocuserInfo["ROI_y"] = roi.y();
            roiAndFocuserInfo["BoxSideLength"] = roi.width();
            autoFocuserIsROI = true;
        } }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusFitUpdated, this, [this](double a, double b, double c, double bestPosition, double minFWHM)
            {
        Logger::Log(QString("接收到focusFitUpdated信号: a=%1, b=%2, c=%3, bestPosition=%4, minFWHM=%5")
                   .arg(a).arg(b).arg(c).arg(bestPosition).arg(minFWHM).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        QString curveData = QString("fitQuadraticCurve:%1:%2:%3:%4:%5")
                           .arg(a, 0, 'g', 15)
                           .arg(b, 0, 'g', 15)
                           .arg(c, 0, 'g', 15)
                           .arg(bestPosition, 0, 'f', 2)
                           .arg(minFWHM, 0, 'f', 3);

        Logger::Log(QString("发送二次曲线数据: %1").arg(curveData).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(curveData);

        QString minPointData = QString("fitQuadraticCurve_minPoint:%1:%2")
                              .arg(bestPosition, 0, 'f', 2)
                              .arg(minFWHM, 0, 'f', 3);

        Logger::Log(QString("发送最小点数据: %1").arg(minPointData).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(minPointData);

        Logger::Log(QString("二次拟合结果发送完成").toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusDataPointReady, this, [this](int position, double fwhm, const QString &stage)
            {
        Logger::Log(QString("接收到数据点: position=%1, fwhm=%2, stage=%3")
                   .arg(position).arg(fwhm).arg(stage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        QString dataPointMessage = QString("FocusMoveDone:%1:%2")
                                 .arg(position).arg(fwhm);

        Logger::Log(QString("发送数据点: %1").arg(dataPointMessage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(dataPointMessage);
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autofocusFailed, this, [this]()
            {
        Logger::Log("自动对焦失败，发送提示消息到前端", LogLevel::ERROR, DeviceType::FOCUSER);

        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos;

        isAutoFocus = false;
        isScheduleTriggeredAutoFocus = false;

        emit wsThread->sendMessageToClient("FitResult:Failed:拟合结果为水平线，未找到最佳焦点");

        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦失败，但仍继续执行拍摄任务", LogLevel::WARNING, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus failed, but continuing with CFW setup...";

            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }

            startSetCFW(savedCFWpos);
        }
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::starDetectionResult, this, [this](bool detected, double fwhm)
            {
        if (detected) {
            Logger::Log(QString("识别到星点，FWHM为: %1").arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient(QString("StarDetectionResult:true:%1").arg(fwhm));
        } else {
            Logger::Log("未识别到星点", LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("StarDetectionResult:false:0");
        }
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusModeChanged, this, [this](const QString &mode, double fwhm)
            {
        Logger::Log(QString("自动对焦模式变化: %1, FWHM: %2").arg(mode).arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusModeChanged:%1:%2").arg(mode).arg(fwhm));
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusStepChanged, this, [this](int step, const QString &description)
            {
        Logger::Log(QString("自动对焦步骤变化: 步骤%1 - %2").arg(step).arg(description).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusStepChanged:%1:%2").arg(step).arg(description));
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusCompleted, this, [this](bool success, double bestPosition, double minHFR)
            {
        Logger::Log(QString("自动对焦完成: success=%1, bestPosition=%2, minHFR=%3")
                   .arg(success).arg(bestPosition).arg(minHFR).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos;

        cleanupAutoFocusConnections();

        isScheduleTriggeredAutoFocus = false;

        QString completeMessage = QString("AutoFocusOver:%1:%2:%3")
                                .arg(success ? "true" : "false")
                                .arg(bestPosition, 0, 'f', 2)
                                .arg(minHFR, 0, 'f', 3);

        Logger::Log(QString("发送自动对焦完成消息: %1").arg(completeMessage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(completeMessage);
        isAutoFocus = false;

        emit wsThread->sendMessageToClient("AutoFocusEnded:自动对焦已结束");

        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦已完成，继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus completed, continuing with CFW setup...";

            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }

            startSetCFW(savedCFWpos);
        }
    }));

    // 仅精调(Fine)：从当前位置开始，先拍当前位置，再围绕当前位置双向展开
    autoFocus->startLocalFineAdjustmentFromCurrentPosition();
    isAutoFocus = true;
    autoFocusStep = 0;
}

void MainWindow::startAutoFocusSuperFineOnly()
{
    const bool useSdkMainCamera = isMainCameraSDK();
    const bool useSdkFocuser    = isFocuserSDK();
    const bool focuserConnected = (dpFocuser != nullptr) || useSdkFocuser;
    if (!focuserConnected || !isMainCameraConnected())
    {
        Logger::Log("AutoFocus (super-fine only) | 调焦器或相机未连接", LogLevel::WARNING, DeviceType::FOCUSER);
        isAutoFocus = false;
        emit wsThread->sendMessageToClient("AutoFocusOver:false");
        return;
    }

    // 预处理：统一清理自动对焦相关定时器与信号连接，避免残留或重复
    cleanupAutoFocusConnections();

    if (autoFocus == nullptr)
    {
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }
    else
    {
        // 停止旧对象并清理信号连接
        autoFocus->stopAutoFocus();
        cleanupAutoFocusConnections();
        autoFocus->deleteLater();
        autoFocus = nullptr;
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }

    // SDK/INDI 统一拍摄入口（同 startAutoFocus）
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestCapture,
                                           this, [this](int exposureMs) { this->startMainCameraCapture(exposureMs); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestAbortCapture,
                                           this, [this]() { this->abortMainCameraCapture(); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::coarseRetryPromptRequested,
                                           this, [this](int totalDivisions, const QString &message)
                                           {
        Logger::Log(QString("请求前端弹出粗调补扫确认框：totalDivisions=%1, message=%2")
                        .arg(totalDivisions).arg(message).toStdString(),
                    LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(
            QString("AutoFocusCoarseRetryPrompt:%1:%2").arg(totalDivisions).arg(message));
    }));

    // 与常规自动对焦保持一致的参数配置
    autoFocus->setFocuserMinPosition(focuserMinPosition);
    autoFocus->setFocuserMaxPosition(focuserMaxPosition);
    autoFocus->setCoarseDivisionCount(autoFocusCoarseDivisions);
    autoFocus->setScheduleTriggered(isScheduleTriggeredAutoFocus);
    autoFocus->setDefaultExposureTime(autoFocusExposureTime); // 自动对焦曝光时间（仅作用于自动对焦）
    autoFocus->setUseVirtualData(false);      // 使用虚拟数据

    // 设置空程补偿
    if (autofocusBacklashCompensation > 0) {
        autoFocus->setBacklashCompensation(autofocusBacklashCompensation, autofocusBacklashCompensation);
        autoFocus->setUseBacklashCompensation(true);
        Logger::Log(QString("设置自动对焦空程补偿: %1步").arg(autofocusBacklashCompensation).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
    } else {
        autoFocus->setUseBacklashCompensation(false);
        Logger::Log("自动对焦不使用空程补偿", LogLevel::INFO, DeviceType::FOCUSER);
    }

    // 复用与 startAutoFocus 相同的信号连接
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::roiInfoChanged, this, [this](const QRect &roi)
            {
        if (roi.width() == 0 && roi.height() == 0){
            roiAndFocuserInfo["ROI_x"] = 0;
            roiAndFocuserInfo["ROI_y"] = 0;
            roiAndFocuserInfo["BoxSideLength"] = 300;
            autoFocuserIsROI = false;
        }else{
            roiAndFocuserInfo["ROI_x"] = roi.x();
            roiAndFocuserInfo["ROI_y"] = roi.y();
            roiAndFocuserInfo["BoxSideLength"] = roi.width();
            autoFocuserIsROI = true;
        } }));

    // 连接二次拟合结果信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusFitUpdated, this, [this](double a, double b, double c, double bestPosition, double minFWHM)
            {
        Logger::Log(QString("接收到focusFitUpdated信号: a=%1, b=%2, c=%3, bestPosition=%4, minFWHM=%5")
                   .arg(a).arg(b).arg(c).arg(bestPosition).arg(minFWHM).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        // 发送二次曲线数据到前端
        QString curveData = QString("fitQuadraticCurve:%1:%2:%3:%4:%5")
                           .arg(a, 0, 'g', 15)  // 使用科学计数法，保留15位有效数字，确保小系数不被截断
                           .arg(b, 0, 'g', 15)
                           .arg(c, 0, 'g', 15)
                           .arg(bestPosition, 0, 'f', 2)
                           .arg(minFWHM, 0, 'f', 3);

        Logger::Log(QString("发送二次曲线数据: %1").arg(curveData).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(curveData);

        // 发送最佳位置点数据
        QString minPointData = QString("fitQuadraticCurve_minPoint:%1:%2")
                              .arg(bestPosition, 0, 'f', 2)
                              .arg(minFWHM, 0, 'f', 3);

        Logger::Log(QString("发送最小点数据: %1").arg(minPointData).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(minPointData);

        Logger::Log(QString("二次拟合结果发送完成").toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
    }));

    // 连接数据点信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusDataPointReady, this, [this](int position, double fwhm, const QString &stage)
            {
        Logger::Log(QString("接收到数据点: position=%1, fwhm=%2, stage=%3")
                   .arg(position).arg(fwhm).arg(stage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        // 发送数据点到前端
        QString dataPointMessage = QString("FocusMoveDone:%1:%2")
                                 .arg(position).arg(fwhm);

        Logger::Log(QString("发送数据点: %1").arg(dataPointMessage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(dataPointMessage);
    }));

    // 连接自动对焦失败信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autofocusFailed, this, [this]()
            {
        Logger::Log("自动对焦失败，发送提示消息到前端", LogLevel::ERROR, DeviceType::FOCUSER);

        // 检查是否是由计划任务表触发的自动对焦
        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos; // 保存当前任务的滤镜位置

        isAutoFocus = false;
        isScheduleTriggeredAutoFocus = false; // 重置标志

        emit wsThread->sendMessageToClient("FitResult:Failed:拟合结果为水平线，未找到最佳焦点");

        // 如果是由计划任务表触发的自动对焦失败，仍然继续执行拍摄任务
        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦失败，但仍继续执行拍摄任务", LogLevel::WARNING, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus failed, but continuing with CFW setup...";

            // 检查是否已停止计划任务表
            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }

            // 继续执行设置滤镜和拍摄
            startSetCFW(savedCFWpos);
        }
    }));

    // 连接星点识别结果信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::starDetectionResult, this, [this](bool detected, double fwhm)
            {
        if (detected) {
            Logger::Log(QString("识别到星点，FWHM为: %1").arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient(QString("StarDetectionResult:true:%1").arg(fwhm));
        } else {
            Logger::Log("未识别到星点", LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("StarDetectionResult:false:0");
        }
    }));

    // 连接自动对焦模式变化信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusModeChanged, this, [this](const QString &mode, double fwhm)
            {
        Logger::Log(QString("自动对焦模式变化: %1, FWHM: %2").arg(mode).arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusModeChanged:%1:%2").arg(mode).arg(fwhm));
    }));

    // 连接自动对焦步骤变化信号 - [AUTO_FOCUS_UI_ENHANCEMENT]
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusStepChanged, this, [this](int step, const QString &description)
            {
        Logger::Log(QString("自动对焦步骤变化: 步骤%1 - %2").arg(step).arg(description).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusStepChanged:%1:%2").arg(step).arg(description));
    }));

    // 连接自动对焦完成信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusCompleted, this, [this](bool success, double bestPosition, double minHFR)
            {
        Logger::Log(QString("自动对焦完成: success=%1, bestPosition=%2, minHFR=%3")
                   .arg(success).arg(bestPosition).arg(minHFR).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        // 检查是否是由计划任务表触发的自动对焦
        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos; // 保存当前任务的滤镜位置

        // 结束阶段：统一清理自动对焦相关定时器与信号连接
        cleanupAutoFocusConnections();

        // 重置计划任务表触发标志
        isScheduleTriggeredAutoFocus = false;

        // 发送自动对焦完成消息到前端
        QString completeMessage = QString("AutoFocusOver:%1:%2:%3")
                                .arg(success ? "true" : "false")
                                .arg(bestPosition, 0, 'f', 2)
                                .arg(minHFR, 0, 'f', 3);

        Logger::Log(QString("发送自动对焦完成消息: %1").arg(completeMessage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(completeMessage);
        isAutoFocus = false;

        // 发送自动对焦结束事件到前端 - [AUTO_FOCUS_UI_ENHANCEMENT]
        emit wsThread->sendMessageToClient("AutoFocusEnded:自动对焦已结束");

        // 如果是由计划任务表触发的自动对焦，完成后继续执行拍摄任务
        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦已完成，继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus completed, continuing with CFW setup...";

            // 检查是否已停止计划任务表
            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }

            // 继续执行设置滤镜和拍摄
            startSetCFW(savedCFWpos);
        }
    }));

    autoFocus->startSuperFineFromCurrentPosition();
    isAutoFocus = true;
    autoFocusStep = 0;
}

void MainWindow::startScheduleAutoFocus()
{
    // 检查设备是否连接
    if (dpFocuser == NULL || !isMainCameraConnected())
    {
        Logger::Log("计划任务表自动对焦 | 调焦器或相机未连接，跳过自动对焦", LogLevel::WARNING, DeviceType::FOCUSER);
        isScheduleTriggeredAutoFocus = false;
        // 如果自动对焦失败，直接继续执行拍摄任务
        startSetCFW(schedule_CFWpos);
        return;
    }

    // 如果已经有自动对焦在运行，先停止
    if (isAutoFocus && autoFocus != nullptr)
    {
        Logger::Log("计划任务表自动对焦 | 检测到已有自动对焦在运行，先停止", LogLevel::INFO, DeviceType::FOCUSER);
        autoFocus->stopAutoFocus();
        cleanupAutoFocusConnections();
        autoFocus->deleteLater();
        autoFocus = nullptr;
        isAutoFocus = false;
    }

    // 标记这是由计划任务表触发的自动对焦
    isScheduleTriggeredAutoFocus = true;
    // 向前端发送步骤状态：当前行进入自动对焦阶段
    emit wsThread->sendMessageToClient(
        "ScheduleStepState:" +
        QString::number(schedule_currentNum) + ":" +
        "autofocus:" +
        "0:" +
        "0:" +
        "0");

    // 调用通用的自动对焦启动函数
    Logger::Log("计划任务表自动对焦 | Refocus=ON：开始执行自动对焦最后一步精调(super-fine)", LogLevel::INFO, DeviceType::MAIN);
    // 仅触发自动对焦的最后一步精调：从当前位置进入 super-fine 精调（跳过粗调/精调）
    startAutoFocusSuperFineOnly();
}

void MainWindow::cleanupAutoFocusConnections()
{
    if (autoFocus) {
        // 逐个断开并清空记录的连接，覆盖所有已登记连接
        for (const QMetaObject::Connection &c : autoFocusConnections) {
            QObject::disconnect(c);
        }
        autoFocusConnections.clear();
        // 保险起见再断开双方所有连接
        disconnect(autoFocus, nullptr, this, nullptr);
        disconnect(this, nullptr, autoFocus, nullptr);
    }
}
