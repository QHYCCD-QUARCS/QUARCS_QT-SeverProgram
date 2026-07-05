#include "mainwindow_command_support.h"

namespace {

bool isValidObservatoryLocation(double latitude, double longitude)
{
    return std::isfinite(latitude) &&
           std::isfinite(longitude) &&
           std::abs(latitude) <= 90.0 &&
           std::abs(longitude) <= 180.0 &&
           !(latitude == 0.0 && longitude == 0.0) &&
           latitude != -1.0 &&
           longitude != -1.0;
}

} // namespace

void MainWindow::TelescopeControl_Goto(double Ra, double Dec)
{
    if (dpMount != NULL)
    {
        if (indi_Client->mountState.isTracking)
        {
            indi_Client->slewTelescopeJNowNonBlock(dpMount, Ra, Dec, true);
        }
        else
        {
            indi_Client->slewTelescopeJNowNonBlock(dpMount, Ra, Dec, false);
        }
    }
}

QString MainWindow::TelescopeControl_Status()
{
    if (dpMount != NULL)
    {
        QString Stat;
        indi_Client->getTelescopeStatus(dpMount, Stat);
        return Stat;
    }

    return QString();
}

bool MainWindow::TelescopeControl_Park()
{
    bool isPark = false;
    if (dpMount != NULL)
    {
        indi_Client->getTelescopePark(dpMount, isPark);
        if (isPark == false)
        {
            indi_Client->setTelescopePark(dpMount, true);
        }
        else
        {
            indi_Client->setTelescopePark(dpMount, false);
        }
        indi_Client->getTelescopePark(dpMount, isPark);
        // Logger::Log("TelescopeControl_Park | Telescope is Park ???:" + std::to_string(isPark), LogLevel::INFO, DeviceType::MAIN);
    }

    return isPark;
}

bool MainWindow::TelescopeControl_Track()
{
    bool isTrack = true;
    if (dpMount != NULL)
    {
        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
        if (isTrack == false)
        {
            indi_Client->setTelescopeTrackEnable(dpMount, true);
        }
        else
        {
            indi_Client->setTelescopeTrackEnable(dpMount, false);
        }
        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
        Logger::Log("TelescopeControl_Track | Telescope is Track ???:" + std::to_string(isTrack), LogLevel::INFO, DeviceType::MAIN);
    }
    return isTrack;
}

