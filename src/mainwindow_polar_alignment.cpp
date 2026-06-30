#include "mainwindow.h"

extern INDI::BaseDevice *dpMount;
extern INDI::BaseDevice *dpGuider;
extern INDI::BaseDevice *dpPoleScope;
extern INDI::BaseDevice *dpMainCamera;
extern SdkDeviceHandle sdkGuiderHandle;
extern SdkDeviceHandle sdkPoleScopeHandle;
extern SdkDeviceHandle sdkMainCameraHandle;

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

const char *polarRoleName(MainWindow::PolarAlignmentCameraRole role)
{
    switch (role)
    {
    case MainWindow::PolarAlignmentCameraRole::Guider:
        return "Guider";
    case MainWindow::PolarAlignmentCameraRole::PoleCamera:
        return "PoleCamera";
    case MainWindow::PolarAlignmentCameraRole::MainCamera:
    default:
        return "MainCamera";
    }
}

bool isPoleMasterName(const QString &name)
{
    return name.contains("POLEMASTER", Qt::CaseInsensitive);
}

} // namespace

MainWindow::PolarAlignmentCameraRole MainWindow::parsePolarAlignmentCameraRole(const QString &roleText)
{
    const QString normalized = roleText.trimmed().toLower();
    if (normalized == "guider")
        return PolarAlignmentCameraRole::Guider;
    if (normalized == "polecamera" || normalized == "pole" || normalized == "polescope")
        return PolarAlignmentCameraRole::PoleCamera;
    return PolarAlignmentCameraRole::MainCamera;
}

