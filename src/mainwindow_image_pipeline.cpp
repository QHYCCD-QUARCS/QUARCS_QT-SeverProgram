#include "mainwindow_command_support.h"

#include <QtConcurrent/QtConcurrentRun>

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

std::string sampleBayer2x2Debug(const cv::Mat& image16)
{
    if (image16.empty() || image16.type() != CV_16UC1) {
        return "2x2=unavailable";
    }

    const int maxY = std::min(2, image16.rows);
    const int maxX = std::min(2, image16.cols);
    std::ostringstream oss;
    oss << "2x2=[";
    for (int y = 0; y < maxY; ++y) {
        if (y > 0) oss << ";";
        for (int x = 0; x < maxX; ++x) {
            if (x > 0) oss << ",";
            oss << image16.at<uint16_t>(y, x);
        }
    }
    oss << "]";
    return oss.str();
}

} // namespace

int MainWindow::saveFitsAsPNG(QString fitsFileName, bool ProcessBin, std::function<void(bool)> onComplete)
{
    // 目标：主线程不阻塞；重活放后台，并可用 epoch 取消旧帧任务
    const quint64 epoch = ++tilePyramidEpoch;
    QPointer<MainWindow> self(this);
    const QString fitsCopy = fitsFileName;
    const bool processBinCopy = ProcessBin;

    QtConcurrent::run([self, epoch, fitsCopy, processBinCopy, onComplete]() {
        if (!self) return;
        if (self->tilePyramidEpoch.load() != epoch) return;
        const int rc = self->saveFitsAsPNG_Worker(fitsCopy, processBinCopy);

        // Live 模式的“处理链路 busy”必须在处理结束后再释放（否则会导致每帧都排队重处理）
        QMetaObject::invokeMethod(self, [self, epoch, rc, onComplete]() {
            if (!self) return;
            if (self->tilePyramidEpoch.load() != epoch) return;
            if (onComplete) onComplete(rc == 0);
            (void)rc;
            self->sdkMainLiveProcessingBusy = false;
        }, Qt::QueuedConnection);
    });

    return 0;
}

int MainWindow::saveFitsAsPNG_FromSdkFrame(const std::shared_ptr<SdkFrameData>& frame, bool ProcessBin, std::function<void(bool)> onComplete)
{
    const quint64 epoch = ++tilePyramidEpoch;
    QPointer<MainWindow> self(this);
    const bool processBinCopy = ProcessBin;
    auto frameCopy = frame;

    QtConcurrent::run([self, epoch, processBinCopy, frameCopy, onComplete]() {
        if (!self || !frameCopy) return;
        if (self->tilePyramidEpoch.load() != epoch) return;
        const int rc = self->saveFitsAsPNG_FromSdkFrame_Worker(frameCopy, processBinCopy);

        QMetaObject::invokeMethod(self, [self, epoch, rc, onComplete]() {
            if (!self) return;
            if (self->tilePyramidEpoch.load() != epoch) return;
            if (onComplete) onComplete(rc == 0);
            (void)rc;
            self->sdkMainLiveProcessingBusy = false;
        }, Qt::QueuedConnection);
    });

    return 0;
}