void MainWindow::solveCurrentPosition()
{
    if (solveCurrentPositionTimer.isActive())
    {
        Logger::Log("solveCurrentPosition | SolveCurrentPosition is already running...", LogLevel::INFO, DeviceType::MAIN);
        return;
    }
    // 停止之前的定时器
    solveCurrentPositionTimer.stop();
    disconnect(&solveCurrentPositionTimer, &QTimer::timeout, nullptr, nullptr);
    // 判断解析图像路径下是否有图片
    if (isFileExists(QString::fromStdString(SolveImageFileName.toStdString())))
    {
        // 设置定时器为单次触发
        solveCurrentPositionTimer.setSingleShot(true);
        // 开始解析图像
        Tools::PlateSolve(SolveImageFileName, glFocalLength, glCameraSize_width, glCameraSize_height, false);
        // 连接解析完成信号到处理函数，处理解析完成后的逻辑
        connect(&solveCurrentPositionTimer, &QTimer::timeout, [this]()
        {
            if (Tools::isSolveImageFinish())  // 检查图像解析是否完成
            {
                SloveResults result = Tools::ReadSolveResult(SolveImageFileName, glMainCCDSizeX, glMainCCDSizeY);  // 读取解析结果
                if (result.RA_Degree == -1 && result.DEC_Degree == -1)
                {
                    Logger::Log("solveCurrentPosition | Solve image failed...", LogLevel::INFO, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("SolveCurrentPosition:failed");
                    solveCurrentPositionTimer.stop();
                    disconnect(&solveCurrentPositionTimer, &QTimer::timeout, nullptr, nullptr);
                    return;
                }
                else
                {
                    emit wsThread->sendMessageToClient("SolveCurrentPosition:succeeded:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree)+":"+QString::number(result.RA_0)+":"+QString::number(result.DEC_0)+":"+QString::number(result.RA_1)+":"+QString::number(result.DEC_1)+":"+QString::number(result.RA_2)+":"+QString::number(result.DEC_2)+":"+QString::number(result.RA_3)+":"+QString::number(result.DEC_3));
                    solveCurrentPositionTimer.stop();
                    disconnect(&solveCurrentPositionTimer, &QTimer::timeout, nullptr, nullptr);
                    return;
                }
            }
            else
            {
                solveCurrentPositionTimer.start(1000);
            }
        });
        solveCurrentPositionTimer.start(1000);
    }
    else
    {
        Logger::Log("solveCurrentPosition | SolveImageFileName: " + SolveImageFileName.toStdString() + " does not exist.", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("SolveCurrentPosition:failed");
        solveCurrentPositionTimer.stop();
        return;
    }
}

void MainWindow::TelescopeControl_SolveSYNC()
{
    // 在函数开始时断开之前的连接
    disconnect(&captureTimer, &QTimer::timeout, nullptr, nullptr);
    disconnect(&solveTimer, &QTimer::timeout, nullptr, nullptr);

    // 停止之前的定时器
    captureTimer.stop();
    solveTimer.stop();

    if (!isMainCameraConnected())
    {
        emit wsThread->sendMessageToClient("MainCameraNotConnect");
        return;
    }
    Logger::Log("TelescopeControl_SolveSYNC start ...", LogLevel::INFO, DeviceType::MAIN);
    if (glMainCameraStatu == "Exposuring" || isFocusLoopShooting == true)
    {
        Logger::Log("TelescopeControl_SolveSYNC | Camera is not idle.glMainCameraStatu:" + glMainCameraStatu.toStdString() + ", isFocusLoopShooting:" + std::to_string(isFocusLoopShooting), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CameraNotIdle");
        return;
    }

    double Ra_Hour;
    double Dec_Degree;

    if (dpMount != NULL)
    {
        indi_Client->getTelescopeRADECJNOW(dpMount, Ra_Hour, Dec_Degree); // 获取当前望远镜的赤经和赤纬
    }
    else
    {
        Logger::Log("TelescopeControl_SolveSYNC | No Mount Connect.", LogLevel::INFO, DeviceType::MAIN);
        return; // 如果望远镜未连接，记录日志并退出
    }
    isSolveSYNC = true;
    double Ra_Degree = Tools::HourToDegree(Ra_Hour); // 将赤经从小时转换为度

    Logger::Log("TelescopeControl_SolveSYNC | CurrentRa(Degree):" + std::to_string(Ra_Degree) + "," + "CurrentDec(Degree):" + std::to_string(Dec_Degree), LogLevel::INFO, DeviceType::MAIN);
    isSavePngSuccess = false;
    startMainCameraCapture(1000); // 拍摄1秒曝光进行解析同步

    captureTimer.setSingleShot(true);

    // 连接拍摄定时器的超时信号到处理函数，处理拍摄完成后的逻辑
    connect(&captureTimer, &QTimer::timeout, [this](){
        // 如果需要中止拍摄和解算，则执行中止操作并返回
        if (EndCaptureAndSolve)
        {
            EndCaptureAndSolve = false;
            abortMainCameraCapture();
            Logger::Log("TelescopeControl_SolveSYNC | End Capture And Solve!!!", LogLevel::INFO, DeviceType::MAIN);
            isSolveSYNC = false;
            emit wsThread->sendMessageToClient("SolveImagefailed");
            return;
        }
        Logger::Log("TelescopeControl_SolveSYNC | WaitForShootToComplete ..." , LogLevel::INFO, DeviceType::MAIN);

        // 检查拍摄是否完成
        if (isSavePngSuccess)
        {
            // 停止拍摄定时器，表示拍摄任务完成
            captureTimer.stop();

            // 开始进行图像解析
            Tools::PlateSolve(SolveImageFileName, glFocalLength, glCameraSize_width, glCameraSize_height, false);

            solveTimer.setSingleShot(true);  // 设置解析定时器为单次触发

            connect(&solveTimer, &QTimer::timeout, [this]()
            {
                // 检查解析进程是否已结束
                bool solveProcessFinished = !Tools::isPlateSolveInProgress();

                if (Tools::isSolveImageFinish())  // 检查图像解析是否成功完成
                {
                    SloveResults result = Tools::ReadSolveResult(SolveImageFileName, glMainCCDSizeX, glMainCCDSizeY);  // 读取解析结果
                    Logger::Log("TelescopeControl_SolveSYNC | Plate Solve Result(RA_Degree, DEC_Degree):" + std::to_string(result.RA_Degree) + ", " + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);

                    if (result.RA_Degree == -1 && result.DEC_Degree == -1)
                    {
                        Logger::Log("TelescopeControl_SolveSYNC | Solve image failed...", LogLevel::INFO, DeviceType::MAIN);
                        Logger::Log("TelescopeControl_SolveSYNC | SolveImageFileName: " + SolveImageFileName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                        Logger::Log("TelescopeControl_SolveSYNC | mainCameraSaveFailedParse: " + std::string(mainCameraSaveFailedParse ? "true" : "false"), LogLevel::INFO, DeviceType::MAIN);
                        if (mainCameraSaveFailedParse)
                        {
                            int saveResult = solveFailedImageSave(SolveImageFileName);
                            if (saveResult == 0)
                            {
                                Logger::Log("TelescopeControl_SolveSYNC | Failed solve image saved successfully", LogLevel::INFO, DeviceType::MAIN);
                            }
                            else
                            {
                                Logger::Log("TelescopeControl_SolveSYNC | Failed to save failed solve image, error code: " + std::to_string(saveResult), LogLevel::WARNING, DeviceType::MAIN);
                            }
                        }
                        else
                        {
                            Logger::Log("TelescopeControl_SolveSYNC | mainCameraSaveFailedParse is disabled, skipping save", LogLevel::INFO, DeviceType::MAIN);
                        }
                        emit wsThread->sendMessageToClient("SolveImagefailed");  // 发送解析失败的消息
                        isSolveSYNC = false;
                        solveTimer.stop();  // 停止定时器
                    }
                    else
                    {
                        if (dpMount != NULL)
                        {
                            Logger::Log("TelescopeControl_SolveSYNC | syncTelescopeJNow | start", LogLevel::INFO, DeviceType::MAIN);
                            QString action = "SYNC";
                            bool isTrack = false;
                            indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
                            if (!isTrack)
                            {
                                indi_Client->setTelescopeTrackEnable(dpMount, true);
                            }
                            emit wsThread->sendMessageToClient("TelescopeTrack:ON");

                            // 解析结果 RA/DEC 为“度”，下发到 INDI 前需要转换为 RA 小时制
                            double solvedRaHour = Tools::DegreeToHour(result.RA_Degree);
                            double solvedDecDeg = result.DEC_Degree;

                            // 同步望远镜的当前位置到目标位置（JNOW, RA:hour / DEC:deg）
                            uint32_t syncResult = indi_Client->syncTelescopeJNow(dpMount, solvedRaHour, solvedDecDeg);
                            if (syncResult != QHYCCD_SUCCESS)
                            {
                                Logger::Log("TelescopeControl_SolveSYNC | syncTelescopeJNow failed",
                                            LogLevel::ERROR, DeviceType::MAIN);
                                emit wsThread->sendMessageToClient("SolveImagefailed");
                            }
                            else
                            {
                                Logger::Log("TelescopeControl_SolveSYNC | syncTelescopeJNow | end", LogLevel::INFO, DeviceType::MAIN);
                                // Logger::Log("TelescopeControl_SolveSYNC | DegreeToHour:" + std::to_string(Tools::DegreeToHour(result.RA_Degree)) + "DEC_Degree:" + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);

                                // indi_Client->setTelescopeRADECJNOW(dpMount, Tools::DegreeToHour(result.RA_Degree), result.DEC_Degree);  // 设置望远镜的目标位置
                                // Logger::Log("TelescopeControl_SolveSYNC | syncTelescopeJNow | end", LogLevel::INFO, DeviceType::MAIN);
                                // double a, b;
                                // indi_Client->getTelescopeRADECJNOW(dpMount, a, b);  // 获取望远镜的当前位置
                                // Logger::Log("TelescopeControl_SolveSYNC | Get_RA_Hour:" + std::to_string(a) + "Get_DEC_Degree:" + std::to_string(b), LogLevel::INFO, DeviceType::MAIN);
                                emit wsThread->sendMessageToClient("SolveImageSucceeded");
                            }

                            isSolveSYNC = false;
                            solveTimer.stop();  // 停止定时器
                        }
                        else
                        {
                            Logger::Log("TelescopeControl_SolveSYNC | No Mount Connect.", LogLevel::INFO, DeviceType::MAIN);
                            emit wsThread->sendMessageToClient("SolveImagefailed");  // 发送解析失败的消息
                            isSolveSYNC = false;
                            solveTimer.stop();  // 停止定时器
                            return;  // 如果望远镜未连接，记录日志并退出
                        }
                    }
                }
                else if (solveProcessFinished)
                {
                    // 解析进程已结束但未成功完成（退出码非0的情况）
                    Logger::Log("TelescopeControl_SolveSYNC | Solve process finished but failed (exit code != 0)", LogLevel::ERROR, DeviceType::MAIN);
                    Logger::Log("TelescopeControl_SolveSYNC | SolveImageFileName: " + SolveImageFileName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    Logger::Log("TelescopeControl_SolveSYNC | mainCameraSaveFailedParse: " + std::string(mainCameraSaveFailedParse ? "true" : "false"), LogLevel::INFO, DeviceType::MAIN);
                    if (mainCameraSaveFailedParse)
                    {
                        int saveResult = solveFailedImageSave(SolveImageFileName);
                        if (saveResult == 0)
                        {
                            Logger::Log("TelescopeControl_SolveSYNC | Failed solve image saved successfully", LogLevel::INFO, DeviceType::MAIN);
                        }
                        else
                        {
                            Logger::Log("TelescopeControl_SolveSYNC | Failed to save failed solve image, error code: " + std::to_string(saveResult), LogLevel::WARNING, DeviceType::MAIN);
                        }
                    }
                    else
                    {
                        Logger::Log("TelescopeControl_SolveSYNC | mainCameraSaveFailedParse is disabled, skipping save", LogLevel::INFO, DeviceType::MAIN);
                    }
                    emit wsThread->sendMessageToClient("SolveImagefailed");  // 发送解析失败的消息
                    isSolveSYNC = false;
                    solveTimer.stop();  // 停止定时器
                }
                else
                {
                    solveTimer.start(1000);  // 如果解析未完成，重新启动定时器继续等待
                }
            });

            solveTimer.start(1000);  // 启动解析定时器

        }
        else
        {
            // 如果拍摄未完成，重新启动拍摄定时器，继续等待
            captureTimer.start(1000);
        } });
    captureTimer.start(1000);
}

LocationResult MainWindow::TelescopeControl_GetLocation()
{
    LocationResult result;

    if (dpMount != NULL && indi_Client != nullptr)
    {
        const uint32_t getLocationResult =
            indi_Client->getLocation(dpMount, result.latitude_degree, result.longitude_degree, result.elevation);
        if (getLocationResult == QHYCCD_SUCCESS &&
            isValidObservatoryLocation(result.latitude_degree, result.longitude_degree))
        {
            observatorylatitude = result.latitude_degree;
            observatorylongitude = result.longitude_degree;
            if (indi_Client != nullptr)
                indi_Client->mountState.updateHomeRAHours(observatorylatitude, observatorylongitude);
            return result;
        }

        Logger::Log("TelescopeControl_GetLocation | INDI location unavailable or invalid, fallback to MainWindow cached location",
                    LogLevel::WARNING, DeviceType::MAIN);
    }

    if (!isValidObservatoryLocation(observatorylatitude, observatorylongitude))
    {
        bool latOk = false;
        bool lonOk = false;
        const double cachedLat = localLat.trimmed().toDouble(&latOk);
        const double cachedLon = localLon.trimmed().toDouble(&lonOk);
        if (latOk && lonOk && isValidObservatoryLocation(cachedLat, cachedLon))
        {
            observatorylatitude = cachedLat;
            observatorylongitude = cachedLon;
            if (indi_Client != nullptr)
                indi_Client->mountState.updateHomeRAHours(observatorylatitude, observatorylongitude);
            Logger::Log("TelescopeControl_GetLocation | restored MainWindow cached location from localLat/localLon: Latitude: " +
                            QString::number(observatorylatitude).toStdString() +
                            ", Longitude: " + QString::number(observatorylongitude).toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
        }
    }

    if (isValidObservatoryLocation(observatorylatitude, observatorylongitude))
    {
        result.latitude_degree = observatorylatitude;
        result.longitude_degree = observatorylongitude;
        result.elevation = 50.0;
        Logger::Log("TelescopeControl_GetLocation | using MainWindow cached location: Latitude: " +
                        QString::number(result.latitude_degree).toStdString() +
                        ", Longitude: " + QString::number(result.longitude_degree).toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("TelescopeControl_GetLocation | no valid INDI or MainWindow cached location available",
                    LogLevel::WARNING, DeviceType::MAIN);
    }

    return result;
}

QDateTime MainWindow::TelescopeControl_GetTimeUTC()
{
    if (dpMount != NULL)
    {
        QDateTime result;

        indi_Client->getTimeUTC(dpMount, result);

        return result;
    }

    return QDateTime();
}

SphericalCoordinates MainWindow::TelescopeControl_GetRaDec()
{
    if (dpMount != NULL)
    {
        SphericalCoordinates result;
        double RA_HOURS, DEC_DEGREE;
        indi_Client->getTelescopeRADECJNOW(dpMount, RA_HOURS, DEC_DEGREE);
        result.ra = Tools::HourToDegree(RA_HOURS);
        result.dec = DEC_DEGREE;

        return result;
    }

    return {};
}

void MainWindow::MountGoto(double Ra_Hour, double Dec_Degree)
{
    Logger::Log("MountGoto start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MountGoto | RaDec(Hour):" + std::to_string(Ra_Hour) + "," + std::to_string(Dec_Degree), LogLevel::INFO, DeviceType::MAIN);

    // 在执行 GOTO 之前，如当前处于导星状态，则暂时停止导星，待转动完成后再恢复
    pauseGuidingBeforeMountMove();

    // 停止和清理先前的计时器
    telescopeTimer.stop();
    telescopeTimer.disconnect();

    TelescopeControl_Goto(Ra_Hour, Dec_Degree);

    sleep(2); // 赤道仪的状态更新有一定延迟

    // 启动等待赤道仪转动的定时器
    telescopeTimer.setSingleShot(true);

    connect(&telescopeTimer, &QTimer::timeout, [this, Ra_Hour, Dec_Degree]()
            {
        if (WaitForTelescopeToComplete())
        {
            telescopeTimer.stop();  // 转动完成时停止定时器
            Logger::Log("MountGoto | Mount Goto Complete!", LogLevel::INFO, DeviceType::MAIN);

            // 如果本次 GOTO 之前处于导星状态，则在赤道仪转动完成后恢复导星
            resumeGuidingAfterMountMove();
            if (GotoThenSolve) // 判断是否进行解算
            {
                Logger::Log("MountGoto | Goto Then Solve!", LogLevel::INFO, DeviceType::MAIN);
                // 启动一次解算同步流程
                isSolveSYNC = true;
                TelescopeControl_SolveSYNC(); // 开始拍摄解析

                if (GotoOlveTimer != nullptr)
                {
                    delete GotoOlveTimer;
                    GotoOlveTimer = nullptr;
                }
                GotoOlveTimer = new QTimer();
                GotoOlveTimer->setSingleShot(true);
                connect(GotoOlveTimer, &QTimer::timeout, [this, Ra_Hour, Dec_Degree]()
                {
                    if (!isSolveSYNC)
                    {
                        GotoOlveTimer->stop();
                        Logger::Log("MountGoto | Goto Then Solve Complete!", LogLevel::INFO, DeviceType::MAIN);
                        // 解算同步完成后，仅再执行一次 Goto 回到目标坐标
                        TelescopeControl_Goto(Ra_Hour, Dec_Degree);
                    }else{
                        GotoOlveTimer->start(1000);
                    }
                });
                GotoOlveTimer->start(1000);
            }
        }
        else
        {
            telescopeTimer.start(1000);  // 继续等待
        } });

    telescopeTimer.start(1000);

    Logger::Log("MountGoto finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::MountOnlyGoto(double Ra_Hour, double Dec_Degree)
{
    if (dpMount == NULL)
    {
        Logger::Log("MountOnlyGoto | No Mount Connect.", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MountOnlyGotoFailed:No Mount Connect");  // 发送转到失败的消息
        return;
    }
    if (Ra_Hour < 0 || Ra_Hour > 24 || Dec_Degree < -90 || Dec_Degree > 90)
    {
        Logger::Log("MountOnlyGoto | Invalid RaDec(Hour):" + std::to_string(Ra_Hour) + "," + std::to_string(Dec_Degree), LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MountOnlyGotoFailed:Invalid RaDec(Hour)");  // 发送转到失败的消息
        return;
    }
    if (indi_Client->mountState.isMovingNow())
    {
        Logger::Log("MountOnlyGoto | Mount is Moving.", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MountOnlyGotoFailed:Mount is Moving");  // 发送转到失败的消息
        return;
    }
    Logger::Log("MountOnlyGoto start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MountOnlyGoto | RaDec(Hour):" + std::to_string(Ra_Hour) + "," + std::to_string(Dec_Degree), LogLevel::INFO, DeviceType::MAIN);

    // 在执行 Goto 之前，如当前处于导星状态，则暂时停止导星，待转动完成后再恢复
    pauseGuidingBeforeMountMove();

    // 停止和清理先前的计时器
    telescopeTimer.stop();
    telescopeTimer.disconnect();

    TelescopeControl_Goto(Ra_Hour, Dec_Degree); // 转到目标位置

    sleep(2); // 赤道仪的状态更新有一定延迟

    // 启动等待赤道仪转动的定时器
    telescopeTimer.setSingleShot(true);

    connect(&telescopeTimer, &QTimer::timeout, [this, Ra_Hour, Dec_Degree]()
    {
        if (WaitForTelescopeToComplete())
        {
            telescopeTimer.stop();  // 转动完成时停止定时器
            Logger::Log("MountOnlyGoto | Mount Only Goto Complete!", LogLevel::INFO, DeviceType::MAIN);
            // 如果本次 Goto 之前处于导星状态，则在赤道仪转动完成后恢复导星
            resumeGuidingAfterMountMove();
            emit wsThread->sendMessageToClient("MountOnlyGotoSuccess");  // 发送转到成功消息
        }
        else
        {
            telescopeTimer.start(1000);  // 继续等待赤道仪转动
        }
    });
    telescopeTimer.start(1000);

    Logger::Log("MountOnlyGoto finish!", LogLevel::INFO, DeviceType::MAIN);

}

void MainWindow::LoopSolveImage(QString Filename, int FocalLength, double CameraWidth, double CameraHeight)
{
    Logger::Log("LoopSolveImage(" + Filename.toStdString() + ") start ...", LogLevel::INFO, DeviceType::MAIN);

    if (!isLoopSolveImage)
    {
        Logger::Log("LoopSolveImage | Loop Solve Image end.", LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    solveTimer.stop();
    solveTimer.disconnect();

    Tools::PlateSolve(Filename, FocalLength, CameraWidth, CameraHeight, false);

    // 启动等待赤道仪转动的定时器
    solveTimer.setSingleShot(true);

    connect(&solveTimer, &QTimer::timeout, [this, Filename]()
            {
        // 检查赤道仪状态
        if (Tools::isSolveImageFinish())
        {
            SloveResults result = Tools::ReadSolveResult(Filename, glMainCCDSizeX, glMainCCDSizeY);
            Logger::Log("LoopSolveImage | Plate Solve Result(RA_Degree, DEC_Degree):" + std::to_string(result.RA_Degree) + ", " + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);

            if (result.RA_Degree == -1 && result.DEC_Degree == -1)
            {
                Logger::Log("LoopSolveImage | Solve image failed...", LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("SolveImagefailed");
                emit wsThread->sendMessageToClient("LoopSolveImageFinished");
            }
            else
            {
                emit wsThread->sendMessageToClient("RealTimeSolveImageResult:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree) + ":" + QString::number(Tools::RadToDegree(0)) + ":" + QString::number(Tools::RadToDegree(0)));
                emit wsThread->sendMessageToClient("LoopSolveImageFinished");
            }

            // CaptureAndSolve(glExpTime, true);
        }
        else
        {
            solveTimer.start(1000);  // 继续等待
        } });

    solveTimer.start(1000);
}

void MainWindow::ClearSloveResultList()
{
    SloveResultList.clear();
}

void MainWindow::RecoverySloveResul()
{
    for (const auto &result : SloveResultList)
    {
        Logger::Log("RecoverySloveResul | Plate Solve Result(RA_Degree, DEC_Degree):" + std::to_string(result.RA_Degree) + ", " + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("SolveImageResult:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree) + ":" + QString::number(Tools::RadToDegree(0)) + ":" + QString::number(Tools::RadToDegree(0)));
        emit wsThread->sendMessageToClient("SolveFovResult:" + QString::number(result.RA_0) + ":" + QString::number(result.DEC_0) + ":" + QString::number(result.RA_1) + ":" + QString::number(result.DEC_1) + ":" + QString::number(result.RA_2) + ":" + QString::number(result.DEC_2) + ":" + QString::number(result.RA_3) + ":" + QString::number(result.DEC_3));
    }
}

void MainWindow::getMountParameters()
{
    Logger::Log("getMountUiInfo ...", LogLevel::DEBUG, DeviceType::MAIN);
    QMap<QString, QString> parameters = Tools::readParameters("Mount");
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
    {
        Logger::Log("getMountParameters | " + it.key().toStdString() + ":" + it.value().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        if (it.key() == "AutoFlip"){
            emit wsThread->sendMessageToClient("AutoFlip:" + it.value());
            isAutoFlip = it.value() == "true";
            continue;
        }
        if (it.key() == "GotoThenSolve"){
            emit wsThread->sendMessageToClient("GotoThenSolve:" + it.value());
            GotoThenSolve = it.value() == "true";
            continue;
        }
        emit wsThread->sendMessageToClient(it.key() + ":" + it.value());
    }

    // 将当前 Mount 串口列表与已保存串口下发给前端（若无保存则 savedPort 为空）
    sendSerialPortOptions("Mount");

    Logger::Log("getMountParameters finish!", LogLevel::DEBUG, DeviceType::MAIN);
}

void MainWindow::synchronizeTime(QString time, QString date)
{
    Logger::Log("synchronizeTime start ...", LogLevel::DEBUG, DeviceType::MAIN);
    Logger::Log("synchronizeTime time: " + time.toStdString() + ", date: " + date.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);

    // 仅首次对时禁用自动时间同步，刷新/重复对时不再执行以缩短耗时（约 2s）
    static bool automaticTimeSyncDisabled = false;
    if (!automaticTimeSyncDisabled)
    {
        Logger::Log("Disabling automatic time synchronization...", LogLevel::DEBUG, DeviceType::MAIN);
        int disableResult1 = system("sudo systemctl stop systemd-timesyncd");
        int disableResult2 = system("sudo systemctl disable systemd-timesyncd");
        int disableResult3 = system("sudo timedatectl set-ntp false");
        if (disableResult1 != 0 || disableResult2 != 0 || disableResult3 != 0)
            Logger::Log("Warning: Failed to disable some automatic time sync services", LogLevel::WARNING, DeviceType::MAIN);
        else
            Logger::Log("Automatic time synchronization disabled successfully", LogLevel::DEBUG, DeviceType::MAIN);
        automaticTimeSyncDisabled = true;
        QThread::msleep(300);   // 缩短等待，原 1000ms 易在刷新时拖慢
    }

    // Create the command string
    QString command = "sudo date -s \"" + date + " " + time + "\"";

    Logger::Log("synchronizeTime command: " + command.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);

    // Execute the command
    int result = system(command.toStdString().c_str());

    if (result == 0)
    {
        Logger::Log("synchronizeTime finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("synchronizeTime failed!", LogLevel::ERROR, DeviceType::MAIN);
    }
}

void MainWindow::setMountLocation(QString lat, QString lon)
{
    Logger::Log("setMountLocation start ...", LogLevel::DEBUG, DeviceType::MAIN);
    observatorylatitude = lat.toDouble();
    observatorylongitude = lon.toDouble();
    if (indi_Client != nullptr)
        indi_Client->mountState.updateHomeRAHours(observatorylatitude, observatorylongitude);
    // 仅当 Mount 已连接时才下发 INDI setLocation，避免未连接时 3s 超时拖慢刷新
    if (dpMount == nullptr || !dpMount->isConnected())
    {
        Logger::Log("setMountLocation | Mount not connected, skip INDI setLocation", LogLevel::DEBUG, DeviceType::MAIN);
        return;
    }
    indi_Client->setLocation(dpMount, observatorylatitude, observatorylongitude, 50);
}

void MainWindow::setMountUTC(QString time, QString date)
{
    Logger::Log("setMountUTC start ...", LogLevel::DEBUG, DeviceType::MAIN);
    if (dpMount == nullptr || !dpMount->isConnected())
    {
        Logger::Log("setMountUTC | Mount not connected, skip INDI time sync (avoid 3s timeout)", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    QDateTime datetime = QDateTime::fromString(date + "T" + time, Qt::ISODate);
    indi_Client->setTimeUTC(dpMount, datetime);
    Logger::Log("UTC Time set for Mount: " + datetime.toString(Qt::ISODate).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    indi_Client->getTimeUTC(dpMount, datetime);
    Logger::Log("UTC Time: " + datetime.currentDateTimeUtc().toString(Qt::ISODate).toStdString(), LogLevel::INFO, DeviceType::MAIN);
}
