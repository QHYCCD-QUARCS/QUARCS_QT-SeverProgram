#include "mainwindow_command_support.h"

namespace {

std::string formatBayerPhaseDebug(const QString& baseCfa, int cfaOffsetX, int cfaOffsetY,
                                  int frameStartX, int frameStartY, const QString& resolvedCfa)
{
    const int totalShiftX = cfaOffsetX + frameStartX;
    const int totalShiftY = cfaOffsetY + frameStartY;
    return "baseCFA=" + baseCfa.toStdString() +
           ", cfaOffset=(" + std::to_string(cfaOffsetX) + "," + std::to_string(cfaOffsetY) + ")" +
           ", frameStart=(" + std::to_string(frameStartX) + "," + std::to_string(frameStartY) + ")" +
           ", totalShift=(" + std::to_string(totalShiftX) + "," + std::to_string(totalShiftY) + ")" +
           ", parity=(" + std::to_string(totalShiftX & 1) + "," + std::to_string(totalShiftY & 1) + ")" +
           ", resolvedCFA=" + resolvedCfa.toStdString();
}

} // namespace

void MainWindow::FocusingLooping()
{
    Logger::Log("FocusingLooping start ...", LogLevel::DEBUG, DeviceType::FOCUSER);

    bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                            systemdevicelist.system_devices[20].isSDKConnect &&
                            sdkMainCameraHandle != nullptr);

    if (!isMainCameraSDK && dpMainCamera == NULL)
    {
        Logger::Log("FocusingLooping | Main Camera not available (both SDK and INDI are NULL)", LogLevel::WARNING, DeviceType::FOCUSER);
        return;
    }

    isSavePngSuccess = false;

    glIsFocusingLooping = true;
    Logger::Log("FocusingLooping | glIsFocusingLooping:" + std::to_string(glIsFocusingLooping), LogLevel::DEBUG, DeviceType::FOCUSER);

    if (glMainCameraStatu != "Exposuring")
    {
        double expTime_sec = (double)glExpTime / 1000;

        glMainCameraStatu = "Exposuring";
        Logger::Log("FocusingLooping | glMainCameraStatu:" + glMainCameraStatu.toStdString(), LogLevel::DEBUG, DeviceType::FOCUSER);

        if (isMainCameraSDK && (glMainCCDSizeX <= 0 || glMainCCDSizeY <= 0))
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
                    emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
                    Logger::Log("FocusingLooping | SDK ChipInfo loaded on-demand: " +
                                    std::to_string(glMainCCDSizeX) + "x" + std::to_string(glMainCCDSizeY),
                                LogLevel::DEBUG, DeviceType::FOCUSER);
                }
                catch (const std::bad_any_cast &)
                {
                }
            }
        }
        if (isMainCameraSDK && (glMainCCDSizeX <= 0 || glMainCCDSizeY <= 0))
        {
            Logger::Log("FocusingLooping | SDK main camera size not initialized (glMainCCDSizeX/Y=0), abort focusing loop.",
                        LogLevel::ERROR, DeviceType::FOCUSER);
            glMainCameraStatu = "IDLE";
            glIsFocusingLooping = false;
            isFocusLoopShooting = false;
            emit wsThread->sendMessageToClient("startFocusLoopFailed:SDK MainCamera size not initialized");
            return;
        }

        int effMinX = 0;
        int effMinY = 0;
        int effW = glMainCCDSizeX;
        int effH = glMainCCDSizeY;
        if (isMainCameraSDK && sdkMainCameraHandle != nullptr)
        {
            if (sdkMainEffectiveAreaCacheHandle != sdkMainCameraHandle)
            {
                sdkMainEffectiveAreaCacheValid = false;
                sdkMainEffectiveAreaCacheHandle = sdkMainCameraHandle;
            }

            if (!sdkMainEffectiveAreaCacheValid)
            {
                SdkCommand effCmd;
                effCmd.type = SdkCommandType::Custom;
                effCmd.name = "GetEffectiveArea";
                effCmd.payload = std::any();
                SdkResult effRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, effCmd);
                if (effRes.success)
                {
                    try
                    {
                        SdkAreaInfo eff = std::any_cast<SdkAreaInfo>(effRes.payload);
                        if (eff.sizeX > 0U && eff.sizeY > 0U)
                        {
                            sdkMainEffectiveAreaMinX = static_cast<int>(eff.startX);
                            sdkMainEffectiveAreaMinY = static_cast<int>(eff.startY);
                            sdkMainEffectiveAreaWidth = static_cast<int>(eff.sizeX);
                            sdkMainEffectiveAreaHeight = static_cast<int>(eff.sizeY);
                            sdkMainEffectiveAreaCacheValid = true;
                            Logger::Log("FocusingLooping | cached GetEffectiveArea: start=(" +
                                            std::to_string(sdkMainEffectiveAreaMinX) + "," +
                                            std::to_string(sdkMainEffectiveAreaMinY) + ") size=(" +
                                            std::to_string(sdkMainEffectiveAreaWidth) + "x" +
                                            std::to_string(sdkMainEffectiveAreaHeight) + ")",
                                        LogLevel::DEBUG, DeviceType::FOCUSER);
                        }
                    }
                    catch (const std::bad_any_cast &)
                    {
                    }
                }
            }

            if (sdkMainEffectiveAreaCacheValid)
            {
                effMinX = sdkMainEffectiveAreaMinX;
                effMinY = sdkMainEffectiveAreaMinY;
                effW = sdkMainEffectiveAreaWidth;
                effH = sdkMainEffectiveAreaHeight;
                Logger::Log("FocusingLooping | effective area baseline: start=(" + std::to_string(effMinX) + "," +
                                std::to_string(effMinY) + ") size=(" + std::to_string(effW) + "x" +
                                std::to_string(effH) + ")",
                            LogLevel::DEBUG, DeviceType::FOCUSER);
            }
        }

        int roiBox = BoxSideLength;
        if (roiBox <= 0)
            roiBox = 300;
        {
            const int capW = std::min(glMainCCDSizeX, effW);
            const int capH = std::min(glMainCCDSizeY, effH);
            if (roiBox > capW)
                roiBox = capW;
            if (roiBox > capH)
                roiBox = capH;
        }
        if (roiBox < 2)
            roiBox = 2;
        QSize ROI{roiBox, roiBox};

        Logger::Log("FocusingLooping |当前ROI值 ROI_x:" + std::to_string(roiAndFocuserInfo["ROI_x"]) + ", ROI_y:" + std::to_string(roiAndFocuserInfo["ROI_y"]), LogLevel::DEBUG, DeviceType::FOCUSER);
        int cameraX = static_cast<int>(roiAndFocuserInfo["ROI_x"]);
        int cameraY = static_cast<int>(roiAndFocuserInfo["ROI_y"]);

        if (cameraX % 2 != 0)
            cameraX += 1;
        if (cameraY % 2 != 0)
            cameraY += 1;

        const bool tileModeActive = (isStagingImage && !SavedImage.empty());
        const int roiCoordScale = tileModeActive ? 1 : std::max(1, glMainCameraBinning);

        int scaledX = cameraX * roiCoordScale;
        int scaledY = cameraY * roiCoordScale;
        if (scaledX < 0)
            scaledX = 0;
        if (scaledY < 0)
            scaledY = 0;
        ROI = QSize(roiBox, roiBox);
        if (scaledX > effW - ROI.width())
            scaledX = effW - ROI.width();
        if (scaledY > effH - ROI.height())
            scaledY = effH - ROI.height();

        if (scaledX <= effW - ROI.width() && scaledY <= effH - ROI.height())
        {
            Logger::Log("FocusingLooping | set Camera ROI x:" + std::to_string(cameraX) + ", y:" + std::to_string(cameraY) + ", width:" + std::to_string(roiBox) + ", height:" + std::to_string(roiBox), LogLevel::DEBUG, DeviceType::FOCUSER);
            if (roiCoordScale > 0)
            {
                roiAndFocuserInfo["ROI_x"] = static_cast<double>(scaledX) / roiCoordScale;
                roiAndFocuserInfo["ROI_y"] = static_cast<double>(scaledY) / roiCoordScale;
            }

            if (isMainCameraSDK)
            {
                {
                    SdkCommand cancelCmd;
                    cancelCmd.type = SdkCommandType::Custom;
                    cancelCmd.name = "CancelExposure";
                    cancelCmd.payload = std::any();
                    SdkManager::instance().callByHandle(sdkMainCameraHandle, cancelCmd);
                }

                int roiW = roiBox;
                int roiH = roiBox;
                if (roiW % 2 != 0)
                    roiW += 1;
                if (roiH % 2 != 0)
                    roiH += 1;
                if (roiW > effW)
                    roiW = effW;
                if (roiH > effH)
                    roiH = effH;
                if ((effMinX + scaledX) % 2 != 0)
                    scaledX = std::max(0, scaledX - 1);
                if ((effMinY + scaledY) % 2 != 0)
                    scaledY = std::max(0, scaledY - 1);
                if (scaledX > effW - roiW)
                    scaledX = effW - roiW;
                if (scaledY > effH - roiH)
                    scaledY = effH - roiH;
                if (scaledX < 0)
                    scaledX = 0;
                if (scaledY < 0)
                    scaledY = 0;

                const int sensorStartX = effMinX + scaledX;
                const int sensorStartY = effMinY + scaledY;

                lastFocusExposureSnapshotValid = true;
                lastFocusExposureScaledX = scaledX;
                lastFocusExposureScaledY = scaledY;
                lastFocusExposureEffMinX = effMinX;
                lastFocusExposureEffMinY = effMinY;
                lastFocusExposureRoiCoordScale = std::max(1, roiCoordScale);
                lastFocusExposureRoiW = roiW;
                lastFocusExposureRoiH = roiH;

                const QString focusResolvedCfa = resolveFrameCfa(sensorStartX, sensorStartY);
                Logger::Log("FocusingLooping | ROI Bayer debug | previewROI=(" + std::to_string(cameraX) + "," + std::to_string(cameraY) +
                                ") scaled=(" + std::to_string(scaledX) + "," + std::to_string(scaledY) + ")" +
                                ", roiCoordScale=" + std::to_string(roiCoordScale) +
                                ", effRect=(" + std::to_string(effMinX) + "," + std::to_string(effMinY) + "," +
                                std::to_string(effW) + "x" + std::to_string(effH) + ")" +
                                ", roiSize=" + std::to_string(roiW) + "x" + std::to_string(roiH) +
                                ", sensorParity=(" + std::to_string(sensorStartX & 1) + "," + std::to_string(sensorStartY & 1) + ")" +
                                ", " + formatBayerPhaseDebug(MainCameraCFA, MainCameraCFAOffsetX, MainCameraCFAOffsetY,
                                                             sensorStartX, sensorStartY, focusResolvedCfa),
                            LogLevel::INFO, DeviceType::FOCUSER);

                SdkAreaInfo roi;
                roi.startX = static_cast<unsigned int>(sensorStartX);
                roi.startY = static_cast<unsigned int>(sensorStartY);
                roi.sizeX = static_cast<unsigned int>(roiW);
                roi.sizeY = static_cast<unsigned int>(roiH);

                SdkCommand setRoiCmd;
                setRoiCmd.type = SdkCommandType::Custom;
                setRoiCmd.name = "SetResolution";
                setRoiCmd.payload = roi;
                Logger::Log("FocusingLooping | SDK SetResolution | start=(" + std::to_string(roi.startX) + "," + std::to_string(roi.startY) + ") size=(" + std::to_string(roi.sizeX) + "x" + std::to_string(roi.sizeY) + ")",
                            LogLevel::DEBUG, DeviceType::FOCUSER);
                SdkResult roiRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setRoiCmd);
                if (!roiRes.success)
                {
                    Logger::Log("FocusingLooping | SDK SetResolution failed: " + roiRes.message,
                                LogLevel::ERROR, DeviceType::FOCUSER);
                    glMainCameraStatu = "IDLE";
                    glIsFocusingLooping = false;
                    isFocusLoopShooting = false;
                    emit wsThread->sendMessageToClient("startFocusLoopFailed:SDK SetResolution failed");
                    return;
                }

                SdkCommand setExpCmd;
                setExpCmd.type = SdkCommandType::Custom;
                setExpCmd.name = "SetExposure";
                setExpCmd.payload = expTime_sec * 1000000.0;
                SdkResult setExpRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setExpCmd);
                if (!setExpRes.success)
                {
                    Logger::Log("FocusingLooping | SDK SetExposure failed: " + setExpRes.message,
                                LogLevel::ERROR, DeviceType::FOCUSER);
                }

                SdkCommand startExpCmd;
                startExpCmd.type = SdkCommandType::Custom;
                startExpCmd.name = "StartSingleExposure";
                startExpCmd.payload = std::any();
                SdkResult startExpRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, startExpCmd);
                if (!startExpRes.success)
                {
                    Logger::Log("FocusingLooping | SDK StartSingleExposure failed: " + startExpRes.message,
                                LogLevel::ERROR, DeviceType::FOCUSER);
                    glMainCameraStatu = "IDLE";
                    return;
                }
                Logger::Log("FocusingLooping | SDK StartSingleExposure success, will check image after exposure time",
                            LogLevel::DEBUG, DeviceType::FOCUSER);

                int expTime_ms = static_cast<int>(expTime_sec * 1000);
                sdkExposureStartTime = QDateTime::currentMSecsSinceEpoch();
                sdkExposureExpectedDuration = std::max(1, expTime_ms);
                sdkExposureIsROI = true;

                sdkExposureTimer->start(std::max(1, expTime_ms));
                Logger::Log("FocusingLooping | SDK exposure timer started, will check after " + std::to_string(expTime_ms) + "ms",
                            LogLevel::DEBUG, DeviceType::FOCUSER);
            }
            else
            {
                const int indiX = effMinX + scaledX;
                const int indiY = effMinY + scaledY;
                lastFocusExposureSnapshotValid = true;
                lastFocusExposureScaledX = scaledX;
                lastFocusExposureScaledY = scaledY;
                lastFocusExposureEffMinX = effMinX;
                lastFocusExposureEffMinY = effMinY;
                lastFocusExposureRoiCoordScale = std::max(1, roiCoordScale);
                lastFocusExposureRoiW = BoxSideLength;
                lastFocusExposureRoiH = BoxSideLength;

                const QString focusResolvedCfa = resolveFrameCfa(indiX, indiY);
                Logger::Log("FocusingLooping | ROI Bayer debug | previewROI=(" + std::to_string(cameraX) + "," + std::to_string(cameraY) +
                                ") scaled=(" + std::to_string(scaledX) + "," + std::to_string(scaledY) + ")" +
                                ", roiCoordScale=" + std::to_string(roiCoordScale) +
                                ", effRect=(" + std::to_string(effMinX) + "," + std::to_string(effMinY) + "," +
                                std::to_string(effW) + "x" + std::to_string(effH) + ")" +
                                ", roiSize=" + std::to_string(BoxSideLength) + "x" + std::to_string(BoxSideLength) +
                                ", sensorParity=(" + std::to_string(indiX & 1) + "," + std::to_string(indiY & 1) + ")" +
                                ", " + formatBayerPhaseDebug(MainCameraCFA, MainCameraCFAOffsetX, MainCameraCFAOffsetY,
                                                             indiX, indiY, focusResolvedCfa),
                            LogLevel::INFO, DeviceType::FOCUSER);

                Logger::Log("FocusingLooping | INDI setCCDFrameInfo | (" + std::to_string(indiX) + "," + std::to_string(indiY) + ") " + std::to_string(BoxSideLength) + "x" + std::to_string(BoxSideLength),
                            LogLevel::DEBUG, DeviceType::FOCUSER);
                indi_Client->setCCDFrameInfo(dpMainCamera, indiX, indiY, BoxSideLength, BoxSideLength);
                indi_Client->takeExposure(dpMainCamera, expTime_sec);
                Logger::Log("FocusingLooping | INDI takeExposure, expTime_sec:" + std::to_string(expTime_sec), LogLevel::DEBUG, DeviceType::FOCUSER);
            }
        }
        else
        {
            Logger::Log("FocusingLooping | Too close to the edge, please reselect the area.", LogLevel::WARNING, DeviceType::FOCUSER);
            if (scaledX + ROI.width() > effW)
                scaledX = effW - ROI.width();
            if (scaledY + ROI.height() > effH)
                scaledY = effH - ROI.height();

            if (roiCoordScale > 0)
            {
                roiAndFocuserInfo["ROI_x"] = static_cast<double>(scaledX) / roiCoordScale;
                roiAndFocuserInfo["ROI_y"] = static_cast<double>(scaledY) / roiCoordScale;
            }

            if (isMainCameraSDK)
            {
                {
                    SdkCommand cancelCmd;
                    cancelCmd.type = SdkCommandType::Custom;
                    cancelCmd.name = "CancelExposure";
                    cancelCmd.payload = std::any();
                    SdkManager::instance().callByHandle(sdkMainCameraHandle, cancelCmd);
                }

                int roiW = ROI.width();
                int roiH = ROI.height();
                if (roiW % 2 != 0)
                    roiW += 1;
                if (roiH % 2 != 0)
                    roiH += 1;
                if (roiW > effW)
                    roiW = effW;
                if (roiH > effH)
                    roiH = effH;
                if ((effMinX + scaledX) % 2 != 0)
                    scaledX = std::max(0, scaledX - 1);
                if ((effMinY + scaledY) % 2 != 0)
                    scaledY = std::max(0, scaledY - 1);
                if (scaledX > effW - roiW)
                    scaledX = effW - roiW;
                if (scaledY > effH - roiH)
                    scaledY = effH - roiH;
                if (scaledX < 0)
                    scaledX = 0;
                if (scaledY < 0)
                    scaledY = 0;

                const int sensorStartXEdge = effMinX + scaledX;
                const int sensorStartYEdge = effMinY + scaledY;

                lastFocusExposureSnapshotValid = true;
                lastFocusExposureScaledX = scaledX;
                lastFocusExposureScaledY = scaledY;
                lastFocusExposureEffMinX = effMinX;
                lastFocusExposureEffMinY = effMinY;
                lastFocusExposureRoiCoordScale = std::max(1, roiCoordScale);
                lastFocusExposureRoiW = roiW;
                lastFocusExposureRoiH = roiH;

                const QString focusResolvedCfa = resolveFrameCfa(sensorStartXEdge, sensorStartYEdge);
                Logger::Log("FocusingLooping | ROI Bayer debug(edge) | previewROI=(" + std::to_string(cameraX) + "," + std::to_string(cameraY) +
                                ") scaled=(" + std::to_string(scaledX) + "," + std::to_string(scaledY) + ")" +
                                ", roiCoordScale=" + std::to_string(roiCoordScale) +
                                ", effRect=(" + std::to_string(effMinX) + "," + std::to_string(effMinY) + "," +
                                std::to_string(effW) + "x" + std::to_string(effH) + ")" +
                                ", roiSize=" + std::to_string(roiW) + "x" + std::to_string(roiH) +
                                ", sensorParity=(" + std::to_string(sensorStartXEdge & 1) + "," + std::to_string(sensorStartYEdge & 1) + ")" +
                                ", " + formatBayerPhaseDebug(MainCameraCFA, MainCameraCFAOffsetX, MainCameraCFAOffsetY,
                                                             sensorStartXEdge, sensorStartYEdge, focusResolvedCfa),
                            LogLevel::INFO, DeviceType::FOCUSER);

                SdkAreaInfo roi;
                roi.startX = static_cast<unsigned int>(sensorStartXEdge);
                roi.startY = static_cast<unsigned int>(sensorStartYEdge);
                roi.sizeX = static_cast<unsigned int>(roiW);
                roi.sizeY = static_cast<unsigned int>(roiH);

                SdkCommand setRoiCmd;
                setRoiCmd.type = SdkCommandType::Custom;
                setRoiCmd.name = "SetResolution";
                setRoiCmd.payload = roi;
                Logger::Log("FocusingLooping | SDK SetResolution | start=(" + std::to_string(roi.startX) + "," + std::to_string(roi.startY) + ") size=(" + std::to_string(roi.sizeX) + "x" + std::to_string(roi.sizeY) + ")",
                            LogLevel::DEBUG, DeviceType::FOCUSER);
                SdkResult roiRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setRoiCmd);
                if (!roiRes.success)
                {
                    Logger::Log("FocusingLooping | SDK SetResolution failed: " + roiRes.message,
                                LogLevel::ERROR, DeviceType::FOCUSER);
                    glMainCameraStatu = "IDLE";
                    glIsFocusingLooping = false;
                    isFocusLoopShooting = false;
                    emit wsThread->sendMessageToClient("startFocusLoopFailed:SDK SetResolution failed");
                    return;
                }

                SdkCommand setExpCmd;
                setExpCmd.type = SdkCommandType::Custom;
                setExpCmd.name = "SetExposure";
                setExpCmd.payload = expTime_sec * 1000000.0;
                SdkManager::instance().callByHandle(sdkMainCameraHandle, setExpCmd);

                SdkCommand startExpCmd;
                startExpCmd.type = SdkCommandType::Custom;
                startExpCmd.name = "StartSingleExposure";
                startExpCmd.payload = std::any();
                SdkResult startExpRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, startExpCmd);
                if (!startExpRes.success)
                {
                    Logger::Log("FocusingLooping | SDK StartSingleExposure failed: " + startExpRes.message,
                                LogLevel::ERROR, DeviceType::FOCUSER);
                    glMainCameraStatu = "IDLE";
                    return;
                }

                int expTime_ms = static_cast<int>(expTime_sec * 1000);
                sdkExposureStartTime = QDateTime::currentMSecsSinceEpoch();
                sdkExposureExpectedDuration = std::max(1, expTime_ms);
                sdkExposureIsROI = true;

                sdkExposureTimer->start(std::max(1, expTime_ms));
                Logger::Log("FocusingLooping | SDK exposure timer started (edge adjusted), will check after " + std::to_string(expTime_ms) + "ms",
                            LogLevel::DEBUG, DeviceType::FOCUSER);
            }
            else
            {
                const int indiXe = effMinX + scaledX;
                const int indiYe = effMinY + scaledY;
                lastFocusExposureSnapshotValid = true;
                lastFocusExposureScaledX = scaledX;
                lastFocusExposureScaledY = scaledY;
                lastFocusExposureEffMinX = effMinX;
                lastFocusExposureEffMinY = effMinY;
                lastFocusExposureRoiCoordScale = std::max(1, roiCoordScale);
                lastFocusExposureRoiW = ROI.width();
                lastFocusExposureRoiH = ROI.height();

                const QString focusResolvedCfa = resolveFrameCfa(indiXe, indiYe);
                Logger::Log("FocusingLooping | ROI Bayer debug(edge) | previewROI=(" + std::to_string(cameraX) + "," + std::to_string(cameraY) +
                                ") scaled=(" + std::to_string(scaledX) + "," + std::to_string(scaledY) + ")" +
                                ", roiCoordScale=" + std::to_string(roiCoordScale) +
                                ", effRect=(" + std::to_string(effMinX) + "," + std::to_string(effMinY) + "," +
                                std::to_string(effW) + "x" + std::to_string(effH) + ")" +
                                ", roiSize=" + std::to_string(ROI.width()) + "x" + std::to_string(ROI.height()) +
                                ", sensorParity=(" + std::to_string(indiXe & 1) + "," + std::to_string(indiYe & 1) + ")" +
                                ", " + formatBayerPhaseDebug(MainCameraCFA, MainCameraCFAOffsetX, MainCameraCFAOffsetY,
                                                             indiXe, indiYe, focusResolvedCfa),
                            LogLevel::INFO, DeviceType::FOCUSER);

                Logger::Log("FocusingLooping | INDI setCCDFrameInfo | (" + std::to_string(indiXe) + "," + std::to_string(indiYe) + ") " + std::to_string(ROI.width()) + "x" + std::to_string(ROI.height()),
                            LogLevel::DEBUG, DeviceType::FOCUSER);
                indi_Client->setCCDFrameInfo(dpMainCamera, indiXe, indiYe, ROI.width(), ROI.height());
                indi_Client->takeExposure(dpMainCamera, expTime_sec);
            }
        }
    }
    else
    {
        emit wsThread->sendMessageToClient("startFocusLoopFailed:Wait Take Picture Finish!");
    }
    Logger::Log("FocusingLooping finished.", LogLevel::DEBUG, DeviceType::FOCUSER);
}

