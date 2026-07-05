#include "mainwindow_command_support.h"

namespace {
struct SyncCommandResult {
    int exitCode = -1;
    QString out;
    QString err;
    bool finished = false;
};

SyncCommandResult runCommandSync(const QString &program, const QStringList &args, int timeoutMs = 3000)
{
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(timeoutMs)) {
        return {-1, QString(), process.errorString(), false};
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return {124,
                QString::fromUtf8(process.readAllStandardOutput()).trimmed(),
                QString::fromUtf8(process.readAllStandardError()).trimmed() + "\n(timeout)",
                false};
    }
    return {process.exitCode(),
            QString::fromUtf8(process.readAllStandardOutput()).trimmed(),
            QString::fromUtf8(process.readAllStandardError()).trimmed(),
            true};
}

SyncCommandResult runSudoSync(const QString &program, const QStringList &args, int timeoutMs = 3000)
{
    QStringList sudoArgs;
    sudoArgs << "-n" << program;
    sudoArgs << args;
    return runCommandSync("sudo", sudoArgs, timeoutMs);
}
} // namespace

void MainWindow::getClientSettings()
{

    std::string fileName = "config/config.ini";

    std::unordered_map<std::string, std::string> config;

    Tools::readClientSettings(fileName, config);

    const auto itMain = config.find("MainCameraFocalLength");
    const auto itLegacy = config.find("FocalLength");
    if (itMain == config.end() && itLegacy != config.end())
    {
        const QString legacyValue = QString::fromStdString(itLegacy->second).trimmed();
        if (!legacyValue.isEmpty())
        {
            setClientSettings("MainCameraFocalLength", legacyValue);
            config["MainCameraFocalLength"] = legacyValue.toStdString();
            Logger::Log("getClientSettings | migrated FocalLength -> MainCameraFocalLength = " +
                            legacyValue.toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
        }
    }

    Logger::Log("getClientSettings | Current Config:", LogLevel::INFO, DeviceType::MAIN);
    for (const auto &pair : config)
    {
        Logger::Log("getClientSettings | " + pair.first + " = " + pair.second, LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConfigureRecovery:" + QString::fromStdString(pair.first) + ":" + QString::fromStdString(pair.second));

        if (pair.first == "Coordinates")
        {
            QStringList coordinates = QString::fromStdString(pair.second).split(",");
            if (coordinates.size() >= 2)
            {
                observatorylongitude = coordinates[1].toDouble();
                observatorylatitude = coordinates[0].toDouble();
            }
        }

    }
    Logger::Log("getClientSettings finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::setClientSettings(QString ConfigName, QString ConfigValue)
{

    Logger::Log("setClientSettings start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string fileName = "config/config.ini";

    std::unordered_map<std::string, std::string> config;

    config[ConfigName.toStdString()] = ConfigValue.toStdString();

    Logger::Log("setClientSettings | Save Client Setting:" + ConfigName.toStdString() + " = " + ConfigValue.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Tools::saveClientSettings(fileName, config);
    if (ConfigName == "FocalLength" || ConfigName == "MainCameraFocalLength")
    {
        glFocalLength = ConfigValue.toDouble();
        if (ConfigName == "FocalLength")
        {
            std::unordered_map<std::string, std::string> migrateConfig;
            migrateConfig["MainCameraFocalLength"] = ConfigValue.toStdString();
            Tools::saveClientSettings(fileName, migrateConfig);
        }
    }
    if (ConfigName == "GuiderFocalLength")
    {
        guiderFocalLengthMm = ConfigValue.toDouble();
    }
    if (ConfigName == "GuiderExposureMs")
    {
        bool ok = false;
        const int exposureMs = ConfigValue.trimmed().toInt(&ok);
        if (ok && exposureMs > 0)
        {
            guiderExpMs = exposureMs;
            auto p = guiderParamsCache;
            p.exposureMs = exposureMs;
            guiderParamsCache = p;
            postGuiderCore(guiderCore, [p](GuiderCore *core) { core->setParams(p); });
        }
    }
    Logger::Log("setClientSettings finish!", LogLevel::INFO, DeviceType::MAIN);
}





bool MainWindow::isFileExists(const QString &filePath)
{
    QFile file(filePath);
    if (file.exists())
    {
        Logger::Log("isFileExists | file exists: " + filePath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("isFileExists | file does not exist: " + filePath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    return file.exists();
}



int MainWindow::MoveFileToUSB()
{
    qDebug("MoveFileToUSB");
    return 0;
}











// 解析字符串

// 返回 U 盘剩余内存

// 获取文件系统挂载模式

// 将文件系统挂载模式更改为读写模式



// 获取U盘挂载点（统一函数，供其他函数复用）





void MainWindow::SendDebugToVueClient(const QString &msg)
{
    emit wsThread->sendMessageToClient("SendDebugMessage|" + msg);
}

void MainWindow::customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context);

    if (!instance)
    {
        return;
    }

    QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    QString typeStr;
    switch (type)
    {
    case QtDebugMsg:
        typeStr = "Debug";
        break;
    case QtInfoMsg:
        typeStr = "Info";
        break;
    case QtWarningMsg:
        typeStr = "Warning";
        break;
    case QtCriticalMsg:
        typeStr = "Critical";
        break;
    default:
        typeStr = "Unknown";
    }

    // 统一格式化日志消息
    QString logMessage = QString("[Server] %1 | %2 | %3").arg(currentTime).arg(typeStr).arg(msg);

    // // 将日志消息添加到缓存
    // instance->logCache.push_back(logMessage);

    // 统一输出到标准错误，无论日志类型
    fprintf(stderr, "%s\n", logMessage.toLocal8Bit().constData());

    // 发送到客户端的Vue应用，包括日志类型和消息
    instance->SendDebugToVueClient(typeStr + "|" + msg);
}




// 串口通信列表




bool MainWindow::areFilesInSameDirectory(const QString &path1, const QString &path2)
{
    QFileInfo fileInfo1(path1);
    QFileInfo fileInfo2(path2);

    // 获取两个文件的目录路径
    QString dir1 = fileInfo1.absolutePath();
    QString dir2 = fileInfo2.absolutePath();

    // 比较目录路径是否相同
    return (dir1 == dir2);
}


static int scoreByIdLinkForType(const QString &fileNameLower, const QString &driverType)
{
    // 简单关键字打分，匹配越多分越高
    int score = 0;
    if (driverType == "Focuser")
    {
        if (fileNameLower.contains("gigadevice")) score += 2;
        if (fileNameLower.contains("gd32")) score += 2;
        if (fileNameLower.contains("cdc_acm")) score += 1;
        if (fileNameLower.contains("acm")) score += 1;
    }
    else if (driverType == "Mount")
    {
        if (fileNameLower.contains("1a86")) score += 2;          // WCH/CH34x VID
        if (fileNameLower.contains("usb_serial")) score += 2;    // CH34x 常见 by-id
        if (fileNameLower.contains("ch34")) score += 2;
        if (fileNameLower.contains("wch")) score += 1;
        if (fileNameLower.contains("ttyusb")) score += 1;        // 兜底弱信号
    }
    return score;
}






void MainWindow::bin_image(double *input, long width, long height, double *output, long *out_w, long *out_h)
{
    *out_w = width / BIN_SIZE;
    *out_h = height / BIN_SIZE;

    for (long y = 0; y < *out_h; y++)
    {
        for (long x = 0; x < *out_w; x++)
        {
            double sum = 0.0;
            for (int dy = 0; dy < BIN_SIZE; dy++)
            {
                for (int dx = 0; dx < BIN_SIZE; dx++)
                {
                    long ix = x * BIN_SIZE + dx;
                    long iy = y * BIN_SIZE + dy;
                    sum += input[iy * width + ix];
                }
            }
            output[y * (*out_w) + x] = sum / (BIN_SIZE * BIN_SIZE);
        }
    }
}

void MainWindow::process_hdu(fitsfile *infptr, fitsfile *outfptr, int hdunum, int *status)
{
    fits_movabs_hdu(infptr, hdunum, NULL, status);

    int bitpix, naxis;
    long naxes[2] = {1, 1};
    fits_get_img_param(infptr, 2, &bitpix, &naxis, naxes, status);

    if (naxis != 2)
    {
        printf("HDU %d skipped (not 2D image).\n", hdunum);
        return;
    }

    long width = naxes[0], height = naxes[1];
    long npixels = width * height;
    double *img = (double *)malloc(npixels * sizeof(double));
    if (!img)
    {
        printf("Memory error.\n");
        exit(1);
    }

    long fpixel[2] = {1, 1};
    fits_read_pix(infptr, TDOUBLE, fpixel, npixels, NULL, img, NULL, status);

    long out_w, out_h;
    long dims[2] = {out_w, out_h};
    long out_pixels = (width / BIN_SIZE) * (height / BIN_SIZE);
    double *binned = (double *)malloc(out_pixels * sizeof(double));
    bin_image(img, width, height, binned, &out_w, &out_h);

    // 创建输出图像
    fits_create_img(outfptr, DOUBLE_IMG, 2, dims, status);
    fits_write_img(outfptr, TDOUBLE, 1, out_w * out_h, binned, status);

    free(img);
    free(binned);
}
int MainWindow::process_fixed()
{
    const char *infile = "/dev/shm/ccd_simulator_original.fits"; // 输入文件路径
    const char *outfile = "!/dev/shm/ccd_simulator_binned.fits"; // 输出文件路径
    // const char *outfile = "!merged_output.fits";  // 带 '!' 前缀，自动覆盖

    fitsfile *infptr = NULL, *outfptr = NULL;
    int status = 0, hdunum = 0, hdutype = 0;

    fits_open_file(&infptr, infile, READONLY, &status);
    if (status)
    {
        fits_report_error(stderr, status);
        return status;
    }

    fits_create_file(&outfptr, outfile, &status);
    if (status)
    {
        fits_report_error(stderr, status);
        fits_close_file(infptr, &status);
        return status;
    }

    fits_get_num_hdus(infptr, &hdunum, &status);
    for (int i = 1; i <= hdunum && status == 0; i++)
    {
        fits_movabs_hdu(infptr, i, &hdutype, &status);
        if (hdutype == IMAGE_HDU)
        {
            process_hdu(infptr, outfptr, i, &status);
        }
        else
        {
            fits_copy_hdu(infptr, outfptr, 0, &status);
        }
    }

    fits_close_file(infptr, &status);
    fits_close_file(outfptr, &status);

    if (status)
    {
        fits_report_error(stderr, status);
        return status;
    }

    printf("合并覆盖完成：ccd_simulator_binned.fits\n");
    return 0;
}


void MainWindow::sendRoiInfo()
{
    Logger::Log("==========================================", LogLevel::INFO, DeviceType::FOCUSER);
    for (auto it = roiAndFocuserInfo.begin(); it != roiAndFocuserInfo.end(); ++it)
    {
        Logger::Log("roiAndFocuserInfo | Key:" + it->first + " Value:" + std::to_string(it->second), LogLevel::INFO, DeviceType::FOCUSER);
    }
    Logger::Log("==========================================", LogLevel::INFO, DeviceType::FOCUSER);
    // 检查并获取参数，如果不存在则使用默认值
    double boxSideLength = roiAndFocuserInfo.count("BoxSideLength") ? roiAndFocuserInfo["BoxSideLength"] : 300;
    double roi_x = roiAndFocuserInfo.count("ROI_x") ? roiAndFocuserInfo["ROI_x"] : 1;
    double roi_y = roiAndFocuserInfo.count("ROI_y") ? roiAndFocuserInfo["ROI_y"] : 1;
    double visibleX = roiAndFocuserInfo.count("VisibleX") ? roiAndFocuserInfo["VisibleX"] : 0;
    double visibleY = roiAndFocuserInfo.count("VisibleY") ? roiAndFocuserInfo["VisibleY"] : 0;
    double scale = roiAndFocuserInfo.count("scale") ? roiAndFocuserInfo["scale"] : 1;
    double selectStarX = roiAndFocuserInfo.count("SelectStarX") ? roiAndFocuserInfo["SelectStarX"] : -1;
    double selectStarY = roiAndFocuserInfo.count("SelectStarY") ? roiAndFocuserInfo["SelectStarY"] : -1;
    const QPointF snapped = snapRoiOriginToBayerSafePhase(roi_x, roi_y,
                                                          static_cast<int>(std::lround(boxSideLength)),
                                                          static_cast<int>(std::lround(boxSideLength)));
    roi_x = snapped.x();
    roi_y = snapped.y();
    roiAndFocuserInfo["ROI_x"] = roi_x;
    roiAndFocuserInfo["ROI_y"] = roi_y;

    Logger::Log("sendRoiInfo | 发送参数 roi_x:" + std::to_string(roi_x) + " roi_y:" + std::to_string(roi_y) + " boxSideLength:" + std::to_string(boxSideLength) + " visibleX:" + std::to_string(visibleX) + " visibleY:" + std::to_string(visibleY) + " scale:" + std::to_string(scale) + " selectStarX:" + std::to_string(selectStarX) + " selectStarY:" + std::to_string(selectStarY), LogLevel::INFO, DeviceType::FOCUSER);

    // 发送参数
    emit wsThread->sendMessageToClient("SetRedBoxState:" + QString::number(boxSideLength) + ":" + QString::number(roi_x) + ":" + QString::number(roi_y));
    emit wsThread->sendMessageToClient("SetVisibleArea:" + QString::number(visibleX) + ":" + QString::number(visibleY) + ":" + QString::number(scale));
    emit wsThread->sendMessageToClient("SetSelectStars:" + QString::number(selectStarX) + ":" + QString::number(selectStarY));
}
void MainWindow::updateCPUInfo()
{
    // 获取CPU温度和使用率
    QProcess process;
    // 在树莓派上，可以通过读取 /sys/class/thermal/thermal_zone0/temp 文件来获取 CPU 温度
    process.start("cat", QStringList() << "/sys/class/thermal/thermal_zone0/temp");
    process.waitForFinished();
    QString output = process.readAllStandardOutput();
    float cpuTemp = output.toFloat() / 1000; // 转换为摄氏度
    if (process.error() != QProcess::UnknownError)
    {
        cpuTemp = std::numeric_limits<float>::quiet_NaN(); // 如果获取失败，设置为 NaN
    }

    // 在树莓派上，可以通过运行 'top' 命令并解析输出来获取 CPU 使用率
    process.start("sh", QStringList() << "-c" << "top -b -n1 | grep 'Cpu(s)' | awk '{print $2}' | cut -c 1-4");
    process.waitForFinished();
    output = process.readAllStandardOutput();
    QStringList cpuUsages = output.split("\n");
    float cpuUsage = 0;
    if (cpuUsages.size() > 0)
    {
        cpuUsage = cpuUsages[0].toDouble();
    }
    if (process.error() != QProcess::UnknownError)
    {
        cpuUsage = std::numeric_limits<float>::quiet_NaN(); // 如果获取失败，设置为 NaN
    }

    // Logger::Log("updateCPUInfo | CPU Temp: " + std::to_string(cpuTemp) + ", CPU Usage: " + std::to_string(cpuUsage), LogLevel::DEBUG, DeviceType::MAIN);
    emit wsThread->sendMessageToClient("updateCPUInfo:" + QString::number(cpuTemp) + ":" + QString::number(cpuUsage));
}

void MainWindow::getMainCameraParameters()
{
    Logger::Log("getMainCameraParameters start ...", LogLevel::DEBUG, DeviceType::MAIN);
    QMap<QString, QString> parameters = Tools::readParameters("MainCamera");
    QString order = "setMainCameraParameters";
    bool hasTileBuildMode = false;
    bool hasTileLevelMode = false;
    bool hasImageCfa = false;
    bool hasRoiCalcMode = false;
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
    {
        Logger::Log("getMainCameraParameters | " + it.key().toStdString() + ":" + it.value().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        if (it.key() == "ImageCFA") {
            hasImageCfa = true;
            const QString normalizedCfa = normalizeCfaPattern(it.value());
            MainCameraCFA = (normalizedCfa == "NULL" || normalizedCfa == "MONO") ? QString() : normalizedCfa;
            it.value() = MainCameraCFA.isEmpty() ? QStringLiteral("null") : MainCameraCFA;
        }
        if (it.key() == "ROICalcMode") {
            hasRoiCalcMode = true;
            const QString mode = it.value().trimmed().toLower();
            roiUseSelfCalcParams = (mode == "roi" || mode == "self");
            it.value() = roiUseSelfCalcParams ? QStringLiteral("roi") : QStringLiteral("full");
        }
        if (it.key() == "Save Folder" ) {
            QString oldSaveFolder = it.value();
            // 兼容旧的"default"，转换为"local"
            if (oldSaveFolder == "default") {
                oldSaveFolder = "local";
                it.value() = "local";
            }

            if (oldSaveFolder == "local") {
                ImageSaveBaseDirectory = QString::fromStdString(ImageSaveBasePath);
                saveMode = "local";
            } else if (usbMountPointsMap.contains(oldSaveFolder)) {
                ImageSaveBaseDirectory = usbMountPointsMap[oldSaveFolder] + "/QUARCS_ImageSave";
                saveMode = oldSaveFolder;
            } else {
                // U盘不存在，回退到本地
                it.value() = "local";
                ImageSaveBaseDirectory = QString::fromStdString(ImageSaveBasePath);
                saveMode = "local";
                Logger::Log("LoadParameter | USB '" + oldSaveFolder.toStdString() + "' not found, using local", LogLevel::WARNING, DeviceType::MAIN);
            }
        }
        if (it.key() == "Tile Build Mode") {
            if (it.value() == "merged_single_level") {
                Logger::Log("LoadParameter | Tile Build Mode 'merged_single_level' is deprecated, forcing pyramid",
                            LogLevel::WARNING, DeviceType::MAIN);
            }
            tileBuildMode = QStringLiteral("pyramid");
            it.value() = tileBuildMode;
            hasTileBuildMode = true;
        }
        if (it.key() == "Tile Level Mode") {
            const QString requested = it.value().trimmed().toLower();
            tileLevelMode = (requested == QStringLiteral("minmax")) ? QStringLiteral("minmax") : QStringLiteral("full");
            it.value() = tileLevelMode;
            hasTileLevelMode = true;
        }
        order += ":" + it.key() + ":" + it.value();
        if (it.key() == "RedBoxSize") {
            BoxSideLength = it.value().toInt();
            roiAndFocuserInfo["BoxSideLength"] = BoxSideLength;
        }
        if (it.key() == "ROI_x") roiAndFocuserInfo["ROI_x"] = it.value().toDouble();
        if (it.key() == "ROI_y") roiAndFocuserInfo["ROI_y"] = it.value().toDouble();
        if (it.key() == "AutoSave") {
            mainCameraAutoSave = (it.value() == "true");
            // Logger::Log("/*/*/*/*/*/*getMainCameraParameters | AutoSave: " + std::to_string(mainCameraAutoSave), LogLevel::DEBUG, DeviceType::MAIN);
        }
        if (it.key() == "SaveFailedParse") {
            mainCameraSaveFailedParse = (it.value() == "true");
        }
        if (it.key() == "Temperature") {
            CameraTemperature = it.value().toDouble();
        }
        if (it.key() == "Gain") {
            CameraGain = it.value().toInt();
        }
        if (it.key() == "Offset") {
            ImageOffset = it.value().toDouble();
        }
        if (it.key() == "USB Traffic") {
            glUsbTrafficValue = it.value().toInt();
        }
    }
    if (!hasTileBuildMode) {
        tileBuildMode = QStringLiteral("pyramid");
        order += ":Tile Build Mode:" + tileBuildMode;
    }
    if (!hasTileLevelMode) {
        tileLevelMode = QStringLiteral("full");
        order += ":Tile Level Mode:" + tileLevelMode;
    }
    if (!hasRoiCalcMode) {
        roiUseSelfCalcParams = false;
        order += ":ROICalcMode:full";
    }
    if (!hasImageCfa) {
        MainCameraCFA = normalizeCfaPattern(MainCameraCFA);
        if (MainCameraCFA == "NULL" || MainCameraCFA == "MONO") {
            MainCameraCFA.clear();
        }
    }
    Logger::Log("getMainCameraParameters finish!", LogLevel::DEBUG, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(order);

    emit wsThread->sendMessageToClient("MainCameraCFA:" + (MainCameraCFA.isEmpty() ? QStringLiteral("null") : MainCameraCFA));
    emit wsThread->sendMessageToClient("MainCameraCFASource:SAVED");
}





void MainWindow::getLastSelectDevice()
{
    SystemDeviceList newSystemdevicelist = Tools::readSystemDeviceList();

    // 检查是否有历史设备配置记录
    bool hasHistoryConfig = false;
    for (int i = 0; i < newSystemdevicelist.system_devices.size(); i++)
    {
        if (!newSystemdevicelist.system_devices[i].DriverIndiName.isEmpty() &&
            !newSystemdevicelist.system_devices[i].Description.isEmpty())
        {
            hasHistoryConfig = true;
            break;
        }
    }

    if (!hasHistoryConfig)
    {
        Logger::Log("No historical connection records found", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 只有当未连接设备时才触发设备选择
    if (ConnectedDevices.size() == 0)
    {
        systemdevicelist = newSystemdevicelist;
        loadSelectedDriverList();
        Logger::Log("Last Connected Device Has Send", LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("Devices already connected, skip loading last selection", LogLevel::INFO, DeviceType::MAIN);
    }
}

// [停用 2026-04-14] 旧自动电调校准实现：保留函数体用于历史回溯，当前不再通过命令入口触发。
void MainWindow::getCheckBoxSpace()
{
    // 计算应用所在分区的可用空间，避免与实际存储分区不一致
    QString path = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    path = QDir::rootPath();
#endif
    QFileInfo fi(path);
    quint64 freeBytes = 0;
    if (fi.exists())
    {
        QStorageInfo storage(path);
        freeBytes = static_cast<quint64>(storage.bytesAvailable());
    }
    // 发送到前端：Box_Space:<bytes>
    if (wsThread)
    {
        emit wsThread->sendMessageToClient("Box_Space:" + QString::number(freeBytes));
    }
}

void MainWindow::clearLogs()
{
    // 按 Logger::Initialize 约定：运行目录下 logs/MAIN.log 等
    const QString logDirPath = QDir::currentPath() + "/logs";
    QDir logDir(logDirPath);
    if (logDir.exists())
    {
        QFileInfoList entries = logDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &fi : entries)
        {
            // 清空内容而不删除文件
            QFile f(fi.absoluteFilePath());
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                f.close();
            }
        }
    }
    if (wsThread) emit wsThread->sendMessageToClient("ClearLogs:Success");
}

void MainWindow::clearBoxCache(bool clearCache, bool clearUpdatePack, bool clearBackup)
{
    auto clearDirContents = [](const QString &dirPath)
    {
        if (dirPath.isEmpty()) return;
        QDir dir(dirPath);
        if (!dir.exists()) return;
        // 删除目录下的所有条目（文件/链接/目录），保留顶层目录本身
        QFileInfoList entries = dir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden);
        for (const QFileInfo &fi : entries)
        {
            if (fi.isSymLink() || fi.isFile())
            {
                QFile::remove(fi.absoluteFilePath());
            }
            else if (fi.isDir())
            {
                QDir sub(fi.absoluteFilePath());
                sub.removeRecursively();
            }
        }
    };
    auto clearFileIfExists = [](const QString &filePath)
    {
        if (filePath.isEmpty()) return;
        QFileInfo fi(filePath);
        if (!fi.exists() || !fi.isFile()) return;
        QFile::remove(filePath);
    };

    // 1. 清理系统/应用缓存与回收站
    if (clearCache)
    {
        QStringList caches;
        // 用户主目录缓存根（比单纯的 CacheLocation 更全面）
        caches << (QDir::homePath() + "/.cache");
        caches << QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
               << QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
               << QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        // 额外补充持久临时目录与常见开发工具缓存
        caches << "/var/tmp";
        caches << (QDir::homePath() + "/.cursor-server/data/CachedExtensionVSIXs");
        // XDG 垃圾箱（当前用户）常规路径与 XDG_DATA_HOME 路径
        const QString trashBase = QDir::homePath() + "/.local/share/Trash";
        caches << (trashBase + "/files") << (trashBase + "/info") << (trashBase + "/expunged");
        const QString xdgDataHome = qEnvironmentVariableIsSet("XDG_DATA_HOME")
                                    ? QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"))
                                    : (QDir::homePath() + "/.local/share");
        caches << (xdgDataHome + "/Trash/files")
               << (xdgDataHome + "/Trash/info")
               << (xdgDataHome + "/Trash/expunged");
        // 常见桌面环境的垃圾箱路径（可能存在）
        caches << (QDir::homePath() + "/.Trash")
               << (QDir::homePath() + "/.Trash-1000/files")
               << (QDir::homePath() + "/.Trash-1000/info");

        for (const QString &p : caches) clearDirContents(p);

        // 尝试调用 gio 清空垃圾箱（若环境支持）
        QProcess::execute("gio", QStringList() << "trash" << "--empty");

        // 清空可移动介质等可能挂载点的垃圾箱（.Trash-UID 或 .Trash）
        QString uidStr = QString::number(getuid());
        QStringList mountRoots;
        mountRoots << "/mnt" << "/media" << "/run/media";
        for (const QString &root : mountRoots)
        {
            QDir rootDir(root);
            if (!rootDir.exists()) continue;
            QFileInfoList subs = rootDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Dirs);
            for (const QFileInfo &fi : subs)
            {
                const QString base = fi.absoluteFilePath();
                clearDirContents(base + "/.Trash-" + uidStr + "/files");
                clearDirContents(base + "/.Trash-" + uidStr + "/info");
                clearDirContents(base + "/.Trash-" + uidStr + "/expunged");
                clearDirContents(base + "/.Trash/files");
                clearDirContents(base + "/.Trash/info");
                clearDirContents(base + "/.Trash/expunged");
            }
        }

        // 清理 apt 缓存与包索引，避免更新缓存长期堆积。
        clearDirContents("/var/cache/apt");
        clearDirContents("/var/lib/apt/lists");

        // 清掉常见锁文件，避免目录已清空但索引残留。
        clearFileIfExists("/var/lib/apt/lists/lock");
        clearFileIfExists("/var/cache/apt/archives/lock");

        // 限制 systemd journal 体积；失败时静默跳过，避免无 sudo 能力时中断清理。
        runSudoSync("/usr/bin/journalctl", {"--vacuum-size=100M"}, 15000);
    }

    // 2. 可选：清理更新包目录
    if (clearUpdatePack)
    {
        clearDirContents("/var/www/update_pack");
    }

    // 3. 可选：清理备份目录
    if (clearBackup)
    {
        clearDirContents("/home/quarcs/workspace/QUARCS/backup");
    }

    if (wsThread) emit wsThread->sendMessageToClient("ClearBoxCache:Success");
}