void MainWindow::notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole role, const QString &fitsPath)
{
    if (role != currentPolarAlignmentCameraRole)
        return;

    const QString normalizedPath = fitsPath.trimmed();
    if (normalizedPath.isEmpty())
        return;

    if (role == PolarAlignmentCameraRole::PoleCamera && wsThread != nullptr)
    {
        savePoleMasterPreviewAsJPG(normalizedPath);
    }

    if (polarAlignment != nullptr && polarAlignment->isRunning())
    {
        polarAlignment->setCapturedImagePath(normalizedPath);
        polarAlignment->setCaptureEnd(true);
    }
    else if (poleMasterPolarAlignment != nullptr && poleMasterPolarAlignment->isRunning())
    {
        poleMasterPolarAlignment->setCapturedImagePath(normalizedPath);
        poleMasterPolarAlignment->setCaptureEnd(true);
    }
    else
    {
        return;
    }

    Logger::Log("PolarAlignment capture ready | role=" +
                    std::string(polarRoleName(role)) +
                    " | fits=" + normalizedPath.toStdString(),
                LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::savePoleMasterPreviewAsJPG(const QString &fitsPath)
{
    Logger::Log("savePoleMasterPreviewAsJPG | start fits=" + fitsPath.toStdString(),
                LogLevel::INFO,
                DeviceType::MAIN);

    cv::Mat image;
    const int status = Tools::readFits(fitsPath.toLocal8Bit().constData(), image);
    if (status != 0 || image.empty())
    {
        Logger::Log("savePoleMasterPreviewAsJPG | failed to read FITS: " + fitsPath.toStdString(),
                    LogLevel::ERROR,
                    DeviceType::MAIN);
        return;
    }

    cv::Mat preview8;
    if (image.depth() == CV_8U)
    {
        preview8 = image;
        Logger::Log("savePoleMasterPreviewAsJPG | 8-bit source, save without stretch size=" +
                        std::to_string(preview8.cols) + "x" + std::to_string(preview8.rows),
                    LogLevel::INFO,
                    DeviceType::MAIN);
    }
    else
    {
        cv::Mat image16;
        if (image.type() == CV_16UC1)
            image16 = image;
        else
        {
            cv::Mat gray;
            if (image.channels() == 3)
                cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
            else if (image.channels() == 4)
                cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
            else
                gray = image;
            if (gray.type() == CV_16UC1)
                image16 = gray;
            else
                gray.convertTo(image16, CV_16UC1);
        }

        uint16_t black = 0;
        uint16_t white = 65535;
        Tools::GetAutoStretch(image16, 0, black, white);

        double minVal = 0.0;
        double maxVal = 0.0;
        cv::minMaxLoc(image16, &minVal, &maxVal);
        if (white <= black || (maxVal > 0.0 && white > std::min(65535.0, maxVal + 4096.0)))
        {
            black = static_cast<uint16_t>(std::max(0.0, std::min(65535.0, minVal)));
            white = static_cast<uint16_t>(std::max<double>(black + 1, std::min(65535.0, maxVal)));
        }

        preview8 = cv::Mat(image16.rows, image16.cols, CV_8UC(image16.channels()));
        Tools::Bit16To8_Stretch(image16, preview8, black, white);
        Logger::Log("savePoleMasterPreviewAsJPG | 16-bit stretch done size=" +
                        std::to_string(preview8.cols) + "x" + std::to_string(preview8.rows) +
                        " black=" + std::to_string(black) +
                        " white=" + std::to_string(white),
                    LogLevel::INFO,
                    DeviceType::MAIN);
    }
    if (preview8.empty())
    {
        Logger::Log("savePoleMasterPreviewAsJPG | stretched preview is empty",
                    LogLevel::ERROR,
                    DeviceType::MAIN);
        return;
    }

    const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const std::string fileName = "PoleMasterImage_" + uniqueId.toStdString() + ".jpg";
    const std::string filePath = vueDirectoryPath + fileName;
    if (!cv::imwrite(filePath, preview8))
    {
        Logger::Log("savePoleMasterPreviewAsJPG | failed to write JPG: " + filePath,
                    LogLevel::ERROR,
                    DeviceType::MAIN);
        return;
    }

    const std::string command = "sudo ln -sf " + filePath + " " + vueImagePath + fileName;
    system(command.c_str());

    Logger::Log("savePoleMasterPreviewAsJPG | preview ready file=" + fileName +
                    " path=" + filePath +
                    " link=" + vueImagePath + fileName,
                LogLevel::INFO,
                DeviceType::MAIN);

    const QString frameId = QFileInfo(fitsPath).completeBaseName();
    emit wsThread->sendMessageToClient(QString("PoleMasterAlignmentFrameData:%1:%2:%3:%4")
                                           .arg(QString::fromStdString(fileName))
                                           .arg(preview8.cols)
                                           .arg(preview8.rows)
                                           .arg(frameId));

    auto cleanupOldPoleMasterImages = [&](const QString &dirPath, bool includeSymlinks, const QString &protectedFileName) {
        try
        {
            const fs::path dirFsPath = dirPath.toStdString();
            if (!fs::exists(dirFsPath)) return;
            std::vector<fs::path> items;
            const std::string protectedName = protectedFileName.toStdString();
            for (const auto &entry : fs::directory_iterator(dirFsPath))
            {
                const std::string name = entry.path().filename().string();
                if (name.rfind("PoleMasterImage_", 0) != 0 ||
                    name.size() < 4 ||
                    name.compare(name.size() - 4, 4, ".jpg") != 0 ||
                    name == protectedName)
                {
                    continue;
                }
                const bool isLink = fs::is_symlink(entry.symlink_status());
                const bool isFile = fs::is_regular_file(entry.status());
                if ((includeSymlinks && isLink) || (!includeSymlinks && isFile))
                    items.push_back(entry.path());
            }
            std::sort(items.begin(), items.end(), [](const fs::path &a, const fs::path &b) {
                std::error_code eca;
                std::error_code ecb;
                return fs::last_write_time(a, eca) > fs::last_write_time(b, ecb);
            });
            constexpr size_t kKeepRecentPoleMasterImages = 8;
            for (size_t i = kKeepRecentPoleMasterImages; i < items.size(); ++i)
            {
                std::error_code ec;
                fs::remove(items[i], ec);
            }
        }
        catch (...) {}
    };

    const QString protectedName = QString::fromStdString(fileName);
    cleanupOldPoleMasterImages(QString::fromStdString(vueDirectoryPath), false, protectedName);
    cleanupOldPoleMasterImages(QString::fromStdString(vueImagePath), true, protectedName);
}

bool MainWindow::initPolarAlignment(PolarAlignmentCameraRole role)
{
    const bool mountConnected = (dpMount != nullptr) || isMountSDK();
    if (!mountConnected)
    {
        Logger::Log("initPolarAlignment | mount is not connected", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,Mount must be connected");
        return false;
    }

    if (role == PolarAlignmentCameraRole::MainCamera && !isMainCameraConnected())
    {
        Logger::Log("initPolarAlignment | main camera is selected but not connected", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,MainCamera is not connected");
        return false;
    }

    if (role == PolarAlignmentCameraRole::Guider && !isGuiderCameraConnected())
    {
        Logger::Log("initPolarAlignment | guider is selected but not connected", LogLevel::ERROR, DeviceType::GUIDER);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,Guider is not connected");
        return false;
    }

    if (role == PolarAlignmentCameraRole::PoleCamera && !isPoleCameraConnected())
    {
        Logger::Log("initPolarAlignment | pole camera is selected but not connected", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,PoleCamera is not connected");
        return false;
    }

    if (isMountSDK() && dpMount == nullptr)
    {
        Logger::Log("initPolarAlignment | SDK模式赤道仪暂不支持，请使用INDI模式", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,SDK模式赤道仪暂不支持");
        return false;
    }
    if (indi_Client == nullptr)
    {
        Logger::Log("initPolarAlignment | indi_Client is nullptr", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,indi_Client is nullptr");
        return false;
    }

    int selectedFocalLength = 0;
    if (role == PolarAlignmentCameraRole::MainCamera)
    {
        selectedFocalLength = getMainCameraFocalLengthFromConfigAndMigrateIfNeeded();
        if (selectedFocalLength <= 0)
        {
            Logger::Log("initPolarAlignment | MainCameraFocalLength is not set", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,focal length is not set");
            return false;
        }
    }
    else if (role == PolarAlignmentCameraRole::Guider)
    {
        double guiderFocal = guiderFocalLengthMm;
        if (guiderFocal <= 0.0)
        {
            std::unordered_map<std::string, std::string> config;
            Tools::readClientSettings("config/config.ini", config);
            auto it = config.find("GuiderFocalLength");
            if (it != config.end())
            {
                bool ok = false;
                const double v = QString::fromStdString(it->second).trimmed().toDouble(&ok);
                if (ok && v > 0.0)
                    guiderFocal = v;
            }
        }
        if (guiderFocal <= 0.0)
        {
            Logger::Log("initPolarAlignment | GuiderFocalLength is not set", LogLevel::WARNING, DeviceType::GUIDER);
            emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,guider focal length is not set");
            return false;
        }
        guiderFocalLengthMm = guiderFocal;
        selectedFocalLength = static_cast<int>(std::lround(guiderFocal));
    }
    else
    {
        double poleFocal = 0.0;
        std::unordered_map<std::string, std::string> config;
        Tools::readClientSettings("config/config.ini", config);
        auto it = config.find("PoleCameraFocalLength");
        if (it != config.end())
        {
            bool ok = false;
            const double v = QString::fromStdString(it->second).trimmed().toDouble(&ok);
            if (ok && v > 0.0)
                poleFocal = v;
        }
        if (poleFocal <= 0.0)
        {
            Logger::Log("initPolarAlignment | PoleCameraFocalLength is not set", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,pole camera focal length is not set");
            return false;
        }
        selectedFocalLength = static_cast<int>(std::lround(poleFocal));
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
            Logger::Log("initPolarAlignment | using MainWindow localLat/localLon fallback - Latitude: " +
                            QString::number(observatorylatitude).toStdString() +
                            ", Longitude: " + QString::number(observatorylongitude).toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
        }
    }

    if (!isValidObservatoryLocation(observatorylatitude, observatorylongitude))
    {
        Logger::Log("initPolarAlignment | observatorylatitude or observatorylongitude is invalid", LogLevel::WARNING, DeviceType::MAIN);
    }

    double cameraWidthMm = 0.0;
    double cameraHeightMm = 0.0;
    INDI::BaseDevice *captureDevice = nullptr;
    bool useSdkCaptureSource = false;
    QString captureRoleName = "MainCamera";

    if (role == PolarAlignmentCameraRole::MainCamera)
    {
        captureDevice = dpMainCamera;
        useSdkCaptureSource = isMainCameraSDK();
        captureRoleName = "MainCamera";
        cameraWidthMm = glCameraSize_width;
        cameraHeightMm = glCameraSize_height;

        if (cameraWidthMm <= 0 || cameraHeightMm <= 0)
        {
            if (!isMainCameraSDK() && dpMainCamera != nullptr)
            {
                double pixelsize, pixelsizX, pixelsizY;
                int maxX, maxY, bitDepth;
                indi_Client->getCCDBasicInfo(dpMainCamera, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
                cameraWidthMm = maxX * pixelsize / 1000.0;
                cameraHeightMm = maxY * pixelsize / 1000.0;
                cameraWidthMm = std::round(cameraWidthMm * 10.0) / 10.0;
                cameraHeightMm = std::round(cameraHeightMm * 10.0) / 10.0;
                glCameraSize_width = cameraWidthMm;
                glCameraSize_height = cameraHeightMm;
            }
            else if (isMainCameraSDK() && sdkMainCameraHandle != nullptr)
            {
                SdkCommand chipInfoCmd;
                chipInfoCmd.type = SdkCommandType::Custom;
                chipInfoCmd.name = "GetChipInfo";
                chipInfoCmd.payload = std::any();
                SdkResult chipInfoRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, chipInfoCmd);

                if (chipInfoRes.success)
                {
                    try
                    {
                        SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                        glMainCCDSizeX = chipInfo.maxImageSizeX;
                        glMainCCDSizeY = chipInfo.maxImageSizeY;
                        cameraWidthMm = chipInfo.chipWidthMM;
                        cameraHeightMm = chipInfo.chipHeightMM;
                        glCameraSize_width = cameraWidthMm;
                        glCameraSize_height = cameraHeightMm;
                    }
                    catch (const std::bad_any_cast &e)
                    {
                        Logger::Log("initPolarAlignment | SDK GetChipInfo bad_any_cast: " + std::string(e.what()),
                                    LogLevel::ERROR, DeviceType::MAIN);
                    }
                }
                else
                {
                    Logger::Log("initPolarAlignment | SDK GetChipInfo failed: " + chipInfoRes.message,
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }
    }
    else if (role == PolarAlignmentCameraRole::Guider)
    {
        captureDevice = dpGuider;
        useSdkCaptureSource = isGuiderCameraSDK();
        captureRoleName = "Guider";

        if (!isGuiderCameraSDK() && dpGuider != nullptr)
        {
            double pixelsize, pixelsizX, pixelsizY;
            int maxX, maxY, bitDepth;
            indi_Client->getCCDBasicInfo(dpGuider, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
            cameraWidthMm = maxX * pixelsize / 1000.0;
            cameraHeightMm = maxY * pixelsize / 1000.0;
            cameraWidthMm = std::round(cameraWidthMm * 10.0) / 10.0;
            cameraHeightMm = std::round(cameraHeightMm * 10.0) / 10.0;
        }
        else if (isGuiderCameraSDK() && sdkGuiderHandle != nullptr)
        {
            SdkCommand chipInfoCmd;
            chipInfoCmd.type = SdkCommandType::Custom;
            chipInfoCmd.name = "GetChipInfo";
            chipInfoCmd.payload = std::any();
            SdkResult chipInfoRes = SdkManager::instance().callByHandle(sdkGuiderHandle, chipInfoCmd);

            if (chipInfoRes.success)
            {
                try
                {
                    SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                    cameraWidthMm = chipInfo.chipWidthMM;
                    cameraHeightMm = chipInfo.chipHeightMM;
                }
                catch (const std::bad_any_cast &e)
                {
                    Logger::Log("initPolarAlignment | SDK GetChipInfo bad_any_cast: " + std::string(e.what()),
                                LogLevel::ERROR, DeviceType::MAIN);
                }
            }
            else
            {
                Logger::Log("initPolarAlignment | SDK GetChipInfo failed: " + chipInfoRes.message,
                            LogLevel::WARNING, DeviceType::GUIDER);
            }
        }
    }
    else
    {
        captureDevice = dpPoleScope;
        useSdkCaptureSource = isPoleCameraSDK();
        captureRoleName = "PoleCamera";

        if (!isPoleCameraSDK() && dpPoleScope != nullptr)
        {
            double pixelsize, pixelsizX, pixelsizY;
            int maxX, maxY, bitDepth;
            indi_Client->getCCDBasicInfo(dpPoleScope, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
            cameraWidthMm = maxX * pixelsize / 1000.0;
            cameraHeightMm = maxY * pixelsize / 1000.0;
            cameraWidthMm = std::round(cameraWidthMm * 10.0) / 10.0;
            cameraHeightMm = std::round(cameraHeightMm * 10.0) / 10.0;
        }
        else if (isPoleCameraSDK() && sdkPoleScopeHandle != nullptr)
        {
            SdkCommand chipInfoCmd;
            chipInfoCmd.type = SdkCommandType::Custom;
            chipInfoCmd.name = "GetChipInfo";
            chipInfoCmd.payload = std::any();
            SdkResult chipInfoRes = SdkManager::instance().callByHandle(sdkPoleScopeHandle, chipInfoCmd);

            if (chipInfoRes.success)
            {
                try
                {
                    SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                    cameraWidthMm = chipInfo.chipWidthMM;
                    cameraHeightMm = chipInfo.chipHeightMM;
                }
                catch (const std::bad_any_cast &e)
                {
                    Logger::Log("initPolarAlignment | SDK GetChipInfo bad_any_cast: " + std::string(e.what()),
                                LogLevel::ERROR, DeviceType::MAIN);
                }
            }
            else
            {
                Logger::Log("initPolarAlignment | SDK GetChipInfo failed: " + chipInfoRes.message,
                            LogLevel::WARNING, DeviceType::MAIN);
            }
        }
    }

    if (cameraWidthMm <= 0 || cameraHeightMm <= 0)
    {
        Logger::Log("initPolarAlignment | Camera size parameters are invalid", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,Camera size parameters are invalid");
        return false;
    }

    currentPolarAlignmentCameraRole = role;
    polarGuiderSingleCapturePending = false;

    polarAlignment = new PolarAlignment(indi_Client, dpMount, captureDevice, useSdkCaptureSource, captureRoleName);
    if (polarAlignment == nullptr)
    {
        Logger::Log("initPolarAlignment | Failed to create PolarAlignment object", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,Failed to create PolarAlignment object");
        return false;
    }

    QObject::connect(polarAlignment, &PolarAlignment::requestCaptureForRole,
                     this, [this](const QString &cameraRole, int exposureMs)
                     {
                         const PolarAlignmentCameraRole requestedRole = parsePolarAlignmentCameraRole(cameraRole);
                         if (requestedRole == PolarAlignmentCameraRole::Guider)
                         {
                             this->startGuiderSingleCapture(exposureMs);
                         }
                         else if (requestedRole == PolarAlignmentCameraRole::PoleCamera)
                         {
                             this->startPoleCameraSingleCapture(exposureMs);
                         }
                         else
                         {
                             this->startMainCameraCapture(exposureMs);
                         }
                     },
                     Qt::QueuedConnection);

    PolarAlignmentConfig config;
    config.defaultExposureTime = 1000;
    config.recoveryExposureTime = 5000;
    config.raRotationAngle = 15.0;
    config.decRotationAngle = 15.0;
    config.maxRetryAttempts = 3;
    config.captureAndAnalysisTimeout = 30000;
    config.movementTimeout = 15000;
    config.maxAdjustmentAttempts = 3;
    config.adjustmentAngleReduction = 0.5;
    config.cameraWidth = cameraWidthMm;
    config.cameraHeight = cameraHeightMm;
    config.focalLength = selectedFocalLength;
    config.latitude = observatorylatitude;
    config.longitude = observatorylongitude;
    config.finalVerificationThreshold = 0.5;

    polarAlignment->setConfig(config);

    connect(polarAlignment, &PolarAlignment::stateChanged,
            [this](PolarAlignmentState state, QString message, int percentage)
            {
                qDebug() << "状态改变:" << static_cast<int>(state) << " 消息:" << message << " 进度:" << percentage;
                emit this->wsThread->sendMessageToClient(QString("PolarAlignmentState:") +
                                                        (polarAlignment->isRunning() ? "true" : "false") + ":" +
                                                        QString::number(static_cast<int>(state)) + ":" +
                                                        message + ":" +
                                                        QString::number(percentage));

                if (state == PolarAlignmentState::IDLE ||
                    state == PolarAlignmentState::COMPLETED ||
                    state == PolarAlignmentState::FAILED ||
                    state == PolarAlignmentState::USER_INTERVENTION)
                {
                    qunsetenv("QUARCS_POLAR_SOLVE_FIXED");
                    qunsetenv("QUARCS_POLAR_SOLVE_RA_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_DEC_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_RADIUS_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_SCALE_LOW_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_SCALE_HIGH_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_DEPTH");
                    if (indi_Client != nullptr && dpMount != nullptr)
                    {
                        indi_Client->setTelescopeTrackEnable(dpMount, true);

                        bool isTrack = false;
                        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
                        emit this->wsThread->sendMessageToClient(isTrack ? "TelescopeTrack:ON"
                                                                         : "TelescopeTrack:OFF");

                        Logger::Log("PolarAlignment: Telescope tracking restored after polar alignment",
                                    LogLevel::INFO, DeviceType::MAIN);
                    }
                    emit this->wsThread->sendMessageToClient("PolarAlignmentState:false:0:极轴校准已停止:0");
                }
            });

    connect(polarAlignment, &PolarAlignment::adjustmentGuideData,
            [this](double ra, double dec,
                   double ra0, double dec0, double ra1, double dec1,
                   double ra2, double dec2, double ra3, double dec3,
                   double targetRa, double targetDec,
                   double offsetRa, double offsetDec,
                   const QString &adjustmentRa, const QString &adjustmentDec,
                   double fakePolarRA, double fakePolarDEC,
                   double realPolarRA, double realPolarDEC)
            {
                QString logMsg = QString("PolarAlignmentAdjustmentGuideData:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11:%12:%13:%14:%15:%16:%17:%18:%19:%20")
                                     .arg(ra)
                                     .arg(dec)
                                     .arg(ra0)
                                     .arg(dec0)
                                     .arg(ra1)
                                     .arg(dec1)
                                     .arg(ra2)
                                     .arg(dec2)
                                     .arg(ra3)
                                     .arg(dec3)
                                     .arg(targetRa)
                                     .arg(targetDec)
                                     .arg(offsetRa)
                                     .arg(offsetDec)
                                     .arg(adjustmentRa)
                                     .arg(adjustmentDec)
                                     .arg(fakePolarRA)
                                     .arg(fakePolarDEC)
                                     .arg(realPolarRA)
                                     .arg(realPolarDEC);
                Logger::Log("目标点: " + std::to_string(targetRa) + ", " + std::to_string(targetDec), LogLevel::INFO, DeviceType::MAIN);
                Logger::Log("假极轴: " + std::to_string(fakePolarRA) + ", " + std::to_string(fakePolarDEC), LogLevel::INFO, DeviceType::MAIN);
                Logger::Log("真极轴: " + std::to_string(realPolarRA) + ", " + std::to_string(realPolarDEC), LogLevel::INFO, DeviceType::MAIN);
                Logger::Log(logMsg.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                emit this->wsThread->sendMessageToClient(logMsg);
            });

    connect(polarAlignment, &PolarAlignment::guidanceAdjustmentStepProgress,
            [this](GuidanceAdjustmentStep step, QString message, int starCount)
            {
                QString logMsg = QString("PolarAlignmentGuidanceStepProgress:%1:%2:%3")
                                     .arg(static_cast<int>(step))
                                     .arg(message)
                                     .arg(starCount);
                Logger::Log(logMsg.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                emit this->wsThread->sendMessageToClient(logMsg);
            });

    Logger::Log("initPolarAlignment | PolarAlignment initialized successfully, role=" +
                    std::string(polarRoleName(role)) +
                    ", focal=" + std::to_string(selectedFocalLength) +
                    ", size(mm)=" + std::to_string(cameraWidthMm) + "x" + std::to_string(cameraHeightMm),
                LogLevel::INFO, DeviceType::MAIN);
    return true;
}

bool MainWindow::initPoleMasterPolarAlignment()
{
    const bool mountConnected = (dpMount != nullptr) || isMountSDK();
    if (!mountConnected)
    {
        Logger::Log("initPoleMasterPolarAlignment | mount is not connected", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,Mount must be connected");
        return false;
    }
    if (!isPoleCameraConnected())
    {
        Logger::Log("initPoleMasterPolarAlignment | pole camera is not connected", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,PoleCamera is not connected");
        return false;
    }
    if (isMountSDK() && dpMount == nullptr)
    {
        Logger::Log("initPoleMasterPolarAlignment | SDK模式赤道仪暂不支持，请使用INDI模式", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,SDK模式赤道仪暂不支持");
        return false;
    }
    if (indi_Client == nullptr)
    {
        Logger::Log("initPoleMasterPolarAlignment | indi_Client is nullptr", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,indi_Client is nullptr");
        return false;
    }

    double poleFocal = 0.0;
    std::unordered_map<std::string, std::string> clientConfig;
    Tools::readClientSettings("config/config.ini", clientConfig);
    auto it = clientConfig.find("PoleCameraFocalLength");
    if (it != clientConfig.end())
    {
        bool ok = false;
        const double v = QString::fromStdString(it->second).trimmed().toDouble(&ok);
        if (ok && v > 0.0) poleFocal = v;
    }
    if (poleFocal <= 0.0)
    {
        Logger::Log("initPoleMasterPolarAlignment | PoleCameraFocalLength is not set", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,pole camera focal length is not set");
        return false;
    }

    double cameraWidthMm = 0.0;
    double cameraHeightMm = 0.0;
    if (!isPoleCameraSDK() && dpPoleScope != nullptr)
    {
        double pixelsize, pixelsizX, pixelsizY;
        int maxX, maxY, bitDepth;
        indi_Client->getCCDBasicInfo(dpPoleScope, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
        cameraWidthMm = maxX * pixelsize / 1000.0;
        cameraHeightMm = maxY * pixelsize / 1000.0;
        cameraWidthMm = std::round(cameraWidthMm * 10.0) / 10.0;
        cameraHeightMm = std::round(cameraHeightMm * 10.0) / 10.0;
    }
    else if (isPoleCameraSDK() && sdkPoleScopeHandle != nullptr)
    {
        SdkCommand chipInfoCmd;
        chipInfoCmd.type = SdkCommandType::Custom;
        chipInfoCmd.name = "GetChipInfo";
        chipInfoCmd.payload = std::any();
        SdkResult chipInfoRes = SdkManager::instance().callByHandle(sdkPoleScopeHandle, chipInfoCmd);

        if (chipInfoRes.success)
        {
            try
            {
                SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                cameraWidthMm = chipInfo.chipWidthMM;
                cameraHeightMm = chipInfo.chipHeightMM;
            }
            catch (const std::bad_any_cast &e)
            {
                Logger::Log("initPoleMasterPolarAlignment | SDK GetChipInfo bad_any_cast: " + std::string(e.what()),
                            LogLevel::ERROR, DeviceType::MAIN);
            }
        }
        else
        {
            Logger::Log("initPoleMasterPolarAlignment | SDK GetChipInfo failed: " + chipInfoRes.message,
                        LogLevel::WARNING, DeviceType::MAIN);
        }
    }

    if (cameraWidthMm <= 0.0 || cameraHeightMm <= 0.0)
    {
        Logger::Log("initPoleMasterPolarAlignment | Camera size parameters are invalid", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,Camera size parameters are invalid");
        return false;
    }

    currentPolarAlignmentCameraRole = PolarAlignmentCameraRole::PoleCamera;
    polarGuiderSingleCapturePending = false;
    poleMasterPolarAlignment = new PoleMasterPolarAlignment(indi_Client,
                                                            dpMount,
                                                            dpPoleScope,
                                                            isPoleCameraSDK(),
                                                            false,
                                                            QString(),
                                                            this);
    if (poleMasterPolarAlignment == nullptr)
    {
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,Failed to create PoleMaster object");
        return false;
    }

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::requestCaptureForRole,
            this, [this](const QString &, int exposureMs)
            {
                this->startPoleCameraSingleCapture(exposureMs);
            },
            Qt::QueuedConnection);

    PoleMasterAlignmentConfig cfg;
    cfg.defaultExposureTime = 1000;
    cfg.guidanceExposureTime = 1000;
    cfg.captureTimeoutMs = 10000;
    cfg.movementTimeoutMs = 60000;
    cfg.focalLength = static_cast<int>(std::lround(poleFocal));
    cfg.cameraWidth = cameraWidthMm;
    cfg.cameraHeight = cameraHeightMm;
    cfg.raRotationAngle = 35.0;
    cfg.doneThresholdArcsec = 30.0;
    cfg.stableFrameRequirement = 3;
    cfg.latitude = observatorylatitude;
    cfg.solveSearchRadiusDeg = 5.0;
    cfg.solveIndexFilePath = "index-tycho2-09.littleendian.fits";
    poleMasterPolarAlignment->setConfig(cfg);

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::stateChanged,
            [this](PoleMasterAlignmentState state, QString message, int progress, bool running)
            {
                const QString stateMsg = QString("PoleMasterAlignmentState:%1:%2:%3:%4")
                                             .arg(running ? "true" : "false")
                                             .arg(static_cast<int>(state))
                                             .arg(message)
                                             .arg(progress);
                emit this->wsThread->sendMessageToClient(stateMsg);

                if (state == PoleMasterAlignmentState::IDLE ||
                    state == PoleMasterAlignmentState::COMPLETED ||
                    state == PoleMasterAlignmentState::FAILED)
                {
                    qunsetenv("QUARCS_POLAR_SOLVE_FIXED");
                    qunsetenv("QUARCS_POLAR_SOLVE_RA_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_DEC_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_RADIUS_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_SCALE_LOW_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_SCALE_HIGH_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_DEPTH");
                    if (indi_Client != nullptr && dpMount != nullptr)
                    {
                        indi_Client->setTelescopeTrackEnable(dpMount, true);
                        bool isTrack = false;
                        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
                        emit this->wsThread->sendMessageToClient(isTrack ? "TelescopeTrack:ON" : "TelescopeTrack:OFF");
                    }
                    emit this->wsThread->sendMessageToClient("PolarAlignmentState:false:0:电子极轴镜校准已停止:0");
                }
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::guideData,
            [this](int imageW, int imageH,
                   double axisX, double axisY,
                   double poleX, double poleY,
                   double errorPx, double errorArcsec,
                   const QString &frameId,
                   const QString &hint)
            {
                const QString msg = QString("PoleMasterAlignmentGuideData:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10")
                                        .arg(imageW)
                                        .arg(imageH)
                                        .arg(axisX, 0, 'f', 3)
                                        .arg(axisY, 0, 'f', 3)
                                        .arg(poleX, 0, 'f', 3)
                                        .arg(poleY, 0, 'f', 3)
                                        .arg(errorPx, 0, 'f', 3)
                                        .arg(errorArcsec, 0, 'f', 2)
                                        .arg(frameId)
                                        .arg(hint);
                Logger::Log(msg.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                emit this->wsThread->sendMessageToClient(msg);
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::frameData,
            [this](const QString &fileName, int imageW, int imageH, const QString &frameId)
            {
                const QString sourcePath = QDir(QString::fromStdString(vueDirectoryPath)).filePath(fileName);
                const QString targetPath = QDir(QString::fromStdString(vueImagePath)).filePath(fileName);
                if (QFileInfo::exists(sourcePath) && sourcePath != targetPath && !QFileInfo::exists(targetPath))
                {
                    QFile::link(sourcePath, targetPath);
                    if (!QFileInfo::exists(targetPath))
                        QFile::copy(sourcePath, targetPath);
                }
                emit this->wsThread->sendMessageToClient(QString("PoleMasterAlignmentFrameData:%1:%2:%3:%4")
                                                             .arg(fileName)
                                                             .arg(imageW)
                                                             .arg(imageH)
                                                             .arg(frameId));
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::overlayData,
            [this](const QString &jsonPayload)
            {
                emit this->wsThread->sendMessageToClient("PoleMasterAlignmentOverlayData:" + jsonPayload);
            });

    Logger::Log("initPoleMasterPolarAlignment | initialized successfully, focal=" +
                    std::to_string(static_cast<int>(std::lround(poleFocal))) +
                    ", size(mm)=" + std::to_string(cameraWidthMm) + "x" + std::to_string(cameraHeightMm),
                LogLevel::INFO, DeviceType::MAIN);
    return true;
}

bool MainWindow::initPoleMasterAlignmentSimulation()
{
    currentPolarAlignmentCameraRole = PolarAlignmentCameraRole::PoleCamera;

    QString imageRoot = QString::fromStdString(vueImagePath);
    if (imageRoot.trimmed().isEmpty())
        imageRoot = QString::fromStdString(vueDirectoryPath);
    imageRoot = QDir::cleanPath(imageRoot);
    QDir dir(imageRoot);
    if (!dir.exists() && !dir.mkpath("."))
    {
        Logger::Log("initPoleMasterAlignmentSimulation | image root unavailable, stop simulation. root=" +
                        imageRoot.toStdString(),
                    LogLevel::ERROR,
                    DeviceType::MAIN);
        return false;
    }

    poleMasterPolarAlignment = new PoleMasterPolarAlignment(indi_Client,
                                                            dpMount,
                                                            dpPoleScope,
                                                            isPoleCameraSDK(),
                                                            true,
                                                            imageRoot,
                                                            this);
    if (poleMasterPolarAlignment == nullptr)
    {
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera simulation,Failed to create simulation object");
        return false;
    }

    PoleMasterAlignmentConfig cfg;
    cfg.defaultExposureTime = 1000;
    cfg.guidanceExposureTime = 1000;
    cfg.captureTimeoutMs = 15000;
    cfg.movementTimeoutMs = 2000;
    cfg.focalLength = 100;
    cfg.cameraWidth = 21.0;
    cfg.cameraHeight = 15.75;
    cfg.raRotationAngle = 35.0;
    cfg.doneThresholdArcsec = 30.0;
    cfg.stableFrameRequirement = 3;
    cfg.latitude = observatorylatitude;
    cfg.solveSearchRadiusDeg = 5.0;
    cfg.solveIndexFilePath = "index-tycho2-12.littleendian.fits,index-tycho2-13.littleendian.fits,index-tycho2-14.littleendian.fits,index-tycho2-15.littleendian.fits";
    poleMasterPolarAlignment->setConfig(cfg);

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::stateChanged,
            [this](PoleMasterAlignmentState state, QString message, int progress, bool running)
            {
                const QString stateMsg = QString("PoleMasterAlignmentState:%1:%2:%3:%4")
                                             .arg(running ? "true" : "false")
                                             .arg(static_cast<int>(state))
                                             .arg(message)
                                             .arg(progress);
                emit this->wsThread->sendMessageToClient(stateMsg);

                if (state == PoleMasterAlignmentState::IDLE ||
                    state == PoleMasterAlignmentState::COMPLETED ||
                    state == PoleMasterAlignmentState::FAILED)
                {
                    emit this->wsThread->sendMessageToClient("PolarAlignmentState:false:0:电子极轴镜模拟校准已停止:0");
                }
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::guideData,
            [this](int imageW, int imageH,
                   double axisX, double axisY,
                   double poleX, double poleY,
                   double errorPx, double errorArcsec,
                   const QString &frameId,
                   const QString &hint)
            {
                const QString msg = QString("PoleMasterAlignmentGuideData:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10")
                                        .arg(imageW)
                                        .arg(imageH)
                                        .arg(axisX, 0, 'f', 3)
                                        .arg(axisY, 0, 'f', 3)
                                        .arg(poleX, 0, 'f', 3)
                                        .arg(poleY, 0, 'f', 3)
                                        .arg(errorPx, 0, 'f', 3)
                                        .arg(errorArcsec, 0, 'f', 2)
                                        .arg(frameId)
                                        .arg(hint);
                Logger::Log(msg.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                emit this->wsThread->sendMessageToClient(msg);
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::frameData,
            [this](const QString &fileName, int imageW, int imageH, const QString &frameId)
            {
                const QString sourcePath = QDir(QString::fromStdString(vueDirectoryPath)).filePath(fileName);
                const QString targetPath = QDir(QString::fromStdString(vueImagePath)).filePath(fileName);
                if (QFileInfo::exists(sourcePath) && sourcePath != targetPath && !QFileInfo::exists(targetPath))
                {
                    QFile::link(sourcePath, targetPath);
                    if (!QFileInfo::exists(targetPath))
                        QFile::copy(sourcePath, targetPath);
                }
                emit this->wsThread->sendMessageToClient(QString("PoleMasterAlignmentFrameData:%1:%2:%3:%4")
                                                             .arg(fileName)
                                                             .arg(imageW)
                                                             .arg(imageH)
                                                             .arg(frameId));
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::overlayData,
            [this](const QString &jsonPayload)
            {
                emit this->wsThread->sendMessageToClient("PoleMasterAlignmentOverlayData:" + jsonPayload);
            });

    Logger::Log("initPoleMasterAlignmentSimulation | initialized successfully, imageRoot=" +
                    imageRoot.toStdString(),
                LogLevel::INFO,
                DeviceType::MAIN);
    return true;
}
