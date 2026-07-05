#include "mainwindow.h"

#include <regex>

extern INDI::BaseDevice *dpGuider;
extern INDI::BaseDevice *dpPoleScope;
extern INDI::BaseDevice *dpMainCamera;

void MainWindow::initINDIServer()
{
    Logger::Log("initINDIServer ...", LogLevel::INFO, DeviceType::MAIN);
    // 先清理遗留的 QHY INDI 驱动子进程，避免它们继续占用 USB 设备，
    // 导致新启动的 indiserver 只能发现部分相机。
    system("pkill -f '^indi_qhy_ccd$'");
    system("pkill indiserver");
    system("rm -f /tmp/myFIFO");
    system("mkfifo /tmp/myFIFO");
    // FIFO 已重建：恢复 Tools 的 FIFO 熔断状态，允许后续 start/stop 写入
    Tools::resetIndiFifoState();
    glIndiServer = new QProcess();

    // 已知部分驱动会在 verbose 模式下刷屏输出（例如 CCD_TEMPERATURE/CCD_COOLER_POWER 不存在）
    // 这里关闭 -v，并把输出重定向到文件，避免终端持续打印。
    glIndiServer->setProcessChannelMode(QProcess::MergedChannels);
    glIndiServer->setStandardOutputFile("/tmp/indiserver.log", QIODevice::Append);
    glIndiServer->setStandardErrorFile("/tmp/indiserver.log", QIODevice::Append);

    glIndiServer->setProgram("indiserver");
    glIndiServer->setArguments(QStringList() << "-f" << "/tmp/myFIFO" << "-p" << "7624");
    glIndiServer->start();
    Logger::Log("initINDIServer finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::handleIndiServerOutput()
{
    QByteArray output = glIndiServer->readAllStandardOutput();
    Logger::Log("INDI Server Output: " + output.toStdString(), LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::handleIndiServerError()
{
    QByteArray error = glIndiServer->readAllStandardError();
    Logger::Log("INDI Server Error: " + error.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
}

void MainWindow::initINDIClient()
{
    Logger::Log("initINDIClient ...", LogLevel::INFO, DeviceType::MAIN);
    indi_Client = new MyClient();
    indi_Client->setServer("localhost", 7624);
    indi_Client->setConnectionTimeout(3, 0);
    Logger::Log("setConnectionTimeout is 3 seconds!", LogLevel::INFO, DeviceType::MAIN);
    indi_Client->setImageReceivedCallback(
        [this](const std::string &filename, const std::string &devname)
        {
            if (dpGuider != NULL && dpGuider->getDeviceName() == devname)
            {
                guiderExposureInFlight = false;
                const QString fitsPath = QString::fromStdString(filename);

                if (guiderCore)
                {
                    QMetaObject::invokeMethod(guiderCore, "onNewFrame", Qt::QueuedConnection,
                                              Q_ARG(QString, fitsPath));
                }
                else
                {
                    PersistGuidingFits(fitsPath);
                    if (isGuiderLoopExp && guiderLoopTimer)
                    {
                        // INDI 图像回调可能运行在非 GUI 线程，必须回到 MainWindow 线程再启动 QTimer。
                        QMetaObject::invokeMethod(this, [this]() {
                            if (isGuiderLoopExp && guiderLoopTimer)
                                guiderLoopTimer->start(1);
                        }, Qt::QueuedConnection);
                    }
                }
                return;
            }

            // 曝光完成
            if (dpMainCamera != NULL)
            {
                if (dpMainCamera->getDeviceName() == devname)
                {
                    lastMainCaptureFitsPath = QString::fromStdString(filename);
                    glMainCameraStatu = "Displaying";
                    ShootStatus = "Completed";
                    if (autoFocuserIsROI && isAutoFocus)
                    {
                        saveFitsAsJPG(QString::fromStdString(filename), true);
                        Logger::Log("saveFitsAsJPG", LogLevel::DEBUG, DeviceType::MAIN);
                        autoFocus->setCaptureComplete(QString::fromStdString(filename));
                        return;
                    }
                    else if (!autoFocuserIsROI && isAutoFocus)
                    {

                        saveFitsAsPNG(QString::fromStdString(filename), true);

                        Logger::Log("saveFitsAsPNG", LogLevel::DEBUG, DeviceType::MAIN);
                        autoFocus->setCaptureComplete(QString::fromStdString(filename));
                        return;
                    }

                    // 检查：如果 isFocusLoopShooting 为 false 但 glIsFocusingLooping 刚被重置，
                    // 说明这可能是 ROI 停止时的残留帧，应该丢弃或按 ROI 处理
                    if (glIsFocusingLooping == false && !isFocusLoopShooting)
                    {
                        // 读取 FITS 获取图像尺寸，判断是否为 ROI 残留帧
                        fitsfile *fptr = nullptr;
                        int status = 0;
                        long naxes[2] = {0, 0};
                        int naxis = 0;

                        if (fits_open_file(&fptr, filename.c_str(), READONLY, &status) == 0)
                        {
                            fits_get_img_dim(fptr, &naxis, &status);
                            if (naxis == 2)
                            {
                                fits_get_img_size(fptr, 2, naxes, &status);
                            }
                            fits_close_file(fptr, &status);
                        }

                        // 如果图像尺寸远小于全分辨率（例如 < 80%），判定为 ROI 残留帧
                        bool isRoiFrame = false;
                        if (naxes[0] > 0 && naxes[1] > 0 && glMainCCDSizeX > 0 && glMainCCDSizeY > 0)
                        {
                            double widthRatio = (double)naxes[0] / glMainCCDSizeX;
                            double heightRatio = (double)naxes[1] / glMainCCDSizeY;
                            if (widthRatio < 0.8 || heightRatio < 0.8)
                            {
                                isRoiFrame = true;
                                Logger::Log("Image received after ROI stop, detected as ROI frame (" +
                                           std::to_string(naxes[0]) + "x" + std::to_string(naxes[1]) +
                                           " vs " + std::to_string(glMainCCDSizeX) + "x" + std::to_string(glMainCCDSizeY) +
                                           "), discarding...", LogLevel::WARNING, DeviceType::CAMERA);
                            }
                        }

                        // 如果是 ROI 残留帧，直接丢弃
                        if (isRoiFrame)
                        {
                            glMainCameraStatu = "IDLE";
                            Logger::Log("ROI residual frame discarded", LogLevel::INFO, DeviceType::CAMERA);
                            return;
                        }

                        // 否则按正常拍摄处理
                        emit wsThread->sendMessageToClient("ExposureCompleted");
                        emitCaptureTrace(QStringLiteral("backend_exposure_completed"), currentCaptureTraceStartedAtMs,
                                         QStringLiteral("source=fits_callback"));
                        Logger::Log("ExposureCompleted", LogLevel::INFO, DeviceType::CAMERA);
                        if (polarAlignment != nullptr)
                        {
                            if (polarAlignment->isRunning())
                            {
                                notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::MainCamera,
                                                                 QString::fromStdString(filename));
                                return;
                            }
                        }
                        saveFitsAsPNG(QString::fromStdString(filename), true);

                        // 如果自动保存开启，自动保存图像
                        if (mainCameraAutoSave && isScheduleRunning == false)
                        {
                            Logger::Log("Auto Save enabled, saving captured image...", LogLevel::INFO, DeviceType::MAIN);
                            CaptureImageSave();
                        }
                    }
                    else
                    {

                        saveFitsAsJPG(QString::fromStdString(filename), true);

                        Logger::Log("saveFitsAsJPG", LogLevel::DEBUG, DeviceType::MAIN);
                    }
                }
            }

            // 导星相机：INDI 直出图（替代 PHD2）。收到一帧后：
            // 1) 生成导星预览 JPG（前端 Guide 画面）
            // 2) 把 FITS 保存到与主相机相同的保存目录下，命名为 guider.fits（覆盖写）
            // 3) 若开启循环曝光，则调度下一帧
            if (dpGuider != NULL)
            {
                if (dpGuider->getDeviceName() == devname)
                {
                    const QString fitsPath = QString::fromStdString(filename);
                    if (polarGuiderSingleCapturePending)
                    {
                        notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::Guider, fitsPath);
                        polarGuiderSingleCapturePending = false;
                    }

                    // 1) 预览：从 FITS 读取单通道并拉伸为 8-bit，复用原有 saveGuiderImageAsJPG 的前端协议
                    {
                        fitsfile *fptr = nullptr;
                        int status = 0;
                        int bitpix = 0;
                        int naxis = 0;
                        long naxes[2] = {0, 0};

                        if (fits_open_file(&fptr, fitsPath.toUtf8().constData(), READONLY, &status) == 0)
                        {
                            fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status);
                            if (status == 0 && naxis >= 2 && naxes[0] > 0 && naxes[1] > 0)
                            {
                                const long w = naxes[0];
                                const long h = naxes[1];
                                const long npix = w * h;
                                long fpixel[2] = {1, 1};

                                cv::Mat img16;
                                img16.create((int)h, (int)w, CV_16UC1);

                                if (bitpix == 8)
                                {
                                    std::vector<unsigned char> tmp((size_t)npix);
                                    fits_read_pix(fptr, TBYTE, fpixel, npix, NULL, tmp.data(), NULL, &status);
                                    if (status == 0)
                                    {
                                        uint16_t *dst = reinterpret_cast<uint16_t *>(img16.data);
                                        for (long i = 0; i < npix; ++i)
                                            dst[i] = static_cast<uint16_t>(tmp[(size_t)i]) * 257;
                                    }
                                }
                                else
                                {
                                    fits_read_pix(fptr, TUSHORT, fpixel, npix, NULL, img16.data, NULL, &status);
                                }

                                if (status == 0)
                                {
                                    double minVal = 0.0, maxVal = 0.0;
                                    cv::minMaxLoc(img16, &minVal, &maxVal);

                                    uint16_t B = 0, W = 65535;
                                    Tools::GetAutoStretch(img16, 0, B, W);

                                    if (maxVal > 0.0)
                                    {
                                        const uint16_t maxU16 = (uint16_t)std::min(65535.0, std::max(0.0, maxVal));
                                        if (W > (uint16_t)std::min<uint32_t>(65535u, (uint32_t)maxU16 + 1024u))
                                        {
                                            B = 0;
                                            W = std::max<uint16_t>(1, maxU16);
                                        }
                                        if (W <= B) W = (uint16_t)std::min<uint32_t>(65535u, (uint32_t)B + 10u);
                                    }

                                    Logger::Log("GuiderPreviewStretch | bitpix=" + std::to_string(bitpix) +
                                                    " min=" + std::to_string(minVal) +
                                                    " max=" + std::to_string(maxVal) +
                                                    " B=" + std::to_string(B) +
                                                    " W=" + std::to_string(W),
                                                LogLevel::DEBUG, DeviceType::GUIDER);

                                    cv::Mat img8;
                                    img8.create(img16.rows, img16.cols, CV_8UC1);
                                    Tools::Bit16To8_Stretch(img16, img8, B, W);
                                    double min8 = 0.0, max8 = 0.0;
                                    cv::minMaxLoc(img8, &min8, &max8);
                                    if (max8 <= 0.0 && maxVal > 0.0) {
                                        const uint16_t maxU16 = (uint16_t)std::min(65535.0, std::max(1.0, maxVal));
                                        B = 0;
                                        W = maxU16;
                                        Tools::Bit16To8_Stretch(img16, img8, B, W);
                                        Logger::Log("GuiderPreviewStretch | fallback restretch applied (img8 max==0). B=" +
                                                        std::to_string(B) + " W=" + std::to_string(W),
                                                    LogLevel::INFO, DeviceType::GUIDER);
                                    }
                                    saveGuiderImageAsJPG(img8);
                                }
                            }
                            fits_close_file(fptr, &status);
                        }
                    }

                    // 2) 保存：与主相机 CaptureImageSave 的目录结构保持一致（按日期）
                    {
                        std::time_t currentTime = std::time(nullptr);
                        std::tm *timeInfo = std::localtime(&currentTime);
                        char buffer[80];
                        std::strftime(buffer, 80, "%Y-%m-%d", timeInfo);

                        const QString destinationDirectory = ImageSaveBaseDirectory + "/CaptureImage/" + QString(buffer);
                        const QString destinationPath = destinationDirectory + "/guider.fits";
                        const bool isUSBSave = (saveMode != "local");

                        const QString dirPathToCreate = isUSBSave ? destinationDirectory : QString();
                        int checkResult = checkStorageSpaceAndCreateDirectory(
                            fitsPath,
                            ImageSaveBaseDirectory + "/CaptureImage",
                            dirPathToCreate,
                            "GuiderFitsSave",
                            isUSBSave,
                            nullptr);
                        if (checkResult == 0)
                        {
                            saveImageFile(fitsPath, destinationPath, "GuiderFitsSave", isUSBSave);
                        }
                    }

                    // 3) 循环曝光：放行下一帧
                    guiderExposureInFlight = false;
                    if (isGuiderLoopExp && guiderLoopTimer)
                    {
                        QMetaObject::invokeMethod(this, [this]() {
                            if (isGuiderLoopExp && guiderLoopTimer)
                                guiderLoopTimer->start(1);
                        }, Qt::QueuedConnection);
                    }
                }
            }
            if (dpPoleScope != NULL)
            {
                if (dpPoleScope->getDeviceName() == devname)
                {
                    const QString fitsPath = QString::fromStdString(filename);
                    if (polarGuiderSingleCapturePending)
                    {
                        notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::PoleCamera, fitsPath);
                        polarGuiderSingleCapturePending = false;
                    }
                    guiderExposureInFlight = false;
                }
            }
        });
    Logger::Log("indi_Client->setImageReceivedCallback finish!", LogLevel::INFO, DeviceType::MAIN);

    indi_Client->setMessageReceivedCallback(
        [this](const std::string &message)
        {
            QString messageStr = QString::fromStdString(message.c_str());

            std::regex timestampRegex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}: )");
            messageStr = QString::fromStdString(std::regex_replace(messageStr.toStdString(), timestampRegex, ""));

            std::regex typeRegex(R"(\[(INFO|WARNING|ERROR)\])");
            std::smatch typeMatch;
            QString logType;
            if (std::regex_search(message, typeMatch, typeRegex) && typeMatch.size() > 1)
            {
                logType = QString::fromStdString(typeMatch[1].str());
                messageStr = QString::fromStdString(std::regex_replace(messageStr.toStdString(), typeRegex, ""));
            }

            if (messageStr.contains("Telescope focal length is missing.") ||
                messageStr.contains("Telescope aperture is missing."))
            {
                return;
            }
            if (logType == "WARNING")
            {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::WARNING, deviceType);
            }
            else if (logType == "ERROR")
            {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::ERROR, deviceType);
            }
            else
            {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                if (messageStr.contains("Image saved to /dev/shm/guiding.fits"))
                    Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::DEBUG, deviceType);
                else
                    Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::INFO, deviceType);
            }

            std::regex regexPattern(R"(OnStep slew/syncError:\s*(.*))");
            std::smatch matchResult;

            if (std::regex_search(message, matchResult, regexPattern))
            {
                if (matchResult.size() > 1)
                {
                    QString errorContent = QString::fromStdString(matchResult[1].str());
                    Logger::Log("OnStep Error: " + errorContent.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("OnStep Error:" + errorContent);
                    MountGotoError = true;
                }
            }
        });
    Logger::Log("indi_Client->setMessageReceivedCallback finish!", LogLevel::INFO, DeviceType::MAIN);
}