int MainWindow::saveFitsAsPNG_Worker(QString fitsFileName, bool ProcessBin)
{
    // 旧实现会在 saveFitsAsPNG() 一开始就触发下一帧拍摄（并且直接调用 startMainCameraCapture），
    // 在当前 SDK/定时器/线程队列/状态机体系下会与“出图链路”竞争同一相机资源，导致不稳定。
    // 新语义：仅当本帧完整出图成功后，才异步触发下一帧（QueuedConnection），避免递归/重入。
    Logger::Log("Starting to save FITS as PNG...", LogLevel::INFO, DeviceType::CAMERA);
    const quint64 epochAtStart = tilePyramidEpoch.load();
    
    cv::Mat image;
    cv::Mat originalImage16;
    Logger::Log("FITS file path: " + fitsFileName.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    int status = Tools::readFits(fitsFileName.toLocal8Bit().constData(), image);

    if (status != 0)
    {
        Logger::Log("Failed to read FITS file: " + fitsFileName.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return status;
    }
    if (image.empty())
    {
        Logger::Log("saveFitsAsPNG | readFits succeeded but image is empty: " + fitsFileName.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }
    if (image.type() == CV_8UC1 || image.type() == CV_8UC3 || image.type() == CV_16UC1)
    {
        originalImage16 = Tools::convert8UTo16U_BayerSafe(image, false);
        image.release();
    }
    else
    {
        Logger::Log("The current image data type is not supported for processing.", LogLevel::WARNING, DeviceType::CAMERA);
        return -1;
    }
    if (originalImage16.empty())
    {
        Logger::Log("saveFitsAsPNG | convert8UTo16U_BayerSafe returned empty image; skip medianBlur", LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }

    // 创建MainCameraCFA的局部副本，防止多线程竞态条件导致的值污染
    QString localCameraCFA = MainCameraCFA;

    // 验证CFA值的合法性
    QStringList validCFAValues = {"RGGB", "BGGR", "GRBG", "GBRG", "RG", "BG", "GR", "GB", "", "null"};
    if (!validCFAValues.contains(localCameraCFA))
    {
        Logger::Log("saveFitsAsPNG | Invalid MainCameraCFA value detected: '" + localCameraCFA.toStdString() +
                   "'. Using empty (Mono mode) for this operation.", LogLevel::ERROR, DeviceType::CAMERA);
        localCameraCFA = "";
    }

    const int rc = processImageForFrontend(originalImage16, localCameraCFA, ProcessBin, fitsFileName);
    if (rc != 0) {
        return rc;
    }

    if (!fitsFileName.contains("ccd_simulator_original.fits"))
    {
        const QString destinationPath = QStringLiteral("/dev/shm/ccd_simulator_original.fits");
        QFile::remove(destinationPath);
        QFile::copy(fitsFileName, destinationPath);
    }

    return 0;
}

int MainWindow::saveFitsAsPNG_FromSdkFrame_Worker(std::shared_ptr<SdkFrameData> frame, bool ProcessBin)
{
    Logger::Log("Starting to save SDK frame as PNG...", LogLevel::INFO, DeviceType::CAMERA);
    if (!frame)
    {
        Logger::Log("saveFitsAsPNG_FromSdkFrame | frame is null", LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }

    cv::Mat sourceImage;
    cv::Mat originalImage16;

    if (!frame->pixels.empty())
    {
        const size_t needPixels = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
        if (frame->pixels.size() < needPixels)
        {
            Logger::Log("saveFitsAsPNG_FromSdkFrame | pixels buffer too small", LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }
        sourceImage = cv::Mat(frame->height, frame->width, CV_16UC1,
                              const_cast<uint16_t*>(frame->pixels.data())).clone();
    }
    else if (frame->rawBuffer != nullptr && frame->rawBytes > 0)
    {
        if (frame->channels != 1 || (frame->bpp != 16 && frame->bpp != 8))
        {
            Logger::Log("saveFitsAsPNG_FromSdkFrame | unsupported rawBuffer format: bpp=" +
                        std::to_string(frame->bpp) + " channels=" + std::to_string(frame->channels),
                        LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }

        const size_t pixelCount = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
        const size_t needBytes = pixelCount * static_cast<size_t>(frame->bpp / 8);
        if (frame->rawBuffer->size() < needBytes || frame->rawBytes < needBytes)
        {
            Logger::Log("saveFitsAsPNG_FromSdkFrame | rawBuffer too small: needBytes=" +
                        std::to_string(needBytes) + " rawBytes=" + std::to_string(frame->rawBytes) +
                        " bufSize=" + std::to_string(frame->rawBuffer->size()),
                        LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }

        const int cvType = (frame->bpp == 16) ? CV_16UC1 : CV_8UC1;
        sourceImage = cv::Mat(frame->height, frame->width, cvType, frame->rawBuffer->data()).clone();
    }
    else
    {
        Logger::Log("saveFitsAsPNG_FromSdkFrame | frame has no image payload", LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }

    originalImage16 = Tools::convert8UTo16U_BayerSafe(sourceImage, false);
    if (originalImage16.empty())
    {
        Logger::Log("saveFitsAsPNG_FromSdkFrame | convert8UTo16U_BayerSafe returned empty image",
                    LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }

    QString localCameraCFA = MainCameraCFA;
    QStringList validCFAValues = {"RGGB", "BGGR", "GRBG", "GBRG", "RG", "BG", "GR", "GB", "", "null"};
    if (!validCFAValues.contains(localCameraCFA))
    {
        Logger::Log("saveFitsAsPNG_FromSdkFrame | Invalid MainCameraCFA value detected: '" +
                    localCameraCFA.toStdString() + "'. Using empty (Mono mode) for this operation.",
                    LogLevel::ERROR, DeviceType::CAMERA);
        localCameraCFA = "";
    }

    const int rc = processImageForFrontend(originalImage16, localCameraCFA, ProcessBin, QStringLiteral("sdk_frame"));
    if (rc != 0) {
        return rc;
    }

    const std::string fitsPath = "/dev/shm/ccd_simulator.fits";
    SaveQhyFrameDataToFits(*frame, fitsPath);
    const QString destinationPath = QStringLiteral("/dev/shm/ccd_simulator_original.fits");
    QFile::remove(destinationPath);
    QFile::copy(QString::fromStdString(fitsPath), destinationPath);
    return 0;
}

int MainWindow::processImageForFrontend(const cv::Mat& inputImage16, const QString& cameraCFA, bool ProcessBin, const QString& sourceTag)
{
    emitCaptureTrace(QStringLiteral("backend_process_image_start"), currentCaptureTraceStartedAtMs,
                     QString("sourceTag=%1").arg(sourceTag));
    const qint64 processStageStartMs = QDateTime::currentMSecsSinceEpoch();
    const quint64 epochAtStart = tilePyramidEpoch.load();
    cv::Mat originalImage16 = inputImage16.clone();
    cv::Mat image16;
    QString effectiveCameraCFA = cameraCFA;

    // 中值滤波（可选）：大图上会显著增加耗时；默认在 fast 模式关闭
    if (tilePyramidFastEnableMedianBlur) {
        Logger::Log("Starting median blur...", LogLevel::INFO, DeviceType::CAMERA);
        try
        {
            cv::medianBlur(originalImage16, originalImage16, 3);
        }
        catch (const cv::Exception &e)
        {
            Logger::Log(std::string("saveFitsAsPNG | medianBlur failed: ") + e.what(), LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }
        Logger::Log("Median blur applied successfully.", LogLevel::INFO, DeviceType::CAMERA);
    } else {
        Logger::Log("Median blur skipped (fast mode).", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    // 使用局部CFA副本，避免全局变量在多线程环境中被污染
    bool isColor = !(effectiveCameraCFA == "" || effectiveCameraCFA == "null");
    Logger::Log("Camera color mode: " + std::string(isColor ? "Color" : "Mono") + " CFA: " + effectiveCameraCFA.toStdString() +
                " source: " + sourceTag.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    // 记录预览/解析保存使用的软件 bin 因子（用于前端对照调试）
    int binningFactor = 1;
    if (ProcessBin) {
        binningFactor = glMainCameraBinning;
        if (binningFactor < 1) binningFactor = 1;
        if (binningFactor > 16) binningFactor = 16;
        // 防御：若不是 2^N，则向上取整到最近的 2^N（例如 3->4, 6->8），并封顶 16
        int p2 = 1;
        while (p2 < binningFactor && p2 < 16) p2 <<= 1;
        if (p2 > 16) p2 = 16;
        binningFactor = p2;
    }

    if (ProcessBin && binningFactor != 1)
    {
        // 单色大图预览优先走 OpenCV INTER_AREA，避免通用 bin 路径带来的额外逐像素开销。
        if (!isColor && originalImage16.type() == CV_16UC1)
        {
            const int newWidth = std::max(1, originalImage16.cols / binningFactor);
            const int newHeight = std::max(1, originalImage16.rows / binningFactor);
            cv::resize(originalImage16, image16, cv::Size(newWidth, newHeight), 0, 0, cv::INTER_AREA);
        }
        // 使用新的Mat版本的PixelsDataSoftBin_Bayer函数
        else if (effectiveCameraCFA == "RGGB" || effectiveCameraCFA == "RG")
        {
            image16 = Tools::PixelsDataSoftBin_Bayer(originalImage16, binningFactor, binningFactor, BAYER_RGGB);
        }
        else if (effectiveCameraCFA == "BGGR" || effectiveCameraCFA == "BG")
        {
            image16 = Tools::PixelsDataSoftBin_Bayer(originalImage16, binningFactor, binningFactor, BAYER_BGGR);
        }
        else if (effectiveCameraCFA == "GRBG" || effectiveCameraCFA == "GR")
        {
            image16 = Tools::PixelsDataSoftBin_Bayer(originalImage16, binningFactor, binningFactor, BAYER_GRBG);
        }
        else if (effectiveCameraCFA == "GBRG" || effectiveCameraCFA == "GB")
        {
            image16 = Tools::PixelsDataSoftBin_Bayer(originalImage16, binningFactor, binningFactor, BAYER_GBRG);
        }
        else
        {
            image16 = Tools::processMatWithBinAvg(originalImage16, binningFactor, binningFactor, isColor, true);
        }
    }
    else
    {
        image16 = originalImage16.clone();
    }
    emitCaptureTrace(QStringLiteral("backend_process_preview_ready"), processStageStartMs,
                     QString("processBin=%1,preview=%2x%3,tileSource=%4x%5")
                         .arg(ProcessBin ? QStringLiteral("true") : QStringLiteral("false"))
                         .arg(image16.cols)
                         .arg(image16.rows)
                         .arg(originalImage16.cols)
                         .arg(originalImage16.rows));

    // 软件 bin 后图仅用于另一路 FITS 保存；瓦片源统一使用原图，避免在瓦片构建前提前合并。
    const cv::Mat& tileSourceImage = originalImage16;

    const int width = tileSourceImage.cols;
    const int height = tileSourceImage.rows;
    Logger::Log("MainCameraSize (tile source) dimensions: " + std::to_string(width) + "x" + std::to_string(height), LogLevel::INFO, DeviceType::CAMERA);
    emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(width) + ":" + QString::number(height));
    emit wsThread->sendMessageToClient("MainCameraBinning:1");

    if (tileSourceImage.empty())
    {
        Logger::Log("saveFitsAsPNG | tileSourceImage is empty, cannot save.", LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }
    if (tileSourceImage.type() != CV_16UC1 && tileSourceImage.type() != CV_8UC1)
    {
        Logger::Log("saveFitsAsPNG | unsupported image type: " + std::to_string(tileSourceImage.type()), LogLevel::WARNING, DeviceType::CAMERA);
        return -1;
    }

    // 自动图像优化已前端化：后端主链路不再按帧计算自动白平衡增益。
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | tileSourceImageSize = " +
                    std::to_string(tileSourceImage.cols) + "x" + std::to_string(tileSourceImage.rows),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | tileSourceImageType = " +
                    std::to_string(tileSourceImage.type()),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | effectiveCameraCFA = " +
                    effectiveCameraCFA.toStdString(),
                LogLevel::INFO, DeviceType::CAMERA);

    // ========================= 瓦片（视口驱动生成） =========================
    // 每张图像使用独立会话目录：live_<epoch>，便于区分并清理旧图
    const QString sessionId = QString("live_%1").arg(epochAtStart);

    // 确保 tiles 主目录存在（tmpfs：/dev/shm/capture-tiles/）
    QDir tilesDir(QString::fromStdString(tilePyramidPath));
    if (!tilesDir.exists()) {
        if (!tilesDir.mkpath(".")) {
            Logger::Log("Failed to create tiles directory: " + tilePyramidPath, LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }
        QFile::setPermissions(QString::fromStdString(tilePyramidPath),
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
            QFileDevice::ReadGroup | QFileDevice::ExeGroup |
            QFileDevice::ReadOther | QFileDevice::ExeOther);
        Logger::Log("Created tiles directory (tmpfs): " + tilePyramidPath, LogLevel::INFO, DeviceType::CAMERA);
    }
    // 创建当前会话目录
    if (!tilesDir.mkpath(sessionId)) {
        Logger::Log("Failed to create session tiles directory: " + sessionId.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }

    // GPM 仅保留图像与瓦片元数据；自动优化参数改由前端基于 Z=0 计算。
    int maxMergeFactor = 16;
    const qint64 gpmCalcStartMs = QDateTime::currentMSecsSinceEpoch();
    TileGPM gpm = calculateGPM(tileSourceImage, effectiveCameraCFA, maxMergeFactor, /*enableHistogram=*/false);
    emitCaptureTrace(QStringLiteral("backend_calculate_gpm_done"), gpmCalcStartMs,
                     QString("sessionCandidate=live_%1,image=%2x%3,maxZoom=%4")
                         .arg(QString::number(static_cast<qulonglong>(epochAtStart)))
                         .arg(tileSourceImage.cols)
                         .arg(tileSourceImage.rows)
                         .arg(gpm.maxZoomLevel));
    gpm.sessionId = sessionId;
    gpm.previewWidth = image16.cols;
    gpm.previewHeight = image16.rows;
    gpm.previewBinningFactor = binningFactor;
    // 帧ID：与本次 saveFitsAsPNG() 的 epoch 对齐，用于前端/瓦片请求做“错帧丢弃”
    gpm.frameId = epochAtStart;
    gpm.buildMode = QStringLiteral("pyramid");
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | sessionId = " +
                    sessionId.toStdString(),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | frameId = " +
                    std::to_string(static_cast<unsigned long long>(gpm.frameId)),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | gpmBlackWhite = " +
                    std::to_string(gpm.blackLevel) + "," + std::to_string(gpm.whiteLevel),
                LogLevel::INFO, DeviceType::CAMERA);

    // 保存“最新帧”供视口拖动/缩放时按需补瓦片（避免反复 readFits）
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        tileFrame.epoch = epochAtStart;
        tileFrame.sessionId = sessionId;
        tileFrame.frameId = epochAtStart;
        tileFrame.imageWidth = tileSourceImage.cols;
        tileFrame.imageHeight = tileSourceImage.rows;
        tileFrame.previewBinningFactor = binningFactor;
        tileFrame.tileSize = tilePyramidTileSize;
        tileFrame.maxZoomLevel = gpm.maxZoomLevel;
        tileFrame.cfa = effectiveCameraCFA;
        tileFrame.blackLevel = gpm.blackLevel;
        tileFrame.whiteLevel = gpm.whiteLevel;
        tileFrameImage16 = std::make_shared<cv::Mat>(tileSourceImage); // 共享底层buffer（ref-count）
        tileFramePreviewImage16 = std::make_shared<cv::Mat>(image16);
    }

    // 首帧链路只同步准备 z=0 预览瓦片：
    // - 保证前端收到 GPM 后，Z0 一定可立即拉取
    // - currentZ-1/currentZ 的局部高清瓦片改走后续异步补齐，避免它们阻塞首图
    const qint64 visibleTilesStartMs = QDateTime::currentMSecsSinceEpoch();
    generateVisibleTilesSync(epochAtStart, /*includeViewportLevels=*/false);
    emitCaptureTrace(QStringLiteral("backend_visible_tiles_ready"), visibleTilesStartMs,
                     QString("sessionId=%1,frameId=%2")
                         .arg(sessionId)
                         .arg(QString::number(static_cast<qulonglong>(epochAtStart))));

    // 发送GPM到前端
    if (tilePyramidEpoch.load() != epochAtStart) {
        Logger::Log("saveFitsAsPNG | cancelled before sending GPM (newer epoch)", LogLevel::WARNING, DeviceType::CAMERA);
        return -1;
    }
    sendGPMToClient(gpm);
    emitCaptureTrace(QStringLiteral("backend_tilegpm_sent"), currentCaptureTraceStartedAtMs,
                     QString("sessionId=%1,frameId=%2,serverNowMs=%3")
                         .arg(gpm.sessionId)
                         .arg(QString::number(static_cast<qulonglong>(gpm.frameId)))
                         .arg(QString::number(QDateTime::currentMSecsSinceEpoch())));

    // Z0/GPM 已经先行放出；当前视口的其它高清瓦片再异步补齐即可。
    const qint64 viewportScheduleStartMs = QDateTime::currentMSecsSinceEpoch();
    scheduleViewportTileGeneration();
    scheduleFullResTileCompletion();
    emitCaptureTrace(QStringLiteral("backend_schedule_viewport_tiles_done"), viewportScheduleStartMs,
                     QString("sessionId=%1,frameId=%2")
                         .arg(sessionId)
                         .arg(QString::number(static_cast<qulonglong>(epochAtStart))));
    // 删除其它旧会话的瓦片目录，仅保留当前 sessionId
    cleanupOldTileSessionDirs(sessionId);

    // 预览 FITS 挪到后台保存，避免首帧显示被额外 IO/编码阻塞。
    auto previewFitsImage = std::make_shared<cv::Mat>(image16);
    QtConcurrent::run([previewFitsImage]() {
        if (!previewFitsImage || previewFitsImage->empty()) return;
        Tools::SaveMatToFITS(*previewFitsImage);
        Logger::Log("Image saved as FITS.", LogLevel::INFO, DeviceType::CAMERA);
    });

    // 更新状态（回主线程，避免数据竞争；同时在这里触发 loop capture）
    QMetaObject::invokeMethod(this, [this, sessionId, epochAtStart]() {
        if (tilePyramidEpoch.load() != epochAtStart) {
            return;
        }
        isStagingImage = true;
        SavedImage = sessionId.toStdString();
        isSavePngSuccess = true;

        // Loop capture：仅当本帧“出图链路”完成成功后才触发下一帧，避免重入。
        if (LoopCaptureNum > 0) {
            LoopCaptureNum--;
            const int nextExpMs = glExpTime;

            // 防御：若状态机判定仍忙，则直接跳过（避免连环触发造成 Exposuring 竞争）
            if (glMainCameraStatu == "Exposuring" || sdkBurstActive.load()) {
                Logger::Log("LoopCapture | camera busy, skip next trigger", LogLevel::WARNING, DeviceType::CAMERA);
                return;
            }

            // 按当前主相机采集模式分流：
            // - Single：走 startMainCameraCapture（覆盖 INDI + SDK 单帧）
            // - Burst：走 SDK_BurstCapture（输出仍走 saveFitsAsPNG 链路）
            if (mainCameraCaptureMode == MainCameraCaptureMode::Burst) {
                Logger::Log("LoopCapture | trigger next BURST, exp_ms=" + std::to_string(nextExpMs) +
                                ", frames=" + std::to_string(LoopCaptureBurstFrames) +
                                ", remaining=" + std::to_string(LoopCaptureNum),
                            LogLevel::INFO, DeviceType::CAMERA);
                SDK_BurstCapture(nextExpMs, LoopCaptureBurstFrames);
            } else {
                Logger::Log("LoopCapture | trigger next SINGLE, exp_ms=" + std::to_string(nextExpMs) +
                                ", remaining=" + std::to_string(LoopCaptureNum),
                            LogLevel::INFO, DeviceType::CAMERA);
                startMainCameraCapture(nextExpMs);
            }
        }
    }, Qt::QueuedConnection);

    Logger::Log("Tile GPM sent; viewport-driven tiles scheduled.", LogLevel::INFO, DeviceType::CAMERA);
    // ========================= 视口驱动瓦片结束 =========================

    // 移除这里的 setCaptureComplete 调用，避免与外部调用重复
    // 调用者会在需要时调用 autoFocus->setCaptureComplete()
    // if (isAutoFocus)
    // {
    //     autoFocus->setCaptureComplete(fitsFileName);
    // }

    Logger::Log("saveFitsAsPNG completed successfully.", LogLevel::DEBUG, DeviceType::CAMERA);
    return 0;  // 🔧 修复：函数必须返回值，避免未定义行为导致内存错误

}

// ========================= 瓦片金字塔生成相关函数 =========================

/**
 * @brief 计算白平衡增益（基于灰度世界算法）
 * @param image16 16位原始图像
 * @param cfa CFA模式
 * @return QPair<gainR, gainB> R和B通道的增益值
 */
QString MainWindow::normalizeCfaPattern(const QString& cfa)
{
    const QString normalized = cfa.trimmed().toUpper();
    if (normalized == "RG" || normalized == "RGGB") return QStringLiteral("RGGB");
    if (normalized == "BG" || normalized == "BGGR") return QStringLiteral("BGGR");
    if (normalized == "GR" || normalized == "GRBG") return QStringLiteral("GRBG");
    if (normalized == "GB" || normalized == "GBRG") return QStringLiteral("GBRG");
    return normalized;
}

QPointF MainWindow::snapRoiOriginToBayerSafePhase(double roiX, double roiY, int roiWidth, int roiHeight) const
{
    const bool tileModeActive = (isStagingImage && !SavedImage.empty());
    const int roiCoordScale = tileModeActive ? 1 : std::max(1, glMainCameraBinning);

    int effMinX = 0;
    int effMinY = 0;
    int effW = std::max(0, glMainCCDSizeX);
    int effH = std::max(0, glMainCCDSizeY);
    if (sdkMainEffectiveAreaCacheValid) {
        effMinX = sdkMainEffectiveAreaMinX;
        effMinY = sdkMainEffectiveAreaMinY;
        effW = sdkMainEffectiveAreaWidth;
        effH = sdkMainEffectiveAreaHeight;
    }

    int scaledX = static_cast<int>(std::lround(roiX * roiCoordScale));
    int scaledY = static_cast<int>(std::lround(roiY * roiCoordScale));
    scaledX = std::max(0, scaledX);
    scaledY = std::max(0, scaledY);

    if (roiWidth > 0 && effW > 0) scaledX = std::min(scaledX, std::max(0, effW - roiWidth));
    if (roiHeight > 0 && effH > 0) scaledY = std::min(scaledY, std::max(0, effH - roiHeight));

    if (((effMinX + scaledX) & 1) != 0) {
        if (roiWidth > 0 && effW > 0 && scaledX + 1 <= std::max(0, effW - roiWidth)) {
            scaledX += 1;
        } else {
            scaledX = std::max(0, scaledX - 1);
        }
    }
    if (((effMinY + scaledY) & 1) != 0) {
        if (roiHeight > 0 && effH > 0 && scaledY + 1 <= std::max(0, effH - roiHeight)) {
            scaledY += 1;
        } else {
            scaledY = std::max(0, scaledY - 1);
        }
    }

    if (roiWidth > 0 && effW > 0) scaledX = std::min(scaledX, std::max(0, effW - roiWidth));
    if (roiHeight > 0 && effH > 0) scaledY = std::min(scaledY, std::max(0, effH - roiHeight));

    return QPointF(static_cast<double>(scaledX) / static_cast<double>(roiCoordScale),
                   static_cast<double>(scaledY) / static_cast<double>(roiCoordScale));
}

int MainWindow::getOpenCvBayerToBgrCode(const QString& cfa)
{
    const QString normalizedCfa = normalizeCfaPattern(cfa);
    if (normalizedCfa == "RGGB") return cv::COLOR_BayerRG2BGR;
    if (normalizedCfa == "BGGR") return cv::COLOR_BayerBG2BGR;
    if (normalizedCfa == "GRBG") return cv::COLOR_BayerGR2BGR;
    if (normalizedCfa == "GBRG") return cv::COLOR_BayerGB2BGR;
    return -1;
}

bool MainWindow::tryGetBayerPattern(const QString& cfa, BayerPattern& outPattern)
{
    const QString normalizedCfa = normalizeCfaPattern(cfa);
    if (normalizedCfa == "RGGB") {
        outPattern = BAYER_RGGB;
        return true;
    }
    if (normalizedCfa == "BGGR") {
        outPattern = BAYER_BGGR;
        return true;
    }
    if (normalizedCfa == "GRBG") {
        outPattern = BAYER_GRBG;
        return true;
    }
    if (normalizedCfa == "GBRG") {
        outPattern = BAYER_GBRG;
        return true;
    }
    return false;
}

cv::Mat MainWindow::downsampleTileImageForLevel(const cv::Mat& image, const QString& cfa, int scaleFactor)
{
    if (image.empty()) return cv::Mat();
    if (scaleFactor <= 1) return image;

    // 非 2^N 或非单通道 RAW 的路径，退回普通面积缩小。
    const bool isPowerOfTwo = (scaleFactor & (scaleFactor - 1)) == 0;
    BayerPattern bayerPattern = BAYER_RGGB;
    const bool useBayerSafe =
        isPowerOfTwo &&
        (image.type() == CV_16UC1 || image.type() == CV_8UC1 || image.type() == CV_32SC1) &&
        tryGetBayerPattern(cfa, bayerPattern);

    if (!useBayerSafe) {
        cv::Mat resized;
        cv::resize(image, resized,
                   cv::Size(std::max(1, image.cols / scaleFactor), std::max(1, image.rows / scaleFactor)),
                   0, 0, cv::INTER_AREA);
        return resized;
    }

    cv::Mat current = image;
    int factor = scaleFactor;
    while (factor > 1) {
        cv::Mat next = Tools::PixelsDataSoftBin_Bayer(current, 2, 2, bayerPattern);
        if (next.empty()) {
            Logger::Log("[TileDebug] event=downsampleTileImageForLevelFallback reason=BayerSafeBinFailed cfa=" +
                            cfa.toStdString() + " scaleFactor=" + std::to_string(scaleFactor),
                        LogLevel::WARNING, DeviceType::CAMERA);
            cv::Mat fallback;
            cv::resize(image, fallback,
                       cv::Size(std::max(1, image.cols / scaleFactor), std::max(1, image.rows / scaleFactor)),
                       0, 0, cv::INTER_AREA);
            return fallback;
        }
        current = std::move(next);
        factor >>= 1;
    }
    Logger::Log("[TileDebug] event=downsampleTileImageForLevel cfa=" + cfa.toStdString() +
                    " scaleFactor=" + std::to_string(scaleFactor) +
                    " input=" + std::to_string(image.cols) + "x" + std::to_string(image.rows) +
                    " output=" + std::to_string(current.cols) + "x" + std::to_string(current.rows),
                LogLevel::DEBUG, DeviceType::CAMERA);
    return current;
}

QString MainWindow::deriveCfaPatternForOffset(const QString& baseCfa, int shiftX, int shiftY)
{
    const QString normalizedCfa = normalizeCfaPattern(baseCfa);
    if (normalizedCfa.isEmpty() || normalizedCfa == "NULL" || normalizedCfa == "MONO" || normalizedCfa == "NULL") {
        return QString();
    }

    const bool oddX = (shiftX & 1) != 0;
    const bool oddY = (shiftY & 1) != 0;

    if (normalizedCfa == "RGGB") {
        if (!oddX && !oddY) return QStringLiteral("RGGB");
        if ( oddX && !oddY) return QStringLiteral("GRBG");
        if (!oddX &&  oddY) return QStringLiteral("GBRG");
        return QStringLiteral("BGGR");
    }
    if (normalizedCfa == "GRBG") {
        if (!oddX && !oddY) return QStringLiteral("GRBG");
        if ( oddX && !oddY) return QStringLiteral("RGGB");
        if (!oddX &&  oddY) return QStringLiteral("BGGR");
        return QStringLiteral("GBRG");
    }
    if (normalizedCfa == "GBRG") {
        if (!oddX && !oddY) return QStringLiteral("GBRG");
        if ( oddX && !oddY) return QStringLiteral("BGGR");
        if (!oddX &&  oddY) return QStringLiteral("RGGB");
        return QStringLiteral("GRBG");
    }
    if (normalizedCfa == "BGGR") {
        if (!oddX && !oddY) return QStringLiteral("BGGR");
        if ( oddX && !oddY) return QStringLiteral("GBRG");
        if (!oddX &&  oddY) return QStringLiteral("GRBG");
        return QStringLiteral("RGGB");
    }

    return normalizedCfa;
}

QString MainWindow::resolveFrameCfa(int frameStartX, int frameStartY) const
{
    const QString normalizedBase = normalizeCfaPattern(MainCameraCFA);
    if (normalizedBase.isEmpty() || normalizedBase == "NULL" || normalizedBase == "MONO") {
        return QString();
    }
    const int totalShiftX = MainCameraCFAOffsetX + frameStartX;
    const int totalShiftY = MainCameraCFAOffsetY + frameStartY;
    return deriveCfaPatternForOffset(normalizedBase, totalShiftX, totalShiftY);
}

QPair<double, double> MainWindow::calculateWhiteBalanceGains(const cv::Mat& image16, const QString& cfa, uint16_t offset)
{
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | imageSize = " +
                    std::to_string(image16.cols) + "x" + std::to_string(image16.rows),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | cfa = " +
                    cfa.toStdString(),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | offset = " +
                    std::to_string(offset),
                LogLevel::INFO, DeviceType::MAIN);
    if (image16.empty() || cfa == "null" || cfa.isEmpty()) {
        Logger::Log("无法计算白平衡：图像为空或非彩色图像", LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    if (image16.type() != CV_16UC1 || image16.rows < 2 || image16.cols < 2) {
        Logger::Log("无法计算白平衡：图像类型不是 CV_16UC1 或尺寸过小", LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    std::vector<cv::Point> rOffsets;
    std::vector<cv::Point> gOffsets;
    std::vector<cv::Point> bOffsets;
    const QString normalizedCfa = normalizeCfaPattern(cfa);

    if (normalizedCfa == "RGGB") {
        rOffsets = { cv::Point(0, 0) };
        gOffsets = { cv::Point(1, 0), cv::Point(0, 1) };
        bOffsets = { cv::Point(1, 1) };
    } else if (normalizedCfa == "GRBG") {
        gOffsets = { cv::Point(0, 0), cv::Point(1, 1) };
        rOffsets = { cv::Point(1, 0) };
        bOffsets = { cv::Point(0, 1) };
    } else if (normalizedCfa == "GBRG") {
        gOffsets = { cv::Point(0, 0), cv::Point(1, 1) };
        bOffsets = { cv::Point(1, 0) };
        rOffsets = { cv::Point(0, 1) };
    } else if (normalizedCfa == "BGGR") {
        bOffsets = { cv::Point(0, 0) };
        gOffsets = { cv::Point(1, 0), cv::Point(0, 1) };
        rOffsets = { cv::Point(1, 1) };
    } else {
        Logger::Log("未知的CFA模式: " + cfa.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    const int rows = image16.rows;
    const int cols = image16.cols;
    const int sampleStep = std::max(2, static_cast<int>(std::floor(std::min(rows, cols) / 200.0)) * 2);

    struct SampleTriplet {
        double r = 0.0;
        double g1 = 0.0;
        double g2 = 0.0;
        double g = 0.0;
        double b = 0.0;
        double luma = 0.0;
    };
    std::vector<SampleTriplet> samples;
    samples.reserve(static_cast<size_t>((rows / sampleStep + 1) * (cols / sampleStep + 1)));

    auto correctedPixelAt = [&](int y, int x) -> double {
        if (y < 0 || y >= rows || x < 0 || x >= cols) return 0.0;
        const uint16_t raw = image16.at<uint16_t>(y, x);
        return static_cast<double>(raw > offset ? (raw - offset) : 0);
    };

    for (int y = 0; y < rows; y += sampleStep) {
        for (int x = 0; x < cols; x += sampleStep) {
            if ((y % (sampleStep * 2)) != 0 || (x % (sampleStep * 2)) != 0) continue;

            double r = 0.0;
            double g1 = 0.0;
            double g2 = 0.0;
            double b = 0.0;

            for (const auto& pos : rOffsets) r += correctedPixelAt(y + pos.y, x + pos.x);
            for (const auto& pos : bOffsets) b += correctedPixelAt(y + pos.y, x + pos.x);
            if (gOffsets.size() >= 1) g1 = correctedPixelAt(y + gOffsets[0].y, x + gOffsets[0].x);
            if (gOffsets.size() >= 2) g2 = correctedPixelAt(y + gOffsets[1].y, x + gOffsets[1].x);
            const double g = (g1 + g2) * 0.5;

            // 过滤黑电平附近样本与异常色块，避免整体发灰、ROI 发绿。
            if (r <= 32.0 || g1 <= 32.0 || g2 <= 32.0 || b <= 32.0) continue;
            const double maxRgb = std::max({r, g, b});
            const double minRgb = std::min({r, g, b});
            if (minRgb <= 0.0 || (maxRgb / minRgb) > 2.5) continue;

            samples.push_back({r, g1, g2, g, b, (r + 2.0 * g + b) * 0.25});
        }
    }

    if (samples.empty()) {
        Logger::Log("白平衡采样失败：有效 Bayer block 样本不足", LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    std::vector<double> lumas;
    lumas.reserve(samples.size());
    for (const auto& sample : samples) lumas.push_back(sample.luma);
    std::sort(lumas.begin(), lumas.end());

    const size_t lowerIndex = std::min(lumas.size() - 1, static_cast<size_t>(std::floor((lumas.size() - 1) * 0.15)));
    const size_t upperIndex = std::min(lumas.size() - 1, static_cast<size_t>(std::floor((lumas.size() - 1) * 0.85)));
    const double lumaMin = lumas[lowerIndex];
    const double lumaMax = lumas[upperIndex];

    std::vector<double> g1Values;
    std::vector<double> g2Values;
    std::vector<double> greenPlaneRatios;
    std::vector<double> ratiosR;
    std::vector<double> ratiosB;
    g1Values.reserve(samples.size());
    g2Values.reserve(samples.size());
    greenPlaneRatios.reserve(samples.size());
    ratiosR.reserve(samples.size());
    ratiosB.reserve(samples.size());
    for (const auto& sample : samples) {
        if (sample.luma < lumaMin || sample.luma > lumaMax) continue;
        g1Values.push_back(sample.g1);
        g2Values.push_back(sample.g2);
        greenPlaneRatios.push_back(std::max(sample.g1, sample.g2) / std::max(std::min(sample.g1, sample.g2), 1.0));
        ratiosR.push_back(sample.g / std::max(sample.r, 1.0));
        ratiosB.push_back(sample.g / std::max(sample.b, 1.0));
    }

    if (g1Values.empty() || g2Values.empty() || ratiosR.empty() || ratiosB.empty()) {
        Logger::Log("白平衡采样失败：中亮度样本不足", LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    auto trimmedMean = [](std::vector<double>& values) -> double {
        std::sort(values.begin(), values.end());
        const size_t lower = static_cast<size_t>(std::floor(values.size() * 0.1));
        const size_t upper = static_cast<size_t>(std::ceil(values.size() * 0.9));
        if (upper <= lower || upper > values.size()) {
            return values[values.size() / 2];
        }

        double sum = 0.0;
        size_t count = 0;
        for (size_t i = lower; i < upper; ++i) {
            sum += values[i];
            ++count;
        }
        return count > 0 ? (sum / static_cast<double>(count))
                         : values[values.size() / 2];
    };

    const double g1Mean = trimmedMean(g1Values);
    const double g2Mean = trimmedMean(g2Values);
    const double greenPlaneMismatch = std::abs(g1Mean - g2Mean) / std::max((g1Mean + g2Mean) * 0.5, 1.0);
    const double greenPlaneRatio = trimmedMean(greenPlaneRatios);
    if (greenPlaneMismatch > 0.12 || greenPlaneRatio > 1.15) {
        Logger::Log("白平衡已跳过：G1/G2 平面失衡过大, g1Mean=" + std::to_string(g1Mean) +
                    ", g2Mean=" + std::to_string(g2Mean) +
                    ", mismatch=" + std::to_string(greenPlaneMismatch) +
                    ", ratio=" + std::to_string(greenPlaneRatio),
                    LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    const double gainR = std::max(0.1, std::min(3.0, trimmedMean(ratiosR)));
    const double gainB = std::max(0.1, std::min(3.0, trimmedMean(ratiosB)));

    Logger::Log("白平衡增益计算完成: R=" + std::to_string(gainR) +
                ", B=" + std::to_string(gainB) +
                ", offset=" + std::to_string(offset) +
                ", samples=" + std::to_string(samples.size()) +
                ", g1Mean=" + std::to_string(g1Mean) +
                ", g2Mean=" + std::to_string(g2Mean),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | sampleCount = " +
                    std::to_string(samples.size()),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | lumaRange = " +
                    std::to_string(lumaMin) + "," + std::to_string(lumaMax),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | gainR = " +
                    std::to_string(gainR),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | gainB = " +
                    std::to_string(gainB),
                LogLevel::INFO, DeviceType::MAIN);

    return QPair<double, double>(gainR, gainB);
}

MainWindow::TileGPM MainWindow::calculateGPM(const cv::Mat& image16, const QString& cfa, int maxMergeFactor, bool enableHistogram)
{
    TileGPM gpm;
    gpm.imageWidth = image16.cols;
    gpm.imageHeight = image16.rows;
    gpm.tileSize = tilePyramidTileSize;
    gpm.cfa = cfa;
    gpm.gainR = ImageGainR;
    gpm.gainB = ImageGainB;
    gpm.buildMode = QStringLiteral("pyramid");
    gpm.levelMode = (tileLevelMode.trimmed().toLower() == QStringLiteral("minmax"))
        ? QStringLiteral("minmax")
        : QStringLiteral("full");

    // 计算最大缩放层级（基于最低精度层的合并倍数 maxMergeFactor=2^N）
    // 例：maxMergeFactor=16 => level 0=16x16 ... level 4=1x1（共5层）
    if (maxMergeFactor < 1) maxMergeFactor = 1;
    if (maxMergeFactor > 16) maxMergeFactor = 16;
    // 若不是 2^N，则向上取整到最近的 2^N（例如 3->4, 6->8），并封顶 16；
    // 以保证与 generateTilePyramid 的“每层缩小2倍”递进一致
    int p2 = 1;
    while (p2 < maxMergeFactor && p2 < 16) p2 <<= 1;
    if (p2 > 16) p2 = 16;
    maxMergeFactor = p2;
    gpm.maxZoomLevel = 0;
    int factor = maxMergeFactor;
    while (factor > 1) {
        factor /= 2;
        gpm.maxZoomLevel++;
    }

    Logger::Log("Calculating GPM for image " + std::to_string(image16.cols) + "x" + std::to_string(image16.rows) + 
                ", maxZoomLevel=" + std::to_string(gpm.maxZoomLevel) + " (" + std::to_string(maxMergeFactor) + "x" + std::to_string(maxMergeFactor) + " -> 1x1)" +
                (enableHistogram ? ", histogram=on" : ", histogram=off"), LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | cfa = " +
                    cfa.toStdString(),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | enableHistogram = " +
                    std::string(enableHistogram ? "true" : "false"),
                LogLevel::INFO, DeviceType::CAMERA);
    gpm.gainR = 1.0;
    gpm.gainB = 1.0;
    gpm.globalMin = 0.0;
    gpm.globalMax = 0.0;
    gpm.globalMean = 0.0;
    gpm.globalStdDev = 0.0;
    gpm.blackLevel = 0;
    gpm.whiteLevel = 65535;
    gpm.histogramBins = 0;
    gpm.histogramTotal = 0;
    gpm.histogram.clear();
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | auto optimization metadata disabled on backend; returning neutral display params",
                LogLevel::INFO, DeviceType::CAMERA);
    return gpm;

    auto tryComputeColorLumaStats = [&](double& meanLumaOut, double& stdLumaOut, size_t& sampleCountOut) -> bool {
        if (image16.type() != CV_16UC1 || image16.rows < 2 || image16.cols < 2) {
            return false;
        }

        const QString normalizedCfa = normalizeCfaPattern(cfa);
        std::vector<cv::Point> rOffsets;
        std::vector<cv::Point> gOffsets;
        std::vector<cv::Point> bOffsets;
        if (normalizedCfa == "RGGB") {
            rOffsets = { cv::Point(0, 0) };
            gOffsets = { cv::Point(1, 0), cv::Point(0, 1) };
            bOffsets = { cv::Point(1, 1) };
        } else if (normalizedCfa == "GRBG") {
            gOffsets = { cv::Point(0, 0), cv::Point(1, 1) };
            rOffsets = { cv::Point(1, 0) };
            bOffsets = { cv::Point(0, 1) };
        } else if (normalizedCfa == "GBRG") {
            gOffsets = { cv::Point(0, 0), cv::Point(1, 1) };
            bOffsets = { cv::Point(1, 0) };
            rOffsets = { cv::Point(0, 1) };
        } else if (normalizedCfa == "BGGR") {
            bOffsets = { cv::Point(0, 0) };
            gOffsets = { cv::Point(1, 0), cv::Point(0, 1) };
            rOffsets = { cv::Point(1, 1) };
        } else {
            return false;
        }

        const int rows = image16.rows;
        const int cols = image16.cols;
        const int sampleStep = std::max(2, static_cast<int>(std::floor(std::min(rows, cols) / 200.0)) * 2);

        long double sumLuma = 0.0L;
        long double sumSqLuma = 0.0L;
        size_t sampleCount = 0;

        auto rawPixelAt = [&](int y, int x) -> double {
            if (y < 0 || y >= rows || x < 0 || x >= cols) return 0.0;
            return static_cast<double>(image16.at<uint16_t>(y, x));
        };

        for (int y = 0; y < rows; y += sampleStep) {
            for (int x = 0; x < cols; x += sampleStep) {
                if ((y % (sampleStep * 2)) != 0 || (x % (sampleStep * 2)) != 0) continue;

                double r = 0.0;
                double g1 = 0.0;
                double g2 = 0.0;
                double b = 0.0;

                for (const auto& pos : rOffsets) r += rawPixelAt(y + pos.y, x + pos.x);
                for (const auto& pos : bOffsets) b += rawPixelAt(y + pos.y, x + pos.x);
                if (gOffsets.size() >= 1) g1 = rawPixelAt(y + gOffsets[0].y, x + gOffsets[0].x);
                if (gOffsets.size() >= 2) g2 = rawPixelAt(y + gOffsets[1].y, x + gOffsets[1].x);
                const double g = (g1 + g2) * 0.5;
                const double luma = (r + 2.0 * g + b) * 0.25;

                sumLuma += static_cast<long double>(luma);
                sumSqLuma += static_cast<long double>(luma) * static_cast<long double>(luma);
                ++sampleCount;
            }
        }

        if (sampleCount == 0) {
            return false;
        }

        const long double dn = static_cast<long double>(sampleCount);
        const long double meanLuma = sumLuma / dn;
        long double varLuma = (sumSqLuma / dn) - meanLuma * meanLuma;
        if (varLuma < 0.0L) varLuma = 0.0L;

        meanLumaOut = static_cast<double>(meanLuma);
        stdLumaOut = static_cast<double>(std::sqrt(varLuma));
        sampleCountOut = sampleCount;
        return true;
    };

    // 目标：尽量把全局统计/拉伸/直方图合并到“一次全图扫描”里（对 CV_16UC1 生效）
    // 性能：若不需要直方图且图像很大，则采用“子采样统计”（把耗时压到 ~10ms 级别）
    if (image16.type() == CV_16UC1) {
        const size_t nTotal = image16.total();
        gpm.histogramTotal = enableHistogram ? static_cast<uint64_t>(nTotal) : 0;
        gpm.histogramBins = enableHistogram ? 65536 : 0;
        std::vector<uint32_t> hist;
        if (enableHistogram) {
            static constexpr int HIST_BINS = 65536;
            hist.assign(static_cast<size_t>(HIST_BINS), 0u);
        }

        uint16_t minV = std::numeric_limits<uint16_t>::max();
        uint16_t maxV = 0;
        uint64_t sum = 0;
        unsigned __int128 sumSq = 0; // 防溢出：n*65535^2 可能接近 1e17

        size_t n = nTotal;
        if (!enableHistogram && nTotal > 2'000'000) {
            // 目标采样点数（约 1e6），用于快速估计 mean/stddev/min/max
            constexpr long double TARGET = 1'000'000.0L;
            const long double ratio = static_cast<long double>(nTotal) / TARGET;
            const int step = std::max(1, static_cast<int>(std::sqrt(std::max(1.0L, ratio))));
            n = 0;
            for (int r = 0; r < image16.rows; r += step) {
                const uint16_t* row = image16.ptr<uint16_t>(r);
                for (int c = 0; c < image16.cols; c += step) {
                    const uint16_t v = row[c];
                    if (v < minV) minV = v;
                    if (v > maxV) maxV = v;
                    sum += v;
                    sumSq += static_cast<unsigned __int128>(v) * static_cast<unsigned __int128>(v);
                    ++n;
                }
            }
            Logger::Log("GPM | subsample stats: total=" + std::to_string(nTotal) +
                            ", sampled=" + std::to_string(n) + ", step=" + std::to_string(step),
                        LogLevel::DEBUG, DeviceType::CAMERA);
        } else {
            if (image16.isContinuous()) {
                const uint16_t* p = image16.ptr<uint16_t>(0);
                for (size_t i = 0; i < n; ++i) {
                    const uint16_t v = p[i];
                    if (enableHistogram) ++hist[v];
                    if (v < minV) minV = v;
                    if (v > maxV) maxV = v;
                    sum += v;
                    sumSq += static_cast<unsigned __int128>(v) * static_cast<unsigned __int128>(v);
                }
            } else {
                for (int r = 0; r < image16.rows; ++r) {
                    const uint16_t* row = image16.ptr<uint16_t>(r);
                    for (int c = 0; c < image16.cols; ++c) {
                        const uint16_t v = row[c];
                        if (enableHistogram) ++hist[v];
                        if (v < minV) minV = v;
                        if (v > maxV) maxV = v;
                        sum += v;
                        sumSq += static_cast<unsigned __int128>(v) * static_cast<unsigned __int128>(v);
                    }
                }
            }
        }

        gpm.globalMin = static_cast<double>(minV);
        gpm.globalMax = static_cast<double>(maxV);
        if (enableHistogram) gpm.histogram = std::move(hist);

        // mean/stdDev（由 sum/sumSq 推导）
        const long double dn = static_cast<long double>(std::max<size_t>(1, n));
        const long double mean = static_cast<long double>(sum) / dn;
        const long double ex2 = static_cast<long double>(sumSq) / dn;
        long double var = ex2 - mean * mean;
        if (var < 0) var = 0; // 防浮点误差导致负数
        const long double stdDev = std::sqrt(var);
        gpm.globalMean = static_cast<double>(mean);
        gpm.globalStdDev = static_cast<double>(stdDev);
        Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | rawMean = " +
                        std::to_string(gpm.globalMean),
                    LogLevel::INFO, DeviceType::CAMERA);
        Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | rawStdDev = " +
                        std::to_string(gpm.globalStdDev),
                    LogLevel::INFO, DeviceType::CAMERA);

        const bool isColorRaw =
            !normalizeCfaPattern(cfa).isEmpty() &&
            normalizeCfaPattern(cfa) != "null";
        if (isColorRaw) {
            double meanLuma = 0.0;
            double stdLuma = 0.0;
            size_t lumaSamples = 0;
            if (tryComputeColorLumaStats(meanLuma, stdLuma, lumaSamples)) {
                gpm.globalMean = meanLuma;
                gpm.globalStdDev = stdLuma;
                Logger::Log("GPM | color RAW luma stats override: meanLuma=" + std::to_string(meanLuma) +
                                ", stdLuma=" + std::to_string(stdLuma) +
                                ", sampledBlocks=" + std::to_string(lumaSamples),
                            LogLevel::INFO, DeviceType::CAMERA);
                Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | meanLuma = " +
                                std::to_string(meanLuma),
                            LogLevel::INFO, DeviceType::CAMERA);
                Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | stdLuma = " +
                                std::to_string(stdLuma),
                            LogLevel::INFO, DeviceType::CAMERA);
                Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | lumaSampleCount = " +
                                std::to_string(lumaSamples),
                            LogLevel::INFO, DeviceType::CAMERA);
            } else {
                Logger::Log("GPM | color RAW luma stats unavailable, fallback to direct RAW stats",
                            LogLevel::WARNING, DeviceType::CAMERA);
            }
        }

        // black/white（等价于 Tools::GetAutoStretch(mode=0)，但不再额外扫描图像）
        {
            constexpr int a = 3;
            constexpr int b = 5;
            const long double stretchMean = static_cast<long double>(gpm.globalMean);
            const long double stretchStdDev = static_cast<long double>(gpm.globalStdDev);
            long double bx = stretchMean - stretchStdDev * a;
            long double wx = stretchMean + stretchStdDev * b;
            if (bx == wx) wx = bx + 10;

            const uint16_t maxValue = 65535;
            if (bx < 0) bx = 0;
            if (bx > maxValue) bx = maxValue;
            if (wx < 0) wx = 0;
            if (wx > maxValue) wx = maxValue;

            // 关键修复：
            // - bx/wx 可能是小数，直接 static_cast<uint16_t> 会截断为 0，导致 B==W==0（前端回退到 0-65535）
            // - 这里改为“先在整数域四舍五入”，并保证 white > black（至少相差 1）
            long long bi = std::llround(bx);
            long long wi = std::llround(wx);

            // 若区间过窄/反转，优先回退到 min/max（对低信号帧更稳）
            if (wi <= bi) {
                if (maxV > minV) {
                    bi = static_cast<long long>(minV);
                    wi = static_cast<long long>(maxV);
                } else {
                    wi = bi + 1;
                }
            }

            // 再次钳制到 16bit 范围，并保证 wi > bi
            bi = std::max<long long>(0, std::min<long long>(maxValue, bi));
            wi = std::max<long long>(0, std::min<long long>(maxValue, wi));
            if (wi <= bi) {
                wi = std::min<long long>(maxValue, bi + 1);
                if (wi <= bi) bi = std::max<long long>(0, wi - 1);
            }

            // 过曝保护：当画面整体接近饱和时，保留一个明显更亮的窗口，
            // 避免前端把“高亮满屏”拉成近黑。
            if (stretchMean >= static_cast<long double>(maxValue) * 0.85L ||
                minV >= static_cast<uint16_t>(maxValue * 0.75)) {
                wi = maxValue;
                bi = std::min<long long>(bi, maxValue / 2);
            }

            uint16_t B = static_cast<uint16_t>(bi);
            uint16_t W = static_cast<uint16_t>(wi);
            if (B >= maxValue && W >= maxValue) {
                B = 0;
                W = maxValue;
            }
            gpm.blackLevel = B;
            gpm.whiteLevel = W;
            Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | blackLevel = " +
                            std::to_string(gpm.blackLevel),
                        LogLevel::INFO, DeviceType::CAMERA);
            Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | whiteLevel = " +
                            std::to_string(gpm.whiteLevel),
                        LogLevel::INFO, DeviceType::CAMERA);
        }
    } else {
        // 兼容路径：非 16UC1 时，保留原策略（可按需扩展 8bit/3通道）
        cv::Scalar mean, stdDev;
        cv::meanStdDev(image16, mean, stdDev);
        gpm.globalMean = mean[0];
        gpm.globalStdDev = stdDev[0];

        double minVal, maxVal;
        cv::minMaxLoc(image16, &minVal, &maxVal);
        gpm.globalMin = minVal;
        gpm.globalMax = maxVal;

        uint16_t B = 0, W = 65535;
        Tools::GetAutoStretch(image16, 0, B, W);
        gpm.blackLevel = B;
        gpm.whiteLevel = W;
    }

    Logger::Log("GPM calculated: min=" + std::to_string(gpm.globalMin) + 
                ", max=" + std::to_string(gpm.globalMax) +
                ", mean=" + std::to_string(gpm.globalMean) +
                ", stdDev=" + std::to_string(gpm.globalStdDev) +
                ", blackLevel=" + std::to_string(gpm.blackLevel) +
                ", whiteLevel=" + std::to_string(gpm.whiteLevel),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | globalMinMax = " +
                    std::to_string(gpm.globalMin) + "," + std::to_string(gpm.globalMax),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | globalMeanStdDev = " +
                    std::to_string(gpm.globalMean) + "," + std::to_string(gpm.globalStdDev),
                LogLevel::INFO, DeviceType::CAMERA);

    return gpm;
}

QString MainWindow::buildTileSessionId(quint64 frameId)
{
    return QStringLiteral("live_%1").arg(QString::number(static_cast<qulonglong>(frameId)));
}

int MainWindow::currentTilePreviewBinning() const
{
    std::lock_guard<std::mutex> lk(tileFrameMutex);
    return std::max(1, tileFrame.previewBinningFactor);
}

int MainWindow::calculateTileLevelFromScale(double scale, int maxZoomLevel)
{
    // 与前端 App.vue 的 calculateTileLevel 保持一致（10 档离散映射，scale 越小越“放大”）
    const double MIN_SCALE = 0.1;
    const double MAX_SCALE = 1.0;
    const int LEVELS = 10; // 0.1~1.0 共 10 档

    if (maxZoomLevel <= 0) return 0;
    const double s = std::max(MIN_SCALE, std::min(MAX_SCALE, scale));
    const double denom = (MAX_SCALE - MIN_SCALE);
    const double t = (denom != 0.0) ? ((s - MIN_SCALE) / denom) : 0.0; // 0..1
    const int idx = static_cast<int>(std::lround(t * (LEVELS - 1)));    // 0..9
    const int invIdx = (LEVELS - 1) - idx;                              // 9..0
    const int z = static_cast<int>(std::lround((static_cast<double>(invIdx) / (LEVELS - 1)) * maxZoomLevel));
    return std::max(0, std::min(maxZoomLevel, z));
}

void MainWindow::scheduleViewportTileGeneration()
{
    // 合并请求：若已有任务在跑，仅标记 pending，结束后再跑一轮（使用最新视口参数）。
    // 旧任务会在生成循环中检测 tileViewportRequestSeq，并尽快自行退出。
    if (tileViewportGenInFlight.exchange(true))
    {
        tileViewportGenPending = true;
        return;
    }

    tileViewportGenPending = false;

    TileFrameState st;
    std::shared_ptr<cv::Mat> img;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
        img = tileFrameImage16;
    }
    if (!img || img->empty() || st.sessionId.isEmpty() || st.imageWidth <= 0 || st.imageHeight <= 0)
    {
        tileViewportGenInFlight = false;
        return;
    }

    const quint64 epoch = st.epoch;
    const quint64 requestSeq = tileViewportRequestSeq.load();
    const int budgetMs = std::max(1, tilePyramidFastBudgetMs);
    QPointer<MainWindow> self(this);

    QtConcurrent::run([self, epoch, requestSeq, budgetMs]() {
        if (!self) return;
        self->generateViewportTiles_Once(epoch, requestSeq, budgetMs);

        QMetaObject::invokeMethod(self, [self]() {
            if (!self) return;
            self->tileViewportGenInFlight = false;
            if (self->tileViewportGenPending.exchange(false))
            {
                self->scheduleViewportTileGeneration();
                return;
            }
            self->scheduleFullResTileCompletion();
        }, Qt::QueuedConnection);
    });
}

void MainWindow::scheduleFullResTileCompletion()
{
    if (tileBuildMode.trimmed() != QStringLiteral("pyramid")) {
        return;
    }
    if (tileViewportGenInFlight.load()) {
        tileFullResGenPending = true;
        return;
    }
    if (tileFullResGenInFlight.exchange(true))
    {
        tileFullResGenPending = true;
        return;
    }

    tileFullResGenPending = false;

    TileFrameState st;
    std::shared_ptr<cv::Mat> img;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
        img = tileFrameImage16;
    }
    if (!img || img->empty() || st.sessionId.isEmpty() || st.imageWidth <= 0 || st.imageHeight <= 0)
    {
        tileFullResGenInFlight = false;
        return;
    }

    const quint64 epoch = st.epoch;
    const quint64 requestSeq = tileViewportRequestSeq.load();
    const int budgetMs = std::max(1, tilePyramidFastBudgetMs);
    QPointer<MainWindow> self(this);

    QtConcurrent::run([self, epoch, requestSeq, budgetMs]() {
        if (!self) return;
        self->generateFullResTiles_Once(epoch, requestSeq, budgetMs);

        QMetaObject::invokeMethod(self, [self]() {
            if (!self) return;
            self->tileFullResGenInFlight = false;
            if (self->tileViewportGenInFlight.load()) {
                self->tileFullResGenPending = true;
                return;
            }
            if (self->tileFullResGenPending.exchange(false))
            {
                self->scheduleFullResTileCompletion();
            }
        }, Qt::QueuedConnection);
    });
}

void MainWindow::generateViewportTiles_Once(quint64 epoch, quint64 requestSeq, int budgetMs)
{
    TileFrameState st;
    std::shared_ptr<cv::Mat> img;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
        img = tileFrameImage16;
    }
    if (!img || img->empty()) return;
    if (st.epoch != epoch) return;
    if (tilePyramidEpoch.load() != epoch) return;
    if (tileViewportRequestSeq.load() != requestSeq) return;

    const double vx = tileViewportX.load();
    const double vy = tileViewportY.load();
    const double sc = tileViewportScale.load();

    const int W = st.imageWidth;
    const int H = st.imageHeight;
    const int T = (st.tileSize > 0) ? st.tileSize : 512;
    const int maxZ = std::max(0, st.maxZoomLevel);
    const int requestedTargetZ = tileViewportTargetZ.load();
    const int requestedMaxZCap = tileViewportMaxZCap.load();
    const int effectiveMaxZ = (requestedMaxZCap >= 0)
        ? std::max(0, std::min(maxZ, requestedMaxZCap))
        : maxZ;

    // 视口矩形（尽量与前端一致；aspect 使用默认 16:9）
    const double aspect = (tileViewportAspect > 0.1) ? tileViewportAspect : (16.0 / 9.0);
    const double visibleX = std::isfinite(vx) ? vx : (W / 2.0);
    const double visibleY = std::isfinite(vy) ? vy : (H / 2.0);
    // 前端 scale 连续缩放最小可到 0.01（但瓦片层级映射仍会把 <0.1 clamp 到 0.1）
    // 这里用于计算可见区域宽高，应允许更小的 scale；否则会出现“视口瓦片范围算错/为0”。
    const double MIN_VIEW_SCALE = 0.01;
    const double MAX_VIEW_SCALE = 1.0;
    const double scale = std::max(MIN_VIEW_SCALE, std::min(MAX_VIEW_SCALE, (std::isfinite(sc) ? sc : 1.0)));

    const double visibleWidth = W * scale;
    const double visibleHeight = (aspect != 0.0) ? (visibleWidth / aspect) : (H * scale);
    const int fallbackZ = std::min(calculateTileLevelFromScale(scale, maxZ), effectiveMaxZ);
    const int currentZ = (requestedTargetZ >= 0)
        ? std::max(0, std::min(effectiveMaxZ, requestedTargetZ))
        : fallbackZ;
    const bool forceFullImageForCappedMode = (requestedMaxZCap >= 0);
    Logger::Log("[TileDebug] event=generateViewportTiles_OnceBegin session=" + st.sessionId.toStdString() +
                    " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                    " epoch=" + std::to_string(static_cast<unsigned long long>(epoch)) +
                    " requestSeq=" + std::to_string(static_cast<unsigned long long>(requestSeq)) +
                    " requestedTargetZ=" + std::to_string(requestedTargetZ) +
                    " requestedMaxZCap=" + std::to_string(requestedMaxZCap) +
                    " effectiveMaxZ=" + std::to_string(effectiveMaxZ) +
                    " currentZ=" + std::to_string(currentZ) +
                    " scale=" + std::to_string(scale) +
                    " visibleCenter=" + std::to_string(visibleX) + "," + std::to_string(visibleY) +
                    " forceFullImageForCappedMode=" + std::string(forceFullImageForCappedMode ? "true" : "false"),
                LogLevel::INFO, DeviceType::CAMERA);

    QElapsedTimer timer;
    timer.start();

    const QString sessionTilePath = QString::fromStdString(tilePyramidPath) + st.sessionId;
    constexpr int TILE_BORDER = 2;

    struct TileReq { int x; int y; double prio; bool prefetch; };
    bool interruptedByBudget = false;
    auto shouldStop = [&]() -> bool {
        if (tilePyramidEpoch.load() != epoch) return true;
        if (tileViewportRequestSeq.load() != requestSeq) return true;
        if (budgetMs > 0 && timer.elapsed() > budgetMs) {
            interruptedByBudget = true;
            return true;
        }
        return false;
    };

    // 普通拍摄统一只围绕前端显式 targetZ 生成当前视口需要的层级；
    // z=0 整图预览已由 generateVisibleTilesSync() 提前准备。
    const bool minMaxOnly = (tileLevelMode.trimmed().toLower() == QStringLiteral("minmax"));
    const int coarseZ = std::max(0, currentZ - 1);
    std::vector<int> levelsToGenerate = minMaxOnly
        ? std::vector<int>{currentZ}
        : std::vector<int>{coarseZ, currentZ};
    std::sort(levelsToGenerate.begin(), levelsToGenerate.end());
    levelsToGenerate.erase(std::unique(levelsToGenerate.begin(), levelsToGenerate.end()), levelsToGenerate.end());
    const bool allowIdlePrefetch = !forceFullImageForCappedMode && currentZ > 0;
    constexpr int PREFETCH_RING = 1;

    int totalWritten = 0;
    QStringList readyTileKeys;
    std::set<uint64_t> doneKeys;
    bool completedVisibleTiles = true;
    bool completedAllWork = true;
    auto makeKey = [](int tz, int tx, int ty) -> uint64_t {
        return (static_cast<uint64_t>(tz) << 40) |
               (static_cast<uint64_t>(tx) << 20) |
               static_cast<uint64_t>(ty);
    };
    for (int z : levelsToGenerate)
    {
        if (shouldStop()) {
            completedVisibleTiles = false;
            completedAllWork = false;
            break;
        }

        const int levelScaleInt = 1 << std::max(0, (maxZ - z)); // 2^(maxZ-z)
        const double levelScale = static_cast<double>(levelScaleInt);
        const bool singlePreviewTile = (z == 0);

        // 与前端请求策略保持一致：
        // - z=0：整图单瓦片预览
        // - z>0：仅当前视口范围的高分辨率瓦片
        const bool fullImageForThisZ = (z == 0) || forceFullImageForCappedMode;
        const double left = fullImageForThisZ ? 0.0 : std::max(0.0, visibleX - visibleWidth / 2.0);
        const double top = fullImageForThisZ ? 0.0 : std::max(0.0, visibleY - visibleHeight / 2.0);
        const double right = fullImageForThisZ ? static_cast<double>(W) : std::min(static_cast<double>(W), left + visibleWidth);
        const double bottom = fullImageForThisZ ? static_cast<double>(H) : std::min(static_cast<double>(H), top + visibleHeight);

        const double levelLeft = left / levelScale;
        const double levelTop = top / levelScale;
        const double levelRight = right / levelScale;
        const double levelBottom = bottom / levelScale;

        const int levelWidth = static_cast<int>(std::ceil(W / levelScale));
        const int levelHeight = static_cast<int>(std::ceil(H / levelScale));
        const int maxTilesX = singlePreviewTile ? 1 : static_cast<int>(std::ceil(static_cast<double>(levelWidth) / T));
        const int maxTilesY = singlePreviewTile ? 1 : static_cast<int>(std::ceil(static_cast<double>(levelHeight) / T));
        const int startX = singlePreviewTile ? 0 : static_cast<int>(std::floor(levelLeft / T));
        const int startY = singlePreviewTile ? 0 : static_cast<int>(std::floor(levelTop / T));
        const int endX = singlePreviewTile ? 0 : static_cast<int>(std::ceil(levelRight / T) - 1.0);
        const int endY = singlePreviewTile ? 0 : static_cast<int>(std::ceil(levelBottom / T) - 1.0);
        const bool prefetchThisLevel = allowIdlePrefetch && (z == currentZ) && !singlePreviewTile;
        Logger::Log("[TileDebug] event=generateViewportTiles_OnceLevel session=" + st.sessionId.toStdString() +
                        " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                        " z=" + std::to_string(z) +
                        " fullImage=" + std::string(fullImageForThisZ ? "true" : "false") +
                        " tileRange=[" + std::to_string(startX) + "," + std::to_string(startY) +
                        "]-[" + std::to_string(endX) + "," + std::to_string(endY) + "]" +
                        " maxTiles=" + std::to_string(maxTilesX) + "x" + std::to_string(maxTilesY) +
                        " prefetch=" + std::string(prefetchThisLevel ? "ring1" : "none"),
                    LogLevel::DEBUG, DeviceType::CAMERA);

        std::vector<TileReq> tiles;
        const int prefetchStartX = prefetchThisLevel ? std::max(0, startX - PREFETCH_RING) : startX;
        const int prefetchStartY = prefetchThisLevel ? std::max(0, startY - PREFETCH_RING) : startY;
        const int prefetchEndX = prefetchThisLevel ? std::min(maxTilesX - 1, endX + PREFETCH_RING) : endX;
        const int prefetchEndY = prefetchThisLevel ? std::min(maxTilesY - 1, endY + PREFETCH_RING) : endY;
        tiles.reserve(static_cast<size_t>(std::max(0, (prefetchEndX - prefetchStartX + 1) * (prefetchEndY - prefetchStartY + 1))));

        const int cxTile = singlePreviewTile ? 0 : static_cast<int>(std::floor((visibleX / levelScale) / T));
        const int cyTile = singlePreviewTile ? 0 : static_cast<int>(std::floor((visibleY / levelScale) / T));

        for (int ty = prefetchStartY; ty <= prefetchEndY; ++ty)
        {
            if (ty < 0 || ty >= maxTilesY) continue;
            for (int tx = prefetchStartX; tx <= prefetchEndX; ++tx)
            {
                if (tx < 0 || tx >= maxTilesX) continue;
                const bool isPrimaryTile = (tx >= startX && tx <= endX && ty >= startY && ty <= endY);
                if (!isPrimaryTile && !prefetchThisLevel) continue;
                const double dx = static_cast<double>(tx - cxTile);
                const double dy = static_cast<double>(ty - cyTile);
                const double dist = (z == 0) ? 0.0 : std::sqrt(dx * dx + dy * dy);
                const double penalty = isPrimaryTile ? 0.0 : 10000.0;
                tiles.push_back({tx, ty, penalty + dist, !isPrimaryTile});
            }
        }

        std::sort(tiles.begin(), tiles.end(), [](const TileReq& a, const TileReq& b) {
            return a.prio < b.prio;
        });

        const QString zDirPath = sessionTilePath + "/" + QString::number(z);
        QDir().mkpath(zDirPath);

        for (const auto& t : tiles)
        {
            if (shouldStop()) {
                if (!t.prefetch) {
                    completedVisibleTiles = false;
                }
                completedAllWork = false;
                break;
            }

            // 预算续跑时从同一 requestSeq 重新进入本函数。
            // 同一 epoch 下，瓦片内容只由 frame/z/x/y 决定，与“本轮从哪个视口触发”无关；
            // 因此可以安全跳过已经生成完成的瓦片，避免每轮都从高优先级第一块重新开始，
            // 导致 capped/full-image 模式下长期只补出局部一小块。
            const uint64_t packedKey = makeKey(z, t.x, t.y);
            {
                std::lock_guard<std::mutex> lk(tileGenDoneMutex);
                if (tileGenDoneEpoch == epoch && tileGenDoneKeys.find(packedKey) != tileGenDoneKeys.end()) {
                    continue;
                }
            }

            const QString xDirPath = zDirPath + "/" + QString::number(t.x);
            QDir().mkpath(xDirPath);
            const QString tileFilePath = xDirPath + "/" + QString::number(t.y) + ".bin";

            // 每次视口生成都直接覆盖写，不保留旧的“已生成”去重记录，避免视口变化后沿用旧数据。
            const int x0 = singlePreviewTile ? 0 : (t.x * T);
            const int y0 = singlePreviewTile ? 0 : (t.y * T);
            const int coreWidth = singlePreviewTile ? levelWidth : T;
            const int coreHeight = singlePreviewTile ? levelHeight : T;
            const cv::Rect wantedLevel(x0 - TILE_BORDER, y0 - TILE_BORDER,
                                       coreWidth + 2 * TILE_BORDER, coreHeight + 2 * TILE_BORDER);
            const cv::Rect wantedOrig(wantedLevel.x * levelScaleInt,
                                      wantedLevel.y * levelScaleInt,
                                      wantedLevel.width * levelScaleInt,
                                      wantedLevel.height * levelScaleInt);
            const cv::Rect boundsOrig(0, 0, img->cols, img->rows);
            const cv::Rect srcRect = wantedOrig & boundsOrig;

            cv::Mat padded;
            if (srcRect.width <= 0 || srcRect.height <= 0)
            {
                padded = cv::Mat::zeros(wantedOrig.height, wantedOrig.width, img->type());
            }
            else
            {
                cv::Mat src = (*img)(srcRect);
                const int topPad = srcRect.y - wantedOrig.y;
                const int leftPad = srcRect.x - wantedOrig.x;
                const int bottomPad = (wantedOrig.y + wantedOrig.height) - (srcRect.y + srcRect.height);
                const int rightPad = (wantedOrig.x + wantedOrig.width) - (srcRect.x + srcRect.width);
                cv::copyMakeBorder(src, padded, topPad, bottomPad, leftPad, rightPad, cv::BORDER_REPLICATE);
            }

            cv::Mat tileLevel;
            if (levelScaleInt == 1)
            {
                tileLevel = padded;
            }
            else
            {
                tileLevel = downsampleTileImageForLevel(padded, st.cfa, levelScaleInt);
                if (tileLevel.cols != wantedLevel.width || tileLevel.rows != wantedLevel.height) {
                    Logger::Log("[TileDebug] event=tileSizeMismatchAfterDownsample session=" + st.sessionId.toStdString() +
                                    " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                                    " z=" + std::to_string(z) +
                                    " x=" + std::to_string(t.x) +
                                    " y=" + std::to_string(t.y) +
                                    " expected=" + std::to_string(wantedLevel.width) + "x" + std::to_string(wantedLevel.height) +
                                    " actual=" + std::to_string(tileLevel.cols) + "x" + std::to_string(tileLevel.rows),
                                LogLevel::WARNING, DeviceType::CAMERA);
                }
            }

            saveTileFast_NoMkdir(tileLevel, tileFilePath, TILE_BORDER);
            readyTileKeys.push_back(QString::number(z) + "/" + QString::number(t.x) + "/" + QString::number(t.y));
            doneKeys.insert(packedKey);
            totalWritten++;
        }
    }

    std::string levelSummary;
    levelSummary.clear();
    for (size_t i = 0; i < levelsToGenerate.size(); ++i)
    {
        if (i > 0) levelSummary += ",";
        levelSummary += std::to_string(levelsToGenerate[i]);
    }
    Logger::Log("[TileDebug] event=generateViewportTiles_OnceEnd session=" + st.sessionId.toStdString() +
               " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
               " wroteTiles=" + std::to_string(totalWritten) +
               " levels=" + levelSummary,
               LogLevel::DEBUG, DeviceType::CAMERA);
    if (!doneKeys.empty()) {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        if (tileGenDoneEpoch == epoch) {
            for (uint64_t key : doneKeys) tileGenDoneKeys.insert(key);
        }
    }
    if (!readyTileKeys.isEmpty()) {
        sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
    }
    if (completedVisibleTiles && tileBuildMode.trimmed() == QStringLiteral("merged_single_level")) {
        sendTileGenerationCompleteToClient(st.sessionId, epoch);
    }
    if (!completedAllWork &&
        interruptedByBudget &&
        tilePyramidEpoch.load() == epoch &&
        tileViewportRequestSeq.load() == requestSeq) {
        Logger::Log("[TileDebug] event=generateViewportTiles_OnceBudgetRerun session=" +
                        st.sessionId.toStdString() + " frameId=" +
                        std::to_string(static_cast<unsigned long long>(st.frameId)) + " epoch=" +
                        std::to_string(static_cast<unsigned long long>(epoch)),
                    LogLevel::INFO, DeviceType::CAMERA);
        tileViewportGenPending = true;
    }
}

void MainWindow::generateFullResTiles_Once(quint64 epoch, quint64 requestSeq, int budgetMs)
{
    TileFrameState st;
    std::shared_ptr<cv::Mat> img;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
        img = tileFrameImage16;
    }
    if (!img || img->empty()) return;
    if (st.epoch != epoch) return;
    if (tilePyramidEpoch.load() != epoch) return;

    if (tileBuildMode.trimmed() == QStringLiteral("merged_single_level")) {
        const int z = std::max(0, st.maxZoomLevel);
        const int T = (st.tileSize > 0) ? st.tileSize : 512;
        const int levelWidth = st.imageWidth;
        const int levelHeight = st.imageHeight;
        const int maxTilesX = static_cast<int>(std::ceil(static_cast<double>(levelWidth) / T));
        const int maxTilesY = static_cast<int>(std::ceil(static_cast<double>(levelHeight) / T));
        const QString sessionTilePath = QString::fromStdString(tilePyramidPath) + st.sessionId;
        const QString zDirPath = sessionTilePath + "/" + QString::number(z);
        constexpr int TILE_BORDER = 2;
        constexpr int READY_BATCH_SIZE = 32;

        auto makeKey = [](int tz, int tx, int ty) -> uint64_t {
            return (static_cast<uint64_t>(tz) << 40) |
                   (static_cast<uint64_t>(tx) << 20) |
                   static_cast<uint64_t>(ty);
        };

        if (!QDir().mkpath(zDirPath)) {
            Logger::Log("generateFullResTiles_Once: failed to mkpath " + zDirPath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
            return;
        }

        int written = 0;
        int skippedExisting = 0;
        QStringList readyTileKeys;
        readyTileKeys.reserve(READY_BATCH_SIZE);

        for (int ty = 0; ty < maxTilesY; ++ty)
        {
            if (tilePyramidEpoch.load() != epoch) return;
            for (int tx = 0; tx < maxTilesX; ++tx)
            {
                if (tilePyramidEpoch.load() != epoch) return;

                const uint64_t packedKey = makeKey(z, tx, ty);
                {
                    std::lock_guard<std::mutex> lk(tileGenDoneMutex);
                    if (tileGenDoneEpoch == epoch && tileGenDoneKeys.find(packedKey) != tileGenDoneKeys.end()) {
                        skippedExisting++;
                        continue;
                    }
                }

                const QString xDirPath = zDirPath + "/" + QString::number(tx);
                if (!QDir().mkpath(xDirPath)) {
                    continue;
                }
                const QString tileFilePath = xDirPath + "/" + QString::number(ty) + ".bin";

                const int x0 = tx * T;
                const int y0 = ty * T;
                const cv::Rect wantedLevel(x0 - TILE_BORDER, y0 - TILE_BORDER,
                                           T + 2 * TILE_BORDER, T + 2 * TILE_BORDER);
                const cv::Rect boundsOrig(0, 0, img->cols, img->rows);
                const cv::Rect srcRect = wantedLevel & boundsOrig;

                cv::Mat padded;
                if (srcRect.width <= 0 || srcRect.height <= 0)
                {
                    padded = cv::Mat::zeros(wantedLevel.height, wantedLevel.width, img->type());
                }
                else
                {
                    cv::Mat src = (*img)(srcRect);
                    const int topPad = srcRect.y - wantedLevel.y;
                    const int leftPad = srcRect.x - wantedLevel.x;
                    const int bottomPad = (wantedLevel.y + wantedLevel.height) - (srcRect.y + srcRect.height);
                    const int rightPad = (wantedLevel.x + wantedLevel.width) - (srcRect.x + srcRect.width);
                    cv::copyMakeBorder(src, padded, topPad, bottomPad, leftPad, rightPad, cv::BORDER_REPLICATE);
                }

                saveTileFast_NoMkdir(padded, tileFilePath, TILE_BORDER);

                {
                    std::lock_guard<std::mutex> lk(tileGenDoneMutex);
                    if (tileGenDoneEpoch == epoch) {
                        tileGenDoneKeys.insert(packedKey);
                    }
                }

                readyTileKeys.push_back(QString::number(z) + "/" + QString::number(tx) + "/" + QString::number(ty));
                written++;

                if (readyTileKeys.size() >= READY_BATCH_SIZE) {
                    sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
                    readyTileKeys.clear();
                }
            }
        }

        if (!readyTileKeys.isEmpty()) {
            sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
        }

        sendTileGenerationCompleteToClient(st.sessionId, epoch);

        Logger::Log("generateFullResTiles_Once: wrote " + std::to_string(written) +
                   " full-res tiles at z=" + std::to_string(z) +
                   " for session " + st.sessionId.toStdString() +
                   " (skippedExisting=" + std::to_string(skippedExisting) + ")",
                   LogLevel::DEBUG, DeviceType::CAMERA);
        return;
    }

    if (tileBuildMode.trimmed() != QStringLiteral("pyramid")) return;

    QElapsedTimer timer;
    timer.start();

    const int maxZ = std::max(0, st.maxZoomLevel);
    const int T = (st.tileSize > 0) ? st.tileSize : 512;
    const QString sessionTilePath = QString::fromStdString(tilePyramidPath) + st.sessionId;
    constexpr int TILE_BORDER = 2;
    constexpr int READY_BATCH_SIZE = 32;
    bool completedAllTiles = true;
    bool interruptedByViewport = false;
    bool interruptedByBudget = false;

    auto makeKey = [](int tz, int tx, int ty) -> uint64_t {
        return (static_cast<uint64_t>(tz) << 40) |
               (static_cast<uint64_t>(tx) << 20) |
               static_cast<uint64_t>(ty);
    };

    auto shouldStop = [&]() -> bool {
        if (tilePyramidEpoch.load() != epoch) return true;
        if (tileViewportRequestSeq.load() != requestSeq) {
            interruptedByViewport = true;
            return true;
        }
        if (budgetMs > 0 && timer.elapsed() > budgetMs) {
            interruptedByBudget = true;
            return true;
        }
        return false;
    };

    int written = 0;
    int skippedExisting = 0;
    QStringList readyTileKeys;
    readyTileKeys.reserve(READY_BATCH_SIZE);
    std::set<uint64_t> doneKeys;

    const bool minMaxOnly = (tileLevelMode.trimmed().toLower() == QStringLiteral("minmax"));
    std::vector<int> fullResLevels;
    if (minMaxOnly) {
        fullResLevels = {0, maxZ};
    } else {
        fullResLevels.reserve(maxZ + 1);
        for (int z = 0; z <= maxZ; ++z) fullResLevels.push_back(z);
    }
    std::sort(fullResLevels.begin(), fullResLevels.end());
    fullResLevels.erase(std::unique(fullResLevels.begin(), fullResLevels.end()), fullResLevels.end());

    for (int z : fullResLevels)
    {
        if (shouldStop()) {
            completedAllTiles = false;
            break;
        }
        const int levelScaleInt = 1 << std::max(0, (maxZ - z));
        const int levelWidth = static_cast<int>(std::ceil(static_cast<double>(st.imageWidth) / levelScaleInt));
        const int levelHeight = static_cast<int>(std::ceil(static_cast<double>(st.imageHeight) / levelScaleInt));
        const int maxTilesX = (levelWidth + T - 1) / T;
        const int maxTilesY = (levelHeight + T - 1) / T;
        const QString zDirPath = sessionTilePath + "/" + QString::number(z);
        if (!QDir().mkpath(zDirPath)) {
            Logger::Log("generateFullResTiles_Once: failed to mkpath " + zDirPath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
            completedAllTiles = false;
            break;
        }

        for (int ty = 0; ty < maxTilesY; ++ty)
        {
            if (shouldStop()) {
                completedAllTiles = false;
                break;
            }
            for (int tx = 0; tx < maxTilesX; ++tx)
            {
                if (shouldStop()) {
                    completedAllTiles = false;
                    break;
                }

                const uint64_t packedKey = makeKey(z, tx, ty);
                {
                    std::lock_guard<std::mutex> lk(tileGenDoneMutex);
                    if (tileGenDoneEpoch == epoch && tileGenDoneKeys.find(packedKey) != tileGenDoneKeys.end()) {
                        skippedExisting++;
                        continue;
                    }
                }

                const QString xDirPath = zDirPath + "/" + QString::number(tx);
                if (!QDir().mkpath(xDirPath)) {
                    completedAllTiles = false;
                    continue;
                }
                const QString tileFilePath = xDirPath + "/" + QString::number(ty) + ".bin";

                const int x0 = tx * T;
                const int y0 = ty * T;
                const cv::Rect wantedLevel(x0 - TILE_BORDER, y0 - TILE_BORDER,
                                           T + 2 * TILE_BORDER, T + 2 * TILE_BORDER);
                const cv::Rect wantedOrig(wantedLevel.x * levelScaleInt,
                                          wantedLevel.y * levelScaleInt,
                                          wantedLevel.width * levelScaleInt,
                                          wantedLevel.height * levelScaleInt);
                const cv::Rect boundsOrig(0, 0, img->cols, img->rows);
                const cv::Rect srcRect = wantedOrig & boundsOrig;

                cv::Mat padded;
                if (srcRect.width <= 0 || srcRect.height <= 0)
                {
                    padded = cv::Mat::zeros(wantedOrig.height, wantedOrig.width, img->type());
                }
                else
                {
                    cv::Mat src = (*img)(srcRect);
                    const int topPad = srcRect.y - wantedOrig.y;
                    const int leftPad = srcRect.x - wantedOrig.x;
                    const int bottomPad = (wantedOrig.y + wantedOrig.height) - (srcRect.y + srcRect.height);
                    const int rightPad = (wantedOrig.x + wantedOrig.width) - (srcRect.x + srcRect.width);
                    cv::copyMakeBorder(src, padded, topPad, bottomPad, leftPad, rightPad, cv::BORDER_REPLICATE);
                }

                cv::Mat tileLevel;
                if (levelScaleInt == 1)
                {
                    tileLevel = padded;
                }
                else
                {
                    tileLevel = downsampleTileImageForLevel(padded, st.cfa, levelScaleInt);
                }

                saveTileFast_NoMkdir(tileLevel, tileFilePath, TILE_BORDER);
                doneKeys.insert(packedKey);
                readyTileKeys.push_back(QString::number(z) + "/" + QString::number(tx) + "/" + QString::number(ty));
                written++;

                if (readyTileKeys.size() >= READY_BATCH_SIZE) {
                    {
                        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
                        if (tileGenDoneEpoch == epoch) {
                            for (uint64_t key : doneKeys) tileGenDoneKeys.insert(key);
                        }
                    }
                    doneKeys.clear();
                    sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
                    readyTileKeys.clear();
                }
            }
            if (!completedAllTiles) break;
        }
        if (!completedAllTiles) break;
    }

    if (!doneKeys.empty()) {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        if (tileGenDoneEpoch == epoch) {
            for (uint64_t key : doneKeys) tileGenDoneKeys.insert(key);
        }
    }
    if (!readyTileKeys.isEmpty()) {
        sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
    }
    if (completedAllTiles) {
        sendTileGenerationCompleteToClient(st.sessionId, epoch);
    } else if (tilePyramidEpoch.load() == epoch) {
        tileFullResGenPending = true;
    }

    Logger::Log("generateFullResTiles_Once: wrote " + std::to_string(written) +
               " pyramid tiles for session " + st.sessionId.toStdString() +
               " (skippedExisting=" + std::to_string(skippedExisting) +
               ", interruptedByViewport=" + std::string(interruptedByViewport ? "true" : "false") +
               ", interruptedByBudget=" + std::string(interruptedByBudget ? "true" : "false") + ")",
               LogLevel::DEBUG, DeviceType::CAMERA);
}

void MainWindow::generateVisibleTilesSync(quint64 epoch, bool includeViewportLevels)
{
    TileFrameState st;
    std::shared_ptr<cv::Mat> img;
    std::shared_ptr<cv::Mat> previewImg;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
        img = tileFrameImage16;
        previewImg = tileFramePreviewImage16;
    }
    if (!img || img->empty() || st.sessionId.isEmpty() || st.imageWidth <= 0 || st.imageHeight <= 0) return;
    if (st.epoch != epoch || tilePyramidEpoch.load() != epoch) return;

    const int W = st.imageWidth;
    const int H = st.imageHeight;
    const int T = (st.tileSize > 0) ? st.tileSize : 512;
    const int maxZ = std::max(0, st.maxZoomLevel);

    // 视口：无有效视口时按整图（scale=1）处理，即 z=0 全层
    const double vx = tileViewportX.load();
    const double vy = tileViewportY.load();
    const double sc = tileViewportScale.load();
    const int requestedTargetZ = tileViewportTargetZ.load();
    const int requestedMaxZCap = tileViewportMaxZCap.load();
    const int effectiveMaxZ = (requestedMaxZCap >= 0)
        ? std::max(0, std::min(maxZ, requestedMaxZCap))
        : maxZ;
    const double aspect = (tileViewportAspect > 0.1) ? tileViewportAspect : (16.0 / 9.0);
    const double visibleX = std::isfinite(vx) ? vx : (W / 2.0);
    const double visibleY = std::isfinite(vy) ? vy : (H / 2.0);
    const double MIN_VIEW_SCALE = 0.01;
    const double MAX_VIEW_SCALE = 1.0;
    const double scale = std::isfinite(sc) && sc > 0
        ? std::max(MIN_VIEW_SCALE, std::min(MAX_VIEW_SCALE, sc))
        : 1.0;
    const int fallbackZ = std::min(calculateTileLevelFromScale(scale, maxZ), effectiveMaxZ);
    const int currentZ = (requestedTargetZ >= 0)
        ? std::max(0, std::min(effectiveMaxZ, requestedTargetZ))
        : fallbackZ;
    const bool forceFullImageForCappedMode = (requestedMaxZCap >= 0);
    Logger::Log("[TileDebug] event=generateVisibleTilesSyncBegin session=" + st.sessionId.toStdString() +
                    " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                    " epoch=" + std::to_string(static_cast<unsigned long long>(epoch)) +
                    " requestedTargetZ=" + std::to_string(requestedTargetZ) +
                    " scale=" + std::to_string(scale) +
                    " visibleCenter=" + std::to_string(visibleX) + "," + std::to_string(visibleY) +
                    " currentZ=" + std::to_string(currentZ) +
                    " requestedMaxZCap=" + std::to_string(requestedMaxZCap),
                LogLevel::INFO, DeviceType::CAMERA);
    // 同步预生成阶段：
    // - 默认只准备 z=0 整图预览，优先保证 TileGPM + 首图尽快出现
    // - 若显式要求，再额外同步准备 targetZ-1 / targetZ 当前视口层
    const bool minMaxOnly = (tileLevelMode.trimmed().toLower() == QStringLiteral("minmax"));
    std::vector<int> levelsToSync = {0};
    if (includeViewportLevels) {
        if (!minMaxOnly) {
            levelsToSync.push_back(std::max(0, currentZ - 1));
        }
        levelsToSync.push_back(currentZ);
    }
    std::sort(levelsToSync.begin(), levelsToSync.end());
    levelsToSync.erase(std::unique(levelsToSync.begin(), levelsToSync.end()), levelsToSync.end());

    const QString sessionTilePath = QString::fromStdString(tilePyramidPath) + st.sessionId;
    constexpr int TILE_BORDER = 2;
    auto makeKey = [](int tz, int tx, int ty) -> uint64_t {
        return (static_cast<uint64_t>(tz) << 40) |
               (static_cast<uint64_t>(tx) << 20) |
               static_cast<uint64_t>(ty);
    };
    {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        tileGenDoneEpoch = epoch;
        tileGenDoneKeys.clear();
        tileGenCompleteEpoch = 0;
    }
    int totalCount = 0;
    QStringList readyTileKeys;
    const double visibleWidth = W * scale;
    const double visibleHeight = (aspect != 0.0) ? (visibleWidth / aspect) : (H * scale);

    for (int z : levelsToSync) {
        if (tilePyramidEpoch.load() != epoch) return;
        const int levelScaleInt = 1 << std::max(0, (maxZ - z));
        const double levelScale = static_cast<double>(levelScaleInt);
        const bool singlePreviewTile = (z == 0);
        const bool fullImage = singlePreviewTile || forceFullImageForCappedMode;
        const double left = fullImage ? 0.0 : std::max(0.0, visibleX - visibleWidth / 2.0);
        const double top = fullImage ? 0.0 : std::max(0.0, visibleY - visibleHeight / 2.0);
        const double right = fullImage ? static_cast<double>(W) : std::min(static_cast<double>(W), left + visibleWidth);
        const double bottom = fullImage ? static_cast<double>(H) : std::min(static_cast<double>(H), top + visibleHeight);
        const double levelLeft = left / levelScale;
        const double levelTop = top / levelScale;
        const double levelRight = right / levelScale;
        const double levelBottom = bottom / levelScale;
        const int levelWidth = static_cast<int>(std::ceil(static_cast<double>(W) / levelScale));
        const int levelHeight = static_cast<int>(std::ceil(static_cast<double>(H) / levelScale));
        const int maxTilesX = singlePreviewTile ? 1 : static_cast<int>(std::ceil(static_cast<double>(levelWidth) / T));
        const int maxTilesY = singlePreviewTile ? 1 : static_cast<int>(std::ceil(static_cast<double>(levelHeight) / T));
        const int startX = singlePreviewTile ? 0 : static_cast<int>(std::floor(levelLeft / T));
        const int startY = singlePreviewTile ? 0 : static_cast<int>(std::floor(levelTop / T));
        const int endX = singlePreviewTile ? 0 : static_cast<int>(std::ceil(levelRight / T) - 1.0);
        const int endY = singlePreviewTile ? 0 : static_cast<int>(std::ceil(levelBottom / T) - 1.0);
        Logger::Log("[TileDebug] event=generateVisibleTilesSyncLevel session=" + st.sessionId.toStdString() +
                        " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                        " z=" + std::to_string(z) +
                        " fullImage=" + std::string(fullImage ? "true" : "false") +
                        " tileRange=[" + std::to_string(startX) + "," + std::to_string(startY) +
                        "]-[" + std::to_string(endX) + "," + std::to_string(endY) + "]" +
                        " maxTiles=" + std::to_string(maxTilesX) + "x" + std::to_string(maxTilesY),
                    LogLevel::DEBUG, DeviceType::CAMERA);

        const QString zDirPath = sessionTilePath + "/" + QString::number(z);
        if (!QDir().mkpath(zDirPath)) {
            Logger::Log("generateVisibleTilesSync: failed to mkpath " + zDirPath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
            continue;
        }
        std::set<uint64_t> doneKeys;
        for (int ty = startY; ty <= endY; ++ty) {
            if (ty < 0 || ty >= maxTilesY) continue;
            for (int tx = startX; tx <= endX; ++tx) {
                if (tx < 0 || tx >= maxTilesX) continue;
                if (tilePyramidEpoch.load() != epoch) return;
                const uint64_t key = makeKey(z, tx, ty);
                doneKeys.insert(key);
                totalCount++;
                const QString xDirPath = zDirPath + "/" + QString::number(tx);
                if (!QDir().mkpath(xDirPath)) continue;
                const QString tileFilePath = xDirPath + "/" + QString::number(ty) + ".bin";

                if (singlePreviewTile && previewImg && !previewImg->empty()) {
                    cv::Mat previewTile;
                    cv::copyMakeBorder(*previewImg, previewTile,
                                       TILE_BORDER, TILE_BORDER, TILE_BORDER, TILE_BORDER,
                                       cv::BORDER_REPLICATE);
                    saveTileFast_NoMkdir(previewTile, tileFilePath, TILE_BORDER);
                    const QString readyKey =
                        QString::number(z) + "/" + QString::number(tx) + "/" + QString::number(ty);
                    // 首屏体验优化：z=0 预览瓦片一旦原子写完，立即单独放行给前端。
                    // 局部放大的清晰细节仍由后续 currentZ-1/currentZ 瓦片继续覆盖，不受影响。
                    sendTileBatchReadyToClient(st.sessionId, epoch, QStringList{readyKey});
                    continue;
                }

                const int x0 = singlePreviewTile ? 0 : (tx * T);
                const int y0 = singlePreviewTile ? 0 : (ty * T);
                const int coreWidth = singlePreviewTile ? levelWidth : T;
                const int coreHeight = singlePreviewTile ? levelHeight : T;
                const cv::Rect wantedLevel(x0 - TILE_BORDER, y0 - TILE_BORDER,
                                           coreWidth + 2 * TILE_BORDER, coreHeight + 2 * TILE_BORDER);
                const cv::Rect wantedOrig(wantedLevel.x * levelScaleInt,
                                          wantedLevel.y * levelScaleInt,
                                          wantedLevel.width * levelScaleInt,
                                          wantedLevel.height * levelScaleInt);
                const cv::Rect boundsOrig(0, 0, img->cols, img->rows);
                const cv::Rect srcRect = wantedOrig & boundsOrig;

                cv::Mat padded;
                if (srcRect.width <= 0 || srcRect.height <= 0) {
                    padded = cv::Mat::zeros(wantedOrig.height, wantedOrig.width, img->type());
                } else {
                    cv::Mat src = (*img)(srcRect);
                    const int topPad = srcRect.y - wantedOrig.y;
                    const int leftPad = srcRect.x - wantedOrig.x;
                    const int bottomPad = (wantedOrig.y + wantedOrig.height) - (srcRect.y + srcRect.height);
                    const int rightPad = (wantedOrig.x + wantedOrig.width) - (srcRect.x + srcRect.width);
                    cv::copyMakeBorder(src, padded, topPad, bottomPad, leftPad, rightPad, cv::BORDER_REPLICATE);
                }
                cv::Mat tileLevel;
                if (levelScaleInt == 1) {
                    tileLevel = padded;
                } else {
                    tileLevel = downsampleTileImageForLevel(padded, st.cfa, levelScaleInt);
                    if (tileLevel.cols != wantedLevel.width || tileLevel.rows != wantedLevel.height) {
                        Logger::Log("[TileDebug] event=tileSizeMismatchAfterDownsample session=" + st.sessionId.toStdString() +
                                        " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                                        " z=" + std::to_string(z) +
                                        " x=" + std::to_string(tx) +
                                        " y=" + std::to_string(ty) +
                                        " expected=" + std::to_string(wantedLevel.width) + "x" + std::to_string(wantedLevel.height) +
                                        " actual=" + std::to_string(tileLevel.cols) + "x" + std::to_string(tileLevel.rows),
                                    LogLevel::WARNING, DeviceType::CAMERA);
                    }
                }
                saveTileFast_NoMkdir(tileLevel, tileFilePath, TILE_BORDER);
                readyTileKeys.push_back(QString::number(z) + "/" + QString::number(tx) + "/" + QString::number(ty));
            }
        }
        {
            std::lock_guard<std::mutex> lk(tileGenDoneMutex);
            for (uint64_t k : doneKeys) tileGenDoneKeys.insert(k);
        }
    }
    std::string levelSummary;
    levelSummary.clear();
    for (size_t i = 0; i < levelsToSync.size(); ++i)
    {
        if (i > 0) levelSummary += ",";
        levelSummary += std::to_string(levelsToSync[i]);
    }
    Logger::Log("[TileDebug] event=generateVisibleTilesSyncEnd session=" + st.sessionId.toStdString() +
               " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
               " wroteTiles=" + std::to_string(totalCount) +
               " levels=" + levelSummary,
               LogLevel::DEBUG, DeviceType::CAMERA);
    if (!readyTileKeys.isEmpty()) {
        sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
    }
}

void MainWindow::saveTile(const cv::Mat& tile, int z, int x, int y, const QString& sessionId, int border)
{
    // 构建瓦片存储路径: tiles/{sessionId}/{z}/{x}/{y}.bin
    QString tileDirPath = QString::fromStdString(tilePyramidPath) + sessionId + "/" + 
                          QString::number(z) + "/" + QString::number(x) + "/";
    QString tileFilePath = tileDirPath + QString::number(y) + ".bin";

    // 创建目录
    QDir dir;
    if (!dir.mkpath(tileDirPath)) {
        Logger::Log("Failed to create tile directory: " + tileDirPath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    // 保存瓦片为二进制文件 (原始16位数据)
    std::ofstream outFile(tileFilePath.toStdString(), std::ios::binary);
    if (!outFile) {
        Logger::Log("Failed to open tile file for writing: " + tileFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    // 写入瓦片元数据头 (16字节)
    int32_t width = tile.cols;
    int32_t height = tile.rows;
    int32_t type = tile.type();  // CV_16UC1
    // reserved：用于向前端传递“额外边界像素数”，前端会在去马赛克/拉伸后裁剪掉该边界
    int32_t reserved = border;
    outFile.write(reinterpret_cast<const char*>(&width), sizeof(int32_t));
    outFile.write(reinterpret_cast<const char*>(&height), sizeof(int32_t));
    outFile.write(reinterpret_cast<const char*>(&type), sizeof(int32_t));
    outFile.write(reinterpret_cast<const char*>(&reserved), sizeof(int32_t));

    // 写入瓦片像素数据
    if (tile.isContinuous()) {
        outFile.write(reinterpret_cast<const char*>(tile.data), 
                      static_cast<std::streamsize>(tile.total() * tile.elemSize()));
    } else {
        for (int r = 0; r < tile.rows; ++r) {
            outFile.write(reinterpret_cast<const char*>(tile.ptr(r)), 
                          static_cast<std::streamsize>(tile.cols * tile.elemSize()));
        }
    }

    outFile.close();
}

void MainWindow::saveTileFast_NoMkdir(const cv::Mat& tile, const QString& tileFilePath, int border)
{
    const bool isZ0Tile =
        tileFilePath.contains(QStringLiteral("/0/0/0.bin")) ||
        tileFilePath.endsWith(QStringLiteral("\\0\\0\\0.bin"));
    const qint64 z0WriteStartMs = isZ0Tile ? QDateTime::currentMSecsSinceEpoch() : 0;
    if (isZ0Tile) {
        emitCaptureTrace(QStringLiteral("backend_z0_tile_write_start"), currentCaptureTraceStartedAtMs,
                         QString("path=%1,width=%2,height=%3")
                             .arg(tileFilePath)
                             .arg(tile.cols)
                             .arg(tile.rows));
    }

    // 原子写入：避免前端 fetch 在文件写入中途读到“半瓦片”，导致解析失败/花屏/长时间不刷新。
    // QSaveFile 会写入临时文件，commit 时原子替换目标文件（同一文件系统内）。
    QSaveFile file(tileFilePath);
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        Logger::Log("Failed to open tile file for writing: " + tileFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);

    const int32_t width = tile.cols;
    const int32_t height = tile.rows;
    const int32_t type = tile.type();
    const int32_t reserved = border;
    out << width;
    out << height;
    out << type;
    out << reserved;

    if (tile.isContinuous()) {
        const qint64 bytes = static_cast<qint64>(tile.total() * tile.elemSize());
        if (file.write(reinterpret_cast<const char*>(tile.data), bytes) != bytes) {
            Logger::Log("Failed to write tile bytes: " + tileFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
            file.cancelWriting();
            return;
        }
    } else {
        const qint64 rowBytes = static_cast<qint64>(tile.cols * tile.elemSize());
        for (int r = 0; r < tile.rows; ++r) {
            if (file.write(reinterpret_cast<const char*>(tile.ptr(r)), rowBytes) != rowBytes) {
                Logger::Log("Failed to write tile row bytes: " + tileFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
                file.cancelWriting();
                return;
            }
        }
    }

    if (!file.commit()) {
        Logger::Log("Failed to commit tile file (atomic replace): " + tileFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    if (isZ0Tile) {
        emitCaptureTrace(QStringLiteral("backend_z0_tile_write_done"), z0WriteStartMs,
                         QString("path=%1,width=%2,height=%3,fileBytes=%4")
                             .arg(tileFilePath)
                             .arg(tile.cols)
                             .arg(tile.rows)
                             .arg(static_cast<qint64>(QFileInfo(tileFilePath).size())));
    }
}

MainWindow::TileGPM MainWindow::generateTilePyramid(const cv::Mat& image16, const QString& sessionId, const QString& cfa, int maxMergeFactor, bool enableHistogram)
{
    Logger::Log("Starting tile pyramid generation for session: " + sessionId.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    // 取消机制：新帧到来时 tilePyramidEpoch 会递增，旧任务应尽快退出
    const quint64 epochAtStart = tilePyramidEpoch.load();
    QElapsedTimer budgetTimer;
    budgetTimer.start();
    const int budgetMs = std::max(0, tilePyramidFastBudgetMs);

    // 1. 计算GPM
    TileGPM gpm = calculateGPM(image16, cfa, maxMergeFactor, enableHistogram);
    gpm.sessionId = sessionId;
    Logger::Log("Tile pyramid | step GPM done", LogLevel::INFO, DeviceType::CAMERA);

    // 2. 确保 live 目录存在（不删除，直接覆盖写瓦片，避免 SD 卡/磁盘抖动）
    QString sessionTilePath = QString::fromStdString(tilePyramidPath) + sessionId;
    QDir().mkpath(sessionTilePath);
    Logger::Log("Tile pyramid | step session dir mkpath done (overwrite mode)", LogLevel::INFO, DeviceType::CAMERA);

    // 3. 生成各层级瓦片
    // 反向金字塔：level 0是最低精度（16x16合并），maxZoomLevel是原图
    const int T = tilePyramidTileSize;
    
    // 首先生成所有层级的图像（从最高精度到最低精度）
    std::vector<cv::Mat> pyramidLevels;
    pyramidLevels.resize(gpm.maxZoomLevel + 1);
    
    // 最高层级是原图
    pyramidLevels[gpm.maxZoomLevel] = image16.clone();
    Logger::Log("Tile pyramid | clone top level " + std::to_string(image16.cols) + "x" + std::to_string(image16.rows), LogLevel::INFO, DeviceType::CAMERA);
    
    // 从高精度向低精度生成（每次合并2x2像素为1像素）
    for (int z = gpm.maxZoomLevel - 1; z >= 0; --z) {
        const cv::Mat& higherLevel = pyramidLevels[z + 1];
        cv::Mat lowerLevel;

        // 彩色 RAW 必须保持 CFA 相位，不能直接在 Bayer 单通道上做普通面积缩小。
        lowerLevel = downsampleTileImageForLevel(higherLevel, cfa, 2);
        pyramidLevels[z] = lowerLevel;
    }
    Logger::Log("Tile pyramid | build pyramid levels (resize chain) done", LogLevel::INFO, DeviceType::CAMERA);
    
    // 现在生成每个层级的瓦片
    const int requestedSyncMaxZ = std::max(0, tilePyramidFastSyncMaxZ);
    const int syncMaxZ = std::min(requestedSyncMaxZ, gpm.maxZoomLevel);
    int writtenMaxZ = -1;

    for (int z = 0; z <= gpm.maxZoomLevel; ++z) {
        // 预算与取消：同步阶段只写到 syncMaxZ，且尽量把耗时控制在 budgetMs 内
        if (z > syncMaxZ) break;
        if (budgetMs > 0 && budgetTimer.elapsed() > budgetMs) break;
        if (tilePyramidEpoch.load() != epochAtStart) {
            Logger::Log("Tile pyramid | cancelled by newer epoch (sync phase)", LogLevel::WARNING, DeviceType::CAMERA);
            return gpm;
        }

        const cv::Mat& currentLevel = pyramidLevels[z];
        int levelWidth = currentLevel.cols;
        int levelHeight = currentLevel.rows;
        int tilesX = (levelWidth + T - 1) / T;  // 向上取整
        int tilesY = (levelHeight + T - 1) / T;
        int tileCount = tilesX * tilesY;
        
        // 计算当前层级的合并倍数
        int mergeFactor = 1 << (gpm.maxZoomLevel - z);  // 2^(maxZoomLevel - z)

        Logger::Log("Generating level " + std::to_string(z) + " (merge factor " + std::to_string(mergeFactor) + "x" + std::to_string(mergeFactor) + "): " + 
                    std::to_string(levelWidth) + "x" + std::to_string(levelHeight) + 
                    ", tiles: " + std::to_string(tilesX) + "x" + std::to_string(tilesY) + " (" + std::to_string(tileCount) + " total)",
                    LogLevel::INFO, DeviceType::CAMERA);

        // 生成当前层级的所有瓦片
        // 为了让前端“按瓦片局部去马赛克(Bayer->RGBA)”不出现接缝，
        // 这里为每个瓦片额外带一点邻域边界像素（前端渲染时会裁掉）。
        // 说明：边界像素来自相邻区域，超出图像边界时用 BORDER_REPLICATE 补齐。
        constexpr int TILE_BORDER = 2; // 像素；2 对 OpenCV Bayer bilinear 去马赛克更稳
        // 目录预创建：按层级 z 和列 tx 创建目录，避免每个瓦片都 mkpath（系统调用成本高）
        const QString zDirPath = sessionTilePath + "/" + QString::number(z);
        QDir().mkpath(zDirPath);
        for (int tx = 0; tx < tilesX; ++tx) {
            const QString xDirPath = zDirPath + "/" + QString::number(tx);
            QDir().mkpath(xDirPath);
            for (int ty = 0; ty < tilesY; ++ty) {
                QString tileFilePath = xDirPath + "/" + QString::number(ty) + ".bin";

                // 计算瓦片在当前层级的像素范围
                int x0 = tx * T;
                int y0 = ty * T;
                int x1 = std::min(x0 + T, levelWidth);
                int y1 = std::min(y0 + T, levelHeight);

                // 目标瓦片（含边界）的期望区域：以 [x0,y0] 为内核左上角，外扩 TILE_BORDER
                const cv::Rect wanted(x0 - TILE_BORDER, y0 - TILE_BORDER, T + 2 * TILE_BORDER, T + 2 * TILE_BORDER);
                const cv::Rect bounds(0, 0, levelWidth, levelHeight);
                const cv::Rect srcRect = wanted & bounds;

                if (srcRect.width <= 0 || srcRect.height <= 0) {
                    // 极端情况：理论上不会发生（除非图像尺寸为0）
                    cv::Mat emptyTile = cv::Mat::zeros(T + 2 * TILE_BORDER, T + 2 * TILE_BORDER, currentLevel.type());
                    saveTileFast_NoMkdir(emptyTile, tileFilePath, TILE_BORDER);
                    continue;
                }

                // 先取交集区域，再用 copyMakeBorder 补齐到 wanted 的大小（用边界复制，避免黑边）
                cv::Mat src = currentLevel(srcRect);
                const int top = srcRect.y - wanted.y;
                const int left = srcRect.x - wanted.x;
                const int bottom = (wanted.y + wanted.height) - (srcRect.y + srcRect.height);
                const int right = (wanted.x + wanted.width) - (srcRect.x + srcRect.width);

                cv::Mat tileWithBorder;
                cv::copyMakeBorder(src, tileWithBorder, top, bottom, left, right, cv::BORDER_REPLICATE);

                // 确保尺寸正确（防御性）
                if (tileWithBorder.cols != (T + 2 * TILE_BORDER) || tileWithBorder.rows != (T + 2 * TILE_BORDER)) {
                    cv::resize(tileWithBorder, tileWithBorder, cv::Size(T + 2 * TILE_BORDER, T + 2 * TILE_BORDER), 0, 0, cv::INTER_NEAREST);
                }

                // 保存瓦片（目录已预创建，使用 NoMkdir 写入）
                saveTileFast_NoMkdir(tileWithBorder, tileFilePath, TILE_BORDER);
            }
        }
        Logger::Log("Tile pyramid | level " + std::to_string(z) + " done, " + std::to_string(tileCount) + " tiles written", LogLevel::INFO, DeviceType::CAMERA);
        writtenMaxZ = z;
    }

    // 后台补齐剩余层级（避免同步阶段长时间占用 CPU/IO，影响 WS/状态机）
    if (writtenMaxZ < gpm.maxZoomLevel && tilePyramidEpoch.load() == epochAtStart) {
        const int bgStartZ = std::max(0, writtenMaxZ + 1);
        const int bgEndZ = gpm.maxZoomLevel;
        const int tileSizeCopy = tilePyramidTileSize;
        const QString sessionTilePathCopy = sessionTilePath;
        const QString sessionIdCopy = sessionId;
        const quint64 epochCopy = epochAtStart;
        const cv::Mat imageShare = image16; // 共享底层数据（ref-count），避免 clone 大图

        QPointer<MainWindow> self(this);
        QtConcurrent::run([self, epochCopy, imageShare, sessionTilePathCopy, sessionIdCopy, tileSizeCopy, bgStartZ, bgEndZ, gpm, cfa, maxMergeFactor]() mutable {
            if (!self) return;
            if (self->tilePyramidEpoch.load() != epochCopy) return;

            // 重新构建金字塔层（避免捕获 pyramidLevels 导致大内存常驻）
            std::vector<cv::Mat> levels;
            levels.resize(gpm.maxZoomLevel + 1);
            // 共享底层 buffer（cv::Mat 引用计数）；只读场景线程安全
            levels[gpm.maxZoomLevel] = imageShare;
            for (int z = gpm.maxZoomLevel - 1; z >= 0; --z) {
                const cv::Mat& higher = levels[z + 1];
                cv::Mat lower = downsampleTileImageForLevel(higher, cfa, 2);
                levels[z] = std::move(lower);
                if (self->tilePyramidEpoch.load() != epochCopy) return;
            }

            const int Tbg = tileSizeCopy;
            constexpr int TILE_BORDER = 2;
            for (int z = bgStartZ; z <= bgEndZ; ++z) {
                if (self->tilePyramidEpoch.load() != epochCopy) return;
                const cv::Mat& currentLevel = levels[z];
                int levelWidth = currentLevel.cols;
                int levelHeight = currentLevel.rows;
                int tilesX = (levelWidth + Tbg - 1) / Tbg;
                int tilesY = (levelHeight + Tbg - 1) / Tbg;

                const QString zDirPath = sessionTilePathCopy + "/" + QString::number(z);
                QDir().mkpath(zDirPath);
                for (int tx = 0; tx < tilesX; ++tx) {
                    if (self->tilePyramidEpoch.load() != epochCopy) return;
                    const QString xDirPath = zDirPath + "/" + QString::number(tx);
                    QDir().mkpath(xDirPath);
                    for (int ty = 0; ty < tilesY; ++ty) {
                        QString tileFilePath = xDirPath + "/" + QString::number(ty) + ".bin";
                        int x0 = tx * Tbg;
                        int y0 = ty * Tbg;
                        const cv::Rect wanted(x0 - TILE_BORDER, y0 - TILE_BORDER, Tbg + 2 * TILE_BORDER, Tbg + 2 * TILE_BORDER);
                        const cv::Rect bounds(0, 0, levelWidth, levelHeight);
                        const cv::Rect srcRect = wanted & bounds;

                        cv::Mat tileWithBorder;
                        if (srcRect.width <= 0 || srcRect.height <= 0) {
                            tileWithBorder = cv::Mat::zeros(Tbg + 2 * TILE_BORDER, Tbg + 2 * TILE_BORDER, currentLevel.type());
                        } else {
                            cv::Mat src = currentLevel(srcRect);
                            const int top = srcRect.y - wanted.y;
                            const int left = srcRect.x - wanted.x;
                            const int bottom = (wanted.y + wanted.height) - (srcRect.y + srcRect.height);
                            const int right = (wanted.x + wanted.width) - (srcRect.x + srcRect.width);
                            cv::copyMakeBorder(src, tileWithBorder, top, bottom, left, right, cv::BORDER_REPLICATE);
                            if (tileWithBorder.cols != (Tbg + 2 * TILE_BORDER) || tileWithBorder.rows != (Tbg + 2 * TILE_BORDER)) {
                                cv::resize(tileWithBorder, tileWithBorder, cv::Size(Tbg + 2 * TILE_BORDER, Tbg + 2 * TILE_BORDER), 0, 0, cv::INTER_NEAREST);
                            }
                        }
                        self->saveTileFast_NoMkdir(tileWithBorder, tileFilePath, TILE_BORDER);
                    }
                }
            }
        });

        Logger::Log("Tile pyramid | sync wrote z<= " + std::to_string(writtenMaxZ) +
                        ", background will write z=" + std::to_string(bgStartZ) + ".." + std::to_string(bgEndZ),
                    LogLevel::INFO, DeviceType::CAMERA);
    }

    // 4. 保存GPM元数据文件
    QString gpmFilePath = sessionTilePath + "/gpm.json";
    QJsonObject gpmJson;
    gpmJson["imageWidth"] = gpm.imageWidth;
    gpmJson["imageHeight"] = gpm.imageHeight;
    gpmJson["tileSize"] = gpm.tileSize;
    gpmJson["maxZoomLevel"] = gpm.maxZoomLevel;
    gpmJson["globalMin"] = gpm.globalMin;
    gpmJson["globalMax"] = gpm.globalMax;
    gpmJson["globalMean"] = gpm.globalMean;
    gpmJson["globalStdDev"] = gpm.globalStdDev;
    gpmJson["blackLevel"] = gpm.blackLevel;
    gpmJson["whiteLevel"] = gpm.whiteLevel;
    gpmJson["cfa"] = gpm.cfa;
    gpmJson["gainR"] = gpm.gainR;
    gpmJson["gainB"] = gpm.gainB;
    gpmJson["sessionId"] = gpm.sessionId;
    // frameId 可能超过 JSON number 的安全整数范围（JS 2^53），这里按字符串写入更安全
    gpmJson["frameId"] = QString::number(static_cast<qulonglong>(gpm.frameId));
    gpmJson["buildMode"] = gpm.buildMode;
    gpmJson["levelMode"] = gpm.levelMode;

        // 直方图（可选）
        // 注意：完整 65536 bins 写入 JSON 会非常大；这里只写入基础信息，详细直方图走 WebSocket B64 通道
        if (gpm.histogramBins > 0 && !gpm.histogram.empty()) {
            gpmJson["histogramBins"] = gpm.histogramBins;
            gpmJson["histogramTotal"] = QString::number(static_cast<qulonglong>(gpm.histogramTotal));
        }

    QJsonDocument gpmDoc(gpmJson);
    // 原子写入 gpm.json：避免读取到半写 JSON（高帧率覆盖写时尤其明显）
    {
        QSaveFile gpmFile(gpmFilePath);
        gpmFile.setDirectWriteFallback(true);
        if (!gpmFile.open(QIODevice::WriteOnly)) {
            Logger::Log("Failed to open gpm.json for writing: " + gpmFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        } else {
            const QByteArray json = gpmDoc.toJson();
            if (gpmFile.write(json) != json.size()) {
                Logger::Log("Failed to write gpm.json bytes: " + gpmFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
                gpmFile.cancelWriting();
            } else if (!gpmFile.commit()) {
                Logger::Log("Failed to commit gpm.json (atomic replace): " + gpmFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
            } else {
                Logger::Log("GPM saved to: " + gpmFilePath.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
            }
        }
    }
    Logger::Log("Tile pyramid | step write gpm.json done", LogLevel::INFO, DeviceType::CAMERA);

    Logger::Log("Tile pyramid generation completed for session: " + sessionId.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return gpm;
}

void MainWindow::emitCaptureTrace(const QString& stage, qint64 startedAtMs, const QString& detail)
{
    if (currentCaptureTraceId.trimmed().isEmpty() || wsThread == nullptr) {
        return;
    }
    const qint64 baseMs = (startedAtMs >= 0) ? startedAtMs : currentCaptureTraceStartedAtMs;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMs = (baseMs > 0) ? std::max<qint64>(0, nowMs - baseMs) : 0;
    QString safeDetail = detail;
    safeDetail.replace('\n', ' ');
    safeDetail.replace('\r', ' ');
    emit wsThread->sendMessageToClient(
        QString("CaptureTrace:%1:%2:%3:%4")
            .arg(currentCaptureTraceId)
            .arg(stage)
            .arg(elapsedMs)
            .arg(safeDetail)
    );
    Logger::Log(
        QString("CaptureTrace | traceId=%1 | stage=%2 | backendElapsedMs=%3 | detail=%4")
            .arg(currentCaptureTraceId)
            .arg(stage)
            .arg(elapsedMs)
            .arg(safeDetail)
            .toStdString(),
        LogLevel::INFO,
        DeviceType::CAMERA
    );
}

void MainWindow::sendGPMToClient(const TileGPM& gpm)
{
    // 构建GPM消息发送给前端
    // 格式(兼容扩展):
    // - v1: TileGPM:{sessionId}:{imageWidth}:{imageHeight}:{tileSize}:{maxZoomLevel}:{blackLevel}:{whiteLevel}:{cfa}:{gainR}:{gainB}
    // - v2(追加): ...:{previewWidth}:{previewHeight}:{previewBinningFactor}
    // - v3(追加): ...:{frameId}
    // - v4(追加): ...:{buildMode}
    // - v5(追加): ...:{levelMode}
    // 说明：追加字段放在末尾，旧前端按前 11 段解析不会受影响。
    QString gpmMessage = QString("TileGPM:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11:%12:%13:%14:%15:%16")
        .arg(gpm.sessionId)
        .arg(gpm.imageWidth)
        .arg(gpm.imageHeight)
        .arg(gpm.tileSize)
        .arg(gpm.maxZoomLevel)
        .arg(gpm.blackLevel)
        .arg(gpm.whiteLevel)
        .arg(gpm.cfa)
        .arg(gpm.gainR)
        .arg(gpm.gainB)
        .arg(gpm.previewWidth)
        .arg(gpm.previewHeight)
        .arg(gpm.previewBinningFactor)
        .arg(QString::number(static_cast<qulonglong>(gpm.frameId)))
        .arg(gpm.buildMode)
        .arg(gpm.levelMode);

    emit wsThread->sendMessageToClient(gpmMessage);
    Logger::Log("GPM sent to client: " + gpmMessage.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | sendGPMToClient | sessionId = " +
                    gpm.sessionId.toStdString(),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | sendGPMToClient | frameId = " +
                    std::to_string(static_cast<unsigned long long>(gpm.frameId)),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | sendGPMToClient | blackWhite = " +
                    std::to_string(gpm.blackLevel) + "," + std::to_string(gpm.whiteLevel),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | sendGPMToClient | gainRB = " +
                    std::to_string(gpm.gainR) + "," + std::to_string(gpm.gainB),
                LogLevel::INFO, DeviceType::CAMERA);
}

void MainWindow::sendTileBatchReadyToClient(const QString& sessionId, quint64 frameId, const QStringList& tileKeys)
{
    if (sessionId.isEmpty() || tileKeys.isEmpty()) {
        return;
    }

    if (QThread::currentThread() != thread()) {
        QPointer<MainWindow> self(this);
        QMetaObject::invokeMethod(this, [self, sessionId, frameId, tileKeys]() {
            if (!self) return;
            self->sendTileBatchReadyToClient(sessionId, frameId, tileKeys);
        }, Qt::QueuedConnection);
        return;
    }

    QJsonObject payload;
    payload["sessionId"] = sessionId;
    payload["frameId"] = QString::number(static_cast<qulonglong>(frameId));
    QJsonArray tilesJson;
    for (const QString& key : tileKeys) {
        tilesJson.append(key);
    }
    payload["tiles"] = tilesJson;

    const QString message = QStringLiteral("TileBatchReady:")
        + QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    emit wsThread->sendMessageToClient(message);
    QStringList sampleKeys;
    const int sampleCount = std::min(8, tileKeys.size());
    for (int i = 0; i < sampleCount; ++i) {
        sampleKeys.push_back(tileKeys.at(i));
    }
    Logger::Log("TileBatchReady sent to client: session=" + sessionId.toStdString() +
                    ", frameId=" + std::to_string(static_cast<unsigned long long>(frameId)) +
                    ", count=" + std::to_string(tileKeys.size()) +
                    ", sample=[" + sampleKeys.join(", ").toStdString() + "]",
                LogLevel::DEBUG, DeviceType::CAMERA);
    const bool containsZ0 = tileKeys.contains(QStringLiteral("0/0/0"));
    emitCaptureTrace(QStringLiteral("backend_tilebatchready_sent"), currentCaptureTraceStartedAtMs,
                     QString("sessionId=%1,frameId=%2,count=%3,containsZ0=%4")
                         .arg(sessionId)
                         .arg(QString::number(static_cast<qulonglong>(frameId)))
                         .arg(tileKeys.size())
                         .arg(containsZ0 ? QStringLiteral("true") : QStringLiteral("false")));
}

void MainWindow::sendCurrentTileBatchReadySnapshotToClient(const QString& sessionId, quint64 frameId, const QStringList& requestedTileKeys)
{
    if (sessionId.isEmpty() || frameId == 0) {
        return;
    }

    TileFrameState st;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
    }
    if (st.sessionId != sessionId || st.epoch != frameId) {
        Logger::Log("queryTileBatchReady ignored: current session/frame mismatch, currentSession=" +
                        st.sessionId.toStdString() + ", currentFrame=" +
                        std::to_string(static_cast<unsigned long long>(st.epoch)),
                    LogLevel::DEBUG, DeviceType::CAMERA);
        return;
    }

    std::vector<uint64_t> readyKeys;
    {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        if (tileGenDoneEpoch != frameId || tileGenDoneKeys.empty()) {
            return;
        }
        readyKeys.assign(tileGenDoneKeys.begin(), tileGenDoneKeys.end());
    }

    std::set<uint64_t> requestedPackedKeys;
    if (!requestedTileKeys.isEmpty()) {
        for (const QString& key : requestedTileKeys) {
            const QStringList keyParts = key.split('/');
            if (keyParts.size() != 3) continue;
            bool okZ = false;
            bool okX = false;
            bool okY = false;
            const int z = keyParts[0].toInt(&okZ);
            const int x = keyParts[1].toInt(&okX);
            const int y = keyParts[2].toInt(&okY);
            if (!okZ || !okX || !okY || z < 0 || x < 0 || y < 0) continue;
            const uint64_t packedKey =
                (static_cast<uint64_t>(z) << 40) |
                (static_cast<uint64_t>(x) << 20) |
                static_cast<uint64_t>(y);
            requestedPackedKeys.insert(packedKey);
        }
        readyKeys.erase(
            std::remove_if(readyKeys.begin(), readyKeys.end(),
                           [&requestedPackedKeys](uint64_t packedKey) {
                               return requestedPackedKeys.find(packedKey) == requestedPackedKeys.end();
                           }),
            readyKeys.end());
        if (readyKeys.empty()) {
            return;
        }
    }

    std::sort(readyKeys.begin(), readyKeys.end());
    QStringList tileKeys;
    tileKeys.reserve(static_cast<int>(readyKeys.size()));
    for (uint64_t packedKey : readyKeys) {
        const int z = static_cast<int>((packedKey >> 40) & ((1ULL << 24) - 1));
        const int x = static_cast<int>((packedKey >> 20) & ((1ULL << 20) - 1));
        const int y = static_cast<int>(packedKey & ((1ULL << 20) - 1));
        tileKeys.push_back(QString::number(z) + "/" + QString::number(x) + "/" + QString::number(y));
    }

    Logger::Log("queryTileBatchReady snapshot: session=" + sessionId.toStdString() +
                    ", frameId=" + std::to_string(static_cast<unsigned long long>(frameId)) +
                    ", count=" + std::to_string(tileKeys.size()) +
                    ", requestedCount=" + std::to_string(requestedTileKeys.size()),
                LogLevel::DEBUG, DeviceType::CAMERA);
    sendTileBatchReadyToClient(sessionId, frameId, tileKeys);
}

void MainWindow::sendTileGenerationCompleteToClient(const QString& sessionId, quint64 frameId)
{
    if (sessionId.isEmpty() || frameId == 0) {
        return;
    }

    if (QThread::currentThread() != thread()) {
        QPointer<MainWindow> self(this);
        QMetaObject::invokeMethod(this, [self, sessionId, frameId]() {
            if (!self) return;
            self->sendTileGenerationCompleteToClient(sessionId, frameId);
        }, Qt::QueuedConnection);
        return;
    }

    TileFrameState st;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
    }
    if (st.sessionId != sessionId || st.epoch != frameId || tilePyramidEpoch.load() != frameId) {
        return;
    }

    size_t readyCount = 0;
    {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        if (tileGenDoneEpoch != frameId) {
            return;
        }
        if (tileGenCompleteEpoch == frameId) {
            return;
        }
        readyCount = tileGenDoneKeys.size();
        tileGenCompleteEpoch = frameId;
    }

    QJsonObject payload;
    payload["sessionId"] = sessionId;
    payload["frameId"] = QString::number(static_cast<qulonglong>(frameId));
    payload["readyCount"] = static_cast<qint64>(readyCount);
    payload["complete"] = true;

    const QString message = QStringLiteral("TileGenerationComplete:")
        + QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    emit wsThread->sendMessageToClient(message);

    Logger::Log("TileGenerationComplete sent to client: session=" + sessionId.toStdString() +
                    ", frameId=" + std::to_string(static_cast<unsigned long long>(frameId)) +
                    ", readyCount=" + std::to_string(static_cast<unsigned long long>(readyCount)),
                LogLevel::DEBUG, DeviceType::CAMERA);
}

void MainWindow::sendCurrentTileGenerationCompleteSnapshotToClient(const QString& sessionId, quint64 frameId)
{
    if (sessionId.isEmpty() || frameId == 0) {
        return;
    }

    TileFrameState st;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
    }
    if (st.sessionId != sessionId || st.epoch != frameId) {
        return;
    }

    size_t readyCount = 0;
    {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        if (tileGenCompleteEpoch != frameId) {
            return;
        }
        readyCount = tileGenDoneKeys.size();
    }

    Logger::Log("queryTileGenerationComplete snapshot: session=" + sessionId.toStdString() +
                    ", frameId=" + std::to_string(static_cast<unsigned long long>(frameId)),
                LogLevel::DEBUG, DeviceType::CAMERA);

    QJsonObject payload;
    payload["sessionId"] = sessionId;
    payload["frameId"] = QString::number(static_cast<qulonglong>(frameId));
    payload["readyCount"] = static_cast<qint64>(readyCount);
    payload["complete"] = true;

    const QString message = QStringLiteral("TileGenerationComplete:")
        + QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    emit wsThread->sendMessageToClient(message);
}

void MainWindow::sendHistogramToClient(const TileGPM& gpm)
{
    if (gpm.sessionId.isEmpty() || gpm.histogramBins <= 0 || gpm.histogram.empty()) {
        return;
    }

    // ========================= 新方案：直方图保存到 tmpfs，供 HTTPS 下载 =========================
    // 直方图与瓦片同目录（/dev/shm/capture-tiles/），按 session 独立命名。

    QString histogramFileName = QString("histogram_%1.bin").arg(gpm.sessionId);
    QString histogramFilePath = QString::fromStdString(tilePyramidPath) + histogramFileName;
    
    // 2. 将直方图保存为二进制文件
    QFile binFile(histogramFilePath);
    if (!binFile.open(QIODevice::WriteOnly)) {
        Logger::Log("Failed to create histogram file: " + histogramFilePath.toStdString(),
                    LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }
    
    // 写入文件头和数据（便于前端解析）
    // 文件格式：[bins(4字节)][total(8字节)][histogram数据(bins*4字节)]
    QDataStream out(&binFile);
    out.setByteOrder(QDataStream::LittleEndian);
    
    // 写入bins数量（4字节，uint32）
    out << static_cast<quint32>(gpm.histogramBins);
    
    // 写入总像素数（8字节，uint64）
    out << static_cast<quint64>(gpm.histogramTotal);
    
    // 写入直方图数据（bins * 4字节）
    for (uint32_t count : gpm.histogram) {
        out << static_cast<quint32>(count);
    }
    
    qint64 fileSize = binFile.size();
    binFile.close();
    
    Logger::Log("Histogram saved to file: " + histogramFilePath.toStdString() + 
                ", size: " + std::to_string(fileSize) + " bytes" +
                ", bins: " + std::to_string(gpm.histogramBins),
                LogLevel::INFO, DeviceType::CAMERA);
    
    // 3. 构建下载 URL（nginx 需 alias /img/capture-tiles/ -> /dev/shm/capture-tiles/）
    QString histogramUrl = QString("/img/capture-tiles/%1").arg(histogramFileName);
    
    // 4. 通过WebSocket发送元数据和URL（而不是完整的Base64数据）
    // 格式: TileHistogramFile:{sessionId}:{bins}:{total}:{url}
    QString msg = QString("TileHistogramFile:%1:%2:%3:%4")
        .arg(gpm.sessionId)
        .arg(gpm.histogramBins)
        .arg(QString::number(static_cast<qulonglong>(gpm.histogramTotal)))
        .arg(histogramUrl);
    
    emit wsThread->sendMessageToClient(msg);
    
    Logger::Log("Histogram URL sent to client: session=" + gpm.sessionId.toStdString() +
                ", url=" + histogramUrl.toStdString(),
                LogLevel::INFO, DeviceType::CAMERA);
    
    // 5. 每帧独立 histogram 文件；保留最近几帧，兼顾前端异步下载与 tmpfs 占用。
    cleanupOldHistogramFiles(5);
}

void MainWindow::cleanupOldHistogramFiles(int keepCount)
{
    // 直方图与瓦片同在 tmpfs（tilePyramidPath）
    QDir directory(QString::fromStdString(tilePyramidPath));
    QStringList filters;
    filters << "histogram_*.bin";
    
    // 按修改时间排序，最新的在前
    QFileInfoList fileList = directory.entryInfoList(filters, QDir::Files, QDir::Time);
    
    if (fileList.size() <= keepCount) {
        return; // 文件数量未超过保留数量，无需清理
    }
    
    // 删除超出保留数量的旧文件
    for (int i = keepCount; i < fileList.size(); ++i) {
        QString filePath = fileList[i].absoluteFilePath();
        if (QFile::remove(filePath)) {
            Logger::Log("Removed old histogram file: " + filePath.toStdString(), 
                       LogLevel::INFO, DeviceType::CAMERA);
        } else {
            Logger::Log("Failed to remove old histogram file: " + filePath.toStdString(), 
                       LogLevel::WARNING, DeviceType::CAMERA);
        }
    }
    
    int removedCount = fileList.size() - keepCount;
    if (removedCount > 0) {
        Logger::Log("Cleaned up " + std::to_string(removedCount) + " old histogram file(s), kept " + 
                    std::to_string(keepCount) + " most recent", 
                    LogLevel::INFO, DeviceType::CAMERA);
    }
}

void MainWindow::cleanupOldTileSessionDirs(const QString& keepSessionId)
{
    if (keepSessionId.isEmpty()) return;
    QDir baseDir(QString::fromStdString(tilePyramidPath));
    if (!baseDir.exists()) return;
    const QStringList entries = baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    int removed = 0;
    for (const QString& name : entries) {
        if (name == keepSessionId) continue;
        // 只删除会话目录：live 或 live_<数字>
        if (name != QStringLiteral("live") && !name.startsWith(QStringLiteral("live_"))) continue;
        QString absPath = baseDir.absoluteFilePath(name);
        QDir subDir(absPath);
        if (subDir.removeRecursively()) {
            removed++;
            Logger::Log("Removed old tile session dir: " + absPath.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
        } else {
            Logger::Log("Failed to remove old tile session dir: " + absPath.toStdString(), LogLevel::WARNING, DeviceType::CAMERA);
        }
    }
    if (removed > 0) {
        Logger::Log("Cleaned up " + std::to_string(removed) + " old tile session dir(s), kept " + keepSessionId.toStdString(),
                    LogLevel::INFO, DeviceType::CAMERA);
    }
}

// ========================= 瓦片金字塔生成相关函数结束 =========================

cv::Mat MainWindow::colorImage(cv::Mat img16)
{
    Logger::Log("Starting color image processing...", LogLevel::INFO, DeviceType::MAIN);
    QString effectiveCameraCFA = MainCameraCFA;
    // color camera, need to do debayer and color balance
    cv::Mat AWBImg16;
    cv::Mat AWBImg16color;
    cv::Mat AWBImg16mono;
    cv::Mat AWBImg8color;

    uint16_t B = 0;
    uint16_t W = 65535;

    AWBImg16.create(img16.rows, img16.cols, CV_16UC1);
    AWBImg16color.create(img16.rows, img16.cols, CV_16UC3);
    AWBImg16mono.create(img16.rows, img16.cols, CV_16UC1);
    AWBImg8color.create(img16.rows, img16.cols, CV_8UC3);

    const uint16_t offset = static_cast<uint16_t>(std::clamp(std::lround(ImageOffset), 0l, 65535l));
    Logger::Log("Matrices for image processing created.", LogLevel::INFO, DeviceType::MAIN);
    Tools::ImageSoftAWB(img16, AWBImg16, effectiveCameraCFA, ImageGainR, ImageGainB, offset); // image software Auto White Balance is done in RAW image.
    Logger::Log("Auto White Balance applied.", LogLevel::INFO, DeviceType::MAIN);
    const int demosaicCode = getOpenCvBayerToBgrCode(effectiveCameraCFA);
    if (demosaicCode < 0) {
        Logger::Log("colorImage | invalid CFA for Bayer->BGR conversion: " + effectiveCameraCFA.toStdString(),
                    LogLevel::WARNING, DeviceType::MAIN);
        return cv::Mat();
    }
    cv::cvtColor(AWBImg16, AWBImg16color, demosaicCode);
    Logger::Log("Image converted from Bayer to BGR.", LogLevel::INFO, DeviceType::MAIN);

    cv::cvtColor(AWBImg16color, AWBImg16mono, cv::COLOR_BGR2GRAY);
    Logger::Log("Image converted to grayscale.", LogLevel::INFO, DeviceType::MAIN);

    if (AutoStretch == true)
    {
        Tools::GetAutoStretch(AWBImg16mono, 0, B, W);
        Logger::Log("Auto stretch applied.", LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        B = 0;
        W = 65535;
        Logger::Log("Auto stretch not applied, using default values.", LogLevel::INFO, DeviceType::MAIN);
    }
    Logger::Log("AutoStretch values: B=" + std::to_string(B) + ", W=" + std::to_string(W), LogLevel::INFO, DeviceType::MAIN);
    Tools::Bit16To8_Stretch(AWBImg16color, AWBImg8color, B, W);
    Logger::Log("Image stretched from 16-bit to 8-bit.", LogLevel::INFO, DeviceType::MAIN);

    AWBImg16.release();
    AWBImg16color.release();
    AWBImg16mono.release();
    AWBImg8color.release();
    Logger::Log("Temporary matrices released.", LogLevel::INFO, DeviceType::MAIN);

    return AWBImg16color;
}

void MainWindow::CaptureImageSaveAsync()
{
    QPointer<MainWindow> self(this);
    QtConcurrent::run([self]() {
        if (!self) return;
        self->CaptureImageSave();
    });
}

void MainWindow::saveFitsAsJPG(QString filename, bool ProcessBin)
{
    // 创建MainCameraCFA的局部副本，防止多线程竞态条件导致的值污染
    QString localCameraCFA = MainCameraCFA;
    QString roiCameraCFA = localCameraCFA;
    
    // 验证CFA值的合法性
    QStringList validCFAValues = {"RGGB", "BGGR", "GRBG", "GBRG", "RG", "BG", "GR", "GB", "", "null"};
    if (!validCFAValues.contains(localCameraCFA))
    {
        Logger::Log("saveFitsAsJPG | Invalid MainCameraCFA value detected: '" + localCameraCFA.toStdString() + 
                   "'. Using empty (Mono mode) for this operation.", LogLevel::ERROR, DeviceType::CAMERA);
        localCameraCFA = "";  // 使用单色相机模式
    }
    
    cv::Mat image;
    // 读取FITS文件
    Tools::readFits(filename.toLocal8Bit().constData(), image);
    if (image.empty())
    {
        Logger::Log("saveFitsAsJPG | readFits succeeded but image is empty: " + filename.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    Logger::Log("saveFitsAsJPG | input FITS filename=" + filename.toStdString() +
                    ", raw image=" + std::to_string(image.cols) + "x" + std::to_string(image.rows),
                LogLevel::INFO, DeviceType::FOCUSER);

    QList<FITSImage::Star> stars;
    if (roiUseSelfCalcParams)
    {
        stars = Tools::FindStarsByFocusedCppFromFile(filename, true, true);
        Logger::Log("saveFitsAsJPG | ROI star detection uses ROI self-calculated params, source=" +
                        filename.toStdString(),
                    LogLevel::INFO, DeviceType::FOCUSER);
    }
    else
    {
        stars = Tools::FindStarsByFocusedCpp(true, true);
        Logger::Log("saveFitsAsJPG | ROI star detection reuses full-frame params/source, current ROI frame=" +
                        filename.toStdString(),
                    LogLevel::INFO, DeviceType::FOCUSER);
    }
    currentSelectStarPosition = selectStar(stars);

    emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FocuserControl_getPosition()) + ":" + QString::number(roiAndFocuserInfo["SelectStarHFR"]));
    emit wsThread->sendMessageToClient("setSelectStarPosition:" + QString::number(roiAndFocuserInfo["SelectStarX"]) + ":" + QString::number(roiAndFocuserInfo["SelectStarY"]) + ":" + QString::number(roiAndFocuserInfo["SelectStarHFR"]) + ":" + QString::number(roiAndFocuserInfo["SelectStarSNR"]) + ":" + QString::number(roiAndFocuserInfo["SelectStarLocalMax"]) + ":" + QString::number(roiAndFocuserInfo["SelectStarBgStd"]));
    emit wsThread->sendMessageToClient("addFwhmNow:" + QString::number(roiAndFocuserInfo["SelectStarHFR"]));
    emit wsThread->sendMessageToClient("addSnrNow:" + QString::number(roiAndFocuserInfo["SelectStarSNR"]));
    Logger::Log("saveFitsAsJPG | 星点位置更新为 x:" + std::to_string(roiAndFocuserInfo["SelectStarX"]) + ",y:" + std::to_string(roiAndFocuserInfo["SelectStarY"]) + ",HFR:" + std::to_string(roiAndFocuserInfo["SelectStarHFR"]) + ",SNR:" + std::to_string(roiAndFocuserInfo["SelectStarSNR"]) + ",localMax:" + std::to_string(roiAndFocuserInfo["SelectStarLocalMax"]) + ",bgStd:" + std::to_string(roiAndFocuserInfo["SelectStarBgStd"]), LogLevel::INFO, DeviceType::FOCUSER);

    cv::Mat originalImage16;
    if (image.type() == CV_8UC1 || image.type() == CV_8UC3 || image.type() == CV_16UC1)
    {
        originalImage16 = Tools::convert8UTo16U_BayerSafe(image, false);
        Logger::Log("saveFitsAsJPG | image size:" + std::to_string(image.cols) + "x" + std::to_string(image.rows), LogLevel::INFO, DeviceType::FOCUSER);
        image.release();
    }
    else
    {
        Logger::Log("The current image data type is not supported for processing.", LogLevel::WARNING, DeviceType::CAMERA);
        image.release();
        originalImage16.release();
        return;
    }
    if (originalImage16.empty())
    {
        Logger::Log("saveFitsAsJPG | convert8UTo16U_BayerSafe returned empty image; skip medianBlur", LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }
    Logger::Log("saveFitsAsJPG | image16 size:" + std::to_string(originalImage16.cols) + "x" + std::to_string(originalImage16.rows), LogLevel::INFO, DeviceType::FOCUSER);

    // 最小验证：ROI 导出链路暂时不对 Bayer RAW 做 medianBlur。
    // 若颜色恢复正常，可基本确认“RAW 上的滤波破坏 CFA 采样结构”就是偏色根因。
    Logger::Log("saveFitsAsJPG | median blur skipped for ROI Bayer validation", LogLevel::WARNING, DeviceType::CAMERA);

    // 下发给前端的 ROI .bin：不做软件合并，与相机 ROI 读出尺寸一致（与前端红框/瓦片传感器坐标对齐）。
    (void)ProcessBin;
    cv::Mat image16 = originalImage16.clone();
    // 部分驱动/SDK 在 ROI 模式下仍写出全幅 FITS；前端按 RedBoxSideLength² 解码并与 SaveJpgSuccess 的传感器原点对齐。
    // 若缓冲区大于本次曝光 ROI 尺寸，则按 FocusingLooping 记录的快照从全幅中裁出与 .bin 语义一致的子图。
    if (lastFocusExposureSnapshotValid && lastFocusExposureRoiW > 0 && lastFocusExposureRoiH > 0 && !image16.empty()
        && image16.type() == CV_16UC1)
    {
        const int cw = lastFocusExposureRoiW;
        const int ch = lastFocusExposureRoiH;
        const int sx = lastFocusExposureEffMinX + lastFocusExposureScaledX;
        const int sy = lastFocusExposureEffMinY + lastFocusExposureScaledY;
        roiCameraCFA = resolveFrameCfa(sx, sy);
        if (image16.cols == cw && image16.rows == ch)
        {
            // 已是 ROI 子帧（像素 (0,0) 即 ROI 左上角）
        }
        else if (sx >= 0 && sy >= 0 && image16.cols >= sx + cw && image16.rows >= sy + ch)
        {
            const cv::Rect patch(sx, sy, cw, ch);
            image16 = image16(patch).clone();
            Logger::Log("saveFitsAsJPG | crop full buffer to ROI " + std::to_string(cw) + "x" + std::to_string(ch)
                            + " at (" + std::to_string(sx) + "," + std::to_string(sy) + ")",
                        LogLevel::DEBUG, DeviceType::FOCUSER);
        }
        else if (image16.cols > cw || image16.rows > ch)
        {
            const cv::Rect want(sx, sy, cw, ch);
            const cv::Rect bounds(0, 0, image16.cols, image16.rows);
            const cv::Rect inter = want & bounds;
            if (inter.width > 0 && inter.height > 0)
            {
                image16 = image16(inter).clone();
                Logger::Log("saveFitsAsJPG | cropped full-frame buffer (clamped) to " + std::to_string(image16.cols) + "x" + std::to_string(image16.rows),
                            LogLevel::WARNING, DeviceType::FOCUSER);
            }
        }
    }
    roiCameraCFA = normalizeCfaPattern(roiCameraCFA);
    if (roiCameraCFA.isEmpty() && !localCameraCFA.isEmpty()) {
        roiCameraCFA = normalizeCfaPattern(localCameraCFA);
    }
    Logger::Log("saveFitsAsJPG | ROI CFA base=" + localCameraCFA.toStdString() +
                    " resolved=" + roiCameraCFA.toStdString() +
                    " cfaOffset=(" + std::to_string(MainCameraCFAOffsetX) + "," +
                    std::to_string(MainCameraCFAOffsetY) + ")",
                LogLevel::INFO, DeviceType::FOCUSER);
    if (lastFocusExposureSnapshotValid)
    {
        const int snapSensorX = lastFocusExposureEffMinX + lastFocusExposureScaledX;
        const int snapSensorY = lastFocusExposureEffMinY + lastFocusExposureScaledY;
        Logger::Log("saveFitsAsJPG | ROI Bayer debug | " +
                        formatBayerPhaseDebug(localCameraCFA, MainCameraCFAOffsetX, MainCameraCFAOffsetY,
                                             snapSensorX, snapSensorY, roiCameraCFA) +
                        ", snapshotScaled=(" + std::to_string(lastFocusExposureScaledX) + "," + std::to_string(lastFocusExposureScaledY) + ")" +
                        ", snapshotEffMin=(" + std::to_string(lastFocusExposureEffMinX) + "," + std::to_string(lastFocusExposureEffMinY) + ")" +
                        ", snapshotRoiSize=" + std::to_string(lastFocusExposureRoiW) + "x" + std::to_string(lastFocusExposureRoiH) +
                        ", outputImage=" + std::to_string(image16.cols) + "x" + std::to_string(image16.rows) +
                        ", " + sampleBayer2x2Debug(image16),
                    LogLevel::INFO, DeviceType::FOCUSER);
    }
    Logger::Log("saveFitsAsJPG | output image16 " + std::to_string(image16.cols) + "x" + std::to_string(image16.rows), LogLevel::DEBUG, DeviceType::FOCUSER);
    originalImage16.release();

    // ROI 循环频率可能高于 1Hz：若文件名只精确到秒，会在同一秒内反复覆盖同名文件，
    // 造成前端拉取到旧内容/404（尤其在前端处理变慢、跳帧时）。这里改为毫秒级并追加序号，保证全局唯一。
    static std::atomic_uint64_t roiFileSeq{0};
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const uint64_t seq = roiFileSeq.fetch_add(1, std::memory_order_relaxed);
    std::string fileName = "focuserPicture_" + std::to_string(nowMs) + "_" + std::to_string(seq) + ".bin";
    std::string filePath = vueDirectoryPath + fileName;
    std::ofstream outFile(filePath, std::ios::binary);

    // 检查文件是否成功打开
    if (!outFile)
    {
        Logger::Log("Failed to open file: " + filePath, LogLevel::WARNING, DeviceType::FOCUSER);
        if (isFocusLoopShooting)
        {
            FocusingLooping();
        }
        return;
    }

    // 将图像数据写入到文件中
    outFile.write(reinterpret_cast<const char *>(image16.data), image16.total() * image16.elemSize());

    // 检查是否成功写入
    if (!outFile)
    {
        Logger::Log("Failed to write to file: " + filePath, LogLevel::ERROR, DeviceType::FOCUSER);
        if (isFocusLoopShooting)
        {
            FocusingLooping();
        }
        return;
    }

    outFile.close();

    bool saved = true;

    // 创建/更新本次 ROI 对应的符号链接（供前端通过 /img/ 访问）
    std::string Command = "ln -sf " + filePath + " " + vueImagePath + fileName;
    system(Command.c_str());
    Logger::Log("Symbolic link created for new image file.", LogLevel::DEBUG, DeviceType::FOCUSER);

    if (saved)
    {
        // SaveJpgSuccess 中的 ROI 必须与「本帧 .bin 对应的曝光」一致：用 FocusingLooping 里在真正下发曝光前记录的快照。
        // 若先应用 hasPendingRoiUpdate / 或用户已 sendRedBoxState 再发 SaveJpgSuccess，会把下一帧坐标与当前帧像素绑在一起，造成 ROI 叠加与底图错位。
        double emitRoiX = 0.0;
        double emitRoiY = 0.0;
        if (lastFocusExposureSnapshotValid)
        {
            const int snapScale = std::max(1, lastFocusExposureRoiCoordScale);
            emitRoiX = static_cast<double>(lastFocusExposureScaledX) / static_cast<double>(snapScale);
            emitRoiY = static_cast<double>(lastFocusExposureScaledY) / static_cast<double>(snapScale);
        }
        else
        {
            emitRoiX = roiAndFocuserInfo.count("ROI_x") ? roiAndFocuserInfo["ROI_x"] : 0.0;
            emitRoiY = roiAndFocuserInfo.count("ROI_y") ? roiAndFocuserInfo["ROI_y"] : 0.0;
        }

        // 与前端 parseFloat 一致，保留小数（非瓦片 bin 缩放下 emit 可能为小数）
        Logger::Log("saveFitsAsJPG | ROI frame mapping file=" + fileName +
                        ", emitRoi=(" + std::to_string(emitRoiX) + "," + std::to_string(emitRoiY) + ")" +
                        ", image16=" + std::to_string(image16.cols) + "x" + std::to_string(image16.rows) +
                        ", roiCFA=" + roiCameraCFA.toStdString(),
                    LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("SaveJpgSuccess:" + QString::fromStdString(fileName) + ":" +
                                           QString::number(emitRoiX, 'g', 9) + ":" +
                                           QString::number(emitRoiY, 'g', 9) + ":" +
                                           (roiCameraCFA.isEmpty() ? QStringLiteral("null") : roiCameraCFA));

        // 挂起的 ROI 居中更新：在本帧图像已发出后再改 roiAndFocuserInfo，并单独通知前端（与 SaveJpgSuccess 解耦）
        if (hasPendingRoiUpdate)
        {
            hasPendingRoiUpdate = false;
            int boxSideToSend = BoxSideLength;
            if (roiAndFocuserInfo.count("BoxSideLength"))
                boxSideToSend = static_cast<int>(std::lround(roiAndFocuserInfo["BoxSideLength"]));
            const int maxX = std::max(0, glMainCCDSizeX - boxSideToSend);
            const int maxY = std::max(0, glMainCCDSizeY - boxSideToSend);
            int applyX = std::min(std::max(0, pendingRoiX), maxX);
            int applyY = std::min(std::max(0, pendingRoiY), maxY);
            if (applyX % 2 != 0) applyX += (applyX < maxX ? 1 : -1);
            if (applyY % 2 != 0) applyY += (applyY < maxY ? 1 : -1);
            applyX = std::min(std::max(0, applyX), maxX);
            applyY = std::min(std::max(0, applyY), maxY);
            // pendingRoiX/Y 已为传感器像素（见 selectStar 全图坐标）；瓦片模式下勿再除以 previewBinning。
            const bool tileModeActive = (isStagingImage && !SavedImage.empty());
            const int coordScale = tileModeActive ? 1 : std::max(1, glMainCameraBinning);
            const QPointF snapped = snapRoiOriginToBayerSafePhase(static_cast<double>(applyX) / coordScale,
                                                                  static_cast<double>(applyY) / coordScale,
                                                                  boxSideToSend, boxSideToSend);
            roiAndFocuserInfo["ROI_x"] = snapped.x();
            roiAndFocuserInfo["ROI_y"] = snapped.y();
            // 勿在此处 emit SetRedBoxState：本帧 SaveJpgSuccess 已带「当前曝光」ROI；若再发「下一帧居中」坐标，前端会在叠加层仍为当前帧像素时把红框/选星圆改到新 ROI，造成错位。下一帧 SaveJpgSuccess 会携带新快照坐标并同步 UI。sendRoiInfo() 仍会发 SetRedBoxState 供重连等场景。
        }

        Logger::Log("SaveJpgSuccess:" + fileName + " to " + filePath + ",image size:" + std::to_string(image16.cols) + "x" + std::to_string(image16.rows), LogLevel::DEBUG, DeviceType::FOCUSER);

        // 清理旧 ROI 文件/链接：保留最近 N 个，避免前端处理变慢/跳帧时出现 404 或拿不到对应帧
        constexpr size_t kKeepRecentRoiFrames = 100;
        auto cleanupRoiArtifacts = [](const std::string& dirStr, bool wantSymlink) {
            try {
                const fs::path dirPath(dirStr);
                if (!fs::exists(dirPath))
                    return;

                auto hasPrefix = [](const std::string& s, const std::string& p) -> bool {
                    return s.rfind(p, 0) == 0;
                };
                auto hasSuffix = [](const std::string& s, const std::string& suf) -> bool {
                    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
                };

                struct EntryInfo {
                    fs::path path;
                    bool timeOk = false;
                    fs::file_time_type t;
                };

                std::vector<EntryInfo> items;
                items.reserve(256);

                for (const auto& entry : fs::directory_iterator(dirPath)) {
                    const std::string name = entry.path().filename().string();
                    if (!hasPrefix(name, "focuserPicture_") || !hasSuffix(name, ".bin"))
                        continue;

                    const bool isLink = fs::is_symlink(entry.symlink_status());
                    const bool isFile = fs::is_regular_file(entry.status());
                    if (wantSymlink) {
                        if (!isLink)
                            continue;
                    } else {
                        if (!isFile)
                            continue;
                    }

                    EntryInfo info;
                    info.path = entry.path();
                    try {
                        info.t = fs::last_write_time(entry.path());
                        info.timeOk = true;
                    } catch (...) {
                        info.timeOk = false; // 可能是断链等
                    }
                    items.push_back(std::move(info));
                }

                std::sort(items.begin(), items.end(), [](const EntryInfo& a, const EntryInfo& b) {
                    if (a.timeOk != b.timeOk)
                        return a.timeOk; // timeOk=true 排在前面
                    if (!a.timeOk)
                        return a.path.string() < b.path.string();
                    return a.t > b.t; // 新的在前
                });

                // timeOk=false 的条目优先删除（通常是断链），其余超出保留数的删除
                size_t kept = 0;
                for (const auto& it : items) {
                    const bool shouldKeep = it.timeOk && kept < kKeepRecentRoiFrames;
                    if (shouldKeep) {
                        kept++;
                        continue;
                    }
                    std::error_code ec;
                    fs::remove(it.path, ec);
                }
            } catch (...) {
                // 清理失败不影响主流程
            }
        };

        cleanupRoiArtifacts(vueDirectoryPath, false);
        cleanupRoiArtifacts(vueImagePath, true);
        
    }
    else
    {
        Logger::Log("Failed to save image.", LogLevel::ERROR, DeviceType::GUIDER);
    }
    // 释放最终的图像内存
    if (isAutoFocus)
    {
        autoFocus->setCaptureComplete(filename);
    }
    image16.release();
    focusLoopShooting(isFocusLoopShooting); 

}

QPointF MainWindow::selectStar(QList<FITSImage::Star> stars){
    // 1) 边界与输入检查
    if (stars.size() <= 0) {
        Logger::Log("selectStar | no stars", LogLevel::INFO, DeviceType::FOCUSER);
        roiAndFocuserInfo["SelectStarHFR"] = 0.0;
        roiAndFocuserInfo["SelectStarSNR"] = 0.0;
        roiAndFocuserInfo["SelectStarLocalMax"] = 0.0;
        roiAndFocuserInfo["SelectStarBgStd"] = 0.0;
        return QPointF(CurrentPosition, 0);
    }

    // 2) 读取 ROI 与选择点（全图坐标）
    const double boxSide = roiAndFocuserInfo.count("BoxSideLength") ? roiAndFocuserInfo["BoxSideLength"] : BoxSideLength;
    const bool tileModeActive = (isStagingImage && !SavedImage.empty());
    const int roiCoordScale = tileModeActive ? 1 : std::max(1, glMainCameraBinning);
    const double roi_x    = roiAndFocuserInfo.count("ROI_x") ? roiAndFocuserInfo["ROI_x"] * roiCoordScale : 0;
    const double roi_y    = roiAndFocuserInfo.count("ROI_y") ? roiAndFocuserInfo["ROI_y"] * roiCoordScale : 0;
    const double selXFull = roiAndFocuserInfo.count("SelectStarX") ? roiAndFocuserInfo["SelectStarX"] : -1;
    const double selYFull = roiAndFocuserInfo.count("SelectStarY") ? roiAndFocuserInfo["SelectStarY"] : -1;
    Logger::Log("selectStar | inputs stars=" + std::to_string(stars.size()) +
                    ", boxSide=" + std::to_string(boxSide) +
                    ", roi=(" + std::to_string(roi_x) + "," + std::to_string(roi_y) + ")" +
                    ", prevSelect=(" + std::to_string(selXFull) + "," + std::to_string(selYFull) + ")" +
                    ", roiCoordScale=" + std::to_string(roiCoordScale),
                LogLevel::INFO, DeviceType::FOCUSER);

    // 3) 若已锁定目标星，则优先在本帧中追踪最近的那颗
    const int edgeMargin = 5;
    if (selectedStarLocked && lockedStarFull.x() >= 0 && lockedStarFull.y() >= 0)
    {
        // 防抖：优先在粘滞半径内寻找最近星；否则再全局最近
        int stickIdx = -1; double stickDist2 = std::numeric_limits<double>::max();
        int nearIdx  = -1; double nearDist2  = std::numeric_limits<double>::max();

        for (int i = 0; i < stars.size(); ++i) {
            const auto &s = stars[i];
            if (s.HFR <= 0) continue;

            // 排除 ROI 边缘星点（ROI 相对坐标检查）
            if (s.x <= edgeMargin || s.y <= edgeMargin || 
                s.x >= boxSide - edgeMargin || s.y >= boxSide - edgeMargin) {
                continue;
            }

            // ROI 相对坐标 -> 全图坐标
            const double sxFull = roi_x + s.x;
            const double syFull = roi_y + s.y;
            const double dx = sxFull - lockedStarFull.x();
            const double dy = syFull - lockedStarFull.y();
            const double d2 = dx*dx + dy*dy;

            if (d2 < nearDist2) { nearDist2 = d2; nearIdx = i; }
            if (d2 <= (starStickRadiusPx * starStickRadiusPx) && d2 < stickDist2) {
                stickDist2 = d2; stickIdx = i;
            }
        }

        const int bestIdx = (stickIdx != -1 ? stickIdx : nearIdx);
        if (bestIdx != -1)
        {
            const auto &best = stars[bestIdx];
            const double bestXFull = roi_x + best.x;
            const double bestYFull = roi_y + best.y;
            // 存储 ROI 相对坐标
            roiAndFocuserInfo["SelectStarX"] = best.x;
            roiAndFocuserInfo["SelectStarY"] = best.y;
            roiAndFocuserInfo["SelectStarHFR"] = best.HFR;
            roiAndFocuserInfo["SelectStarSNR"] = best.theta;
            roiAndFocuserInfo["SelectStarLocalMax"] = best.a;
            roiAndFocuserInfo["SelectStarBgStd"] = best.b;
            // 更新锁定星点的全图坐标
            lockedStarFull = QPointF(bestXFull, bestYFull);
            Logger::Log("selectStar | tracking locked star: ROI(" + std::to_string(best.x) + "," + std::to_string(best.y) + ") Full(" + std::to_string(bestXFull) + "," + std::to_string(bestYFull) + ") HFR=" + std::to_string(best.HFR) + " SNR=" + std::to_string(best.theta) + " localMax=" + std::to_string(best.a) + " bgStd=" + std::to_string(best.b), LogLevel::DEBUG, DeviceType::FOCUSER);
            // 判断是否需要居中（挂起到下一帧应用）
            const double centerX = roi_x + boxSide / 2.0;
            const double centerY = roi_y + boxSide / 2.0;
            const double halfWin = boxSide * trackWindowRatio; // 半窗
            const bool outOfWindow = std::abs(bestXFull - centerX) > halfWin || std::abs(bestYFull - centerY) > halfWin;
            if (enableAutoRoiCentering)
            {
                if (outOfWindow) {
                    outOfWindowFrames++;
                } else {
                    outOfWindowFrames = 0;
                }
                if (outOfWindowFrames >= requiredOutFramesForRecentre) {
                    double newRoiX = bestXFull - boxSide / 2.0;
                    double newRoiY = bestYFull - boxSide / 2.0;
                    const int maxX = std::max(0, glMainCCDSizeX - static_cast<int>(boxSide));
                    const int maxY = std::max(0, glMainCCDSizeY - static_cast<int>(boxSide));
                    newRoiX = std::min<double>(std::max<double>(0, newRoiX), maxX);
                    newRoiY = std::min<double>(std::max<double>(0, newRoiY), maxY);
                    int newRoiXi = static_cast<int>(std::lround(newRoiX));
                    int newRoiYi = static_cast<int>(std::lround(newRoiY));
                    if (newRoiXi % 2 != 0) newRoiXi += 1;
                    if (newRoiYi % 2 != 0) newRoiYi += 1;
                    newRoiXi = std::min(std::max(0, newRoiXi), maxX);
                    newRoiYi = std::min(std::max(0, newRoiYi), maxY);
                    // 不立即应用到本帧，挂起到下一帧再更新
                    hasPendingRoiUpdate = true;
                    pendingRoiX = newRoiXi;
                    pendingRoiY = newRoiYi;
                    outOfWindowFrames = 0;
                    Logger::Log("selectStar | tracking window exceeded for consecutive frames, pending ROI recenter", LogLevel::INFO, DeviceType::FOCUSER);
                }
            }
            return lockedStarFull;
        }
        // 若锁定丢失，则继续按下面的自动选择逻辑
        Logger::Log("selectStar | locked star lost, attempting re-selection", LogLevel::WARNING, DeviceType::FOCUSER);
        selectedStarLocked = false;
        lockedStarFull = QPointF(-1, -1);
    }

    // 3) 自动选择 ROI 内"最亮且最大的"星点
    // 简化：按亮度/面积代理：优先 HFR 大、或亮度（这里可用 HFR 或自带的 brightness 字段，如无则用 HFR）
    int bestIdx = -1; double bestScore = -1;
    for (int i = 0; i < stars.size(); ++i) {
        const auto &s = stars[i];
        if (s.HFR <= 0) continue;
        // 排除 ROI 边缘星点（使用 <= 和 >= 确保边缘像素被排除）
        if (s.x <= edgeMargin || s.y <= edgeMargin || 
            s.x >= boxSide - edgeMargin || s.y >= boxSide - edgeMargin) {
            continue;
        }
        // 评分：HFR 越大越好，可叠加亮度，如 s.peak 或 s.flux（若结构里没有则仅用 HFR）
        double score = s.HFR; // TODO: 若有亮度字段可加权
        if (score > bestScore) { bestScore = score; bestIdx = i; }
    }
    if (bestIdx == -1) {
        Logger::Log("selectStar | no valid ROI star for auto-select", LogLevel::WARNING, DeviceType::FOCUSER);
        return QPointF(CurrentPosition, 0);
    }

    const auto &autoBest = stars[bestIdx];
    const double bestXFullAuto = roi_x + autoBest.x;
    const double bestYFullAuto = roi_y + autoBest.y;
    // 存储 ROI 相对坐标
    roiAndFocuserInfo["SelectStarX"] = autoBest.x;
    roiAndFocuserInfo["SelectStarY"] = autoBest.y;
    roiAndFocuserInfo["SelectStarHFR"] = autoBest.HFR;
    roiAndFocuserInfo["SelectStarSNR"] = autoBest.theta;
    roiAndFocuserInfo["SelectStarLocalMax"] = autoBest.a;
    roiAndFocuserInfo["SelectStarBgStd"] = autoBest.b;
    // 锁定星点的全图坐标
    lockedStarFull = QPointF(bestXFullAuto, bestYFullAuto);
    selectedStarLocked = true; // 锁定
    Logger::Log("selectStar | auto-selected and locked new star ROI(x,y,HFR,SNR,localMax,bgStd)=(" + std::to_string(autoBest.x) + "," + std::to_string(autoBest.y) + "," + std::to_string(autoBest.HFR) + "," + std::to_string(autoBest.theta) + "," + std::to_string(autoBest.a) + "," + std::to_string(autoBest.b) + ") Full(" + std::to_string(bestXFullAuto) + "," + std::to_string(bestYFullAuto) + ")", LogLevel::INFO, DeviceType::FOCUSER);
    return lockedStarFull;

    // 旧分支与重复逻辑清理完毕
}