void MainWindow::focusLoopShooting(bool isLoop)
{
    if (isLoop)
    {
        isFocusLoopShooting = true;
        if (glMainCameraStatu == "Exposuring")
        {
            emit wsThread->sendMessageToClient("startFocusLoopFailed:Wait Take Picture Finish!");
            isFocusLoopShooting = false;
            return;
        }
        if (!isMainCameraConnected())
        {
            emit wsThread->sendMessageToClient("startFocusLoopFailed:MainCamera is not connected!");
            isFocusLoopShooting = false;
            return;
        }
        FocusingLooping();
    }
    else
    {
        isFocusLoopShooting = false;
        hasPendingRoiUpdate = false;
        glIsFocusingLooping = false;

        bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                                systemdevicelist.system_devices[20].isSDKConnect &&
                                sdkMainCameraHandle != nullptr);

        if (isMainCameraSDK)
        {
            if (sdkMainCameraHandle != nullptr)
            {
                SdkCommand cancelCmd;
                cancelCmd.type = SdkCommandType::Custom;
                cancelCmd.name = "CancelExposure";
                cancelCmd.payload = std::any();
                SdkResult cancelRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, cancelCmd);
                if (!cancelRes.success)
                {
                    Logger::Log("focusLoopShooting | CancelExposure failed: " + cancelRes.message,
                                LogLevel::WARNING, DeviceType::FOCUSER);
                }
            }

            if (sdkExposureTimer)
            {
                sdkExposureTimer->stop();
            }

            sdkExposureIsROI = false;

            SdkAreaInfo full;
            full.startX = 0;
            full.startY = 0;
            full.sizeX = glMainCCDSizeX;
            full.sizeY = glMainCCDSizeY;
            SdkCommand setRoiCmd;
            setRoiCmd.type = SdkCommandType::Custom;
            setRoiCmd.name = "SetResolution";
            setRoiCmd.payload = full;
            SdkResult roiRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setRoiCmd);
            if (!roiRes.success)
            {
                Logger::Log("focusLoopShooting | SDK restore full resolution failed: " + roiRes.message,
                            LogLevel::WARNING, DeviceType::FOCUSER);
            }

            if (glMainCameraStatu == "Exposuring")
            {
                glMainCameraStatu = "IDLE";
            }
        }
        else
        {
            if (sdkExposureTimer)
            {
                sdkExposureTimer->stop();
            }
            sdkExposureIsROI = false;

            if (glMainCameraStatu == "Exposuring" && isMainCameraConnected())
            {
                abortMainCameraCapture();
            }
        }
    }
}

void MainWindow::getFocuserLoopingState()
{
    if (isFocusLoopShooting)
    {
        emit wsThread->sendMessageToClient("setFocuserLoopingState:true");
    }
    else
    {
        emit wsThread->sendMessageToClient("setFocuserLoopingState:false");
    }
}
