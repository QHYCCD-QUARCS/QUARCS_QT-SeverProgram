#include "mainwindow_command_support.h"

int MainWindow::CaptureImageSave()
{
    Logger::Log("CaptureImageSave...", LogLevel::INFO, DeviceType::MAIN);
    const QString sourcePath = latestMainCaptureFitsPath();

    if (sourcePath.isEmpty())
    {
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
        return 1;
    }

    QString CaptureTime = Tools::getFitsCaptureTime(sourcePath.toUtf8().constData());
    Logger::Log("CaptureImageSave | getFitsCaptureTime returned: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // 如果无法从 FITS 文件获取时间，优先使用文件的修改时间
    if (CaptureTime.isEmpty())
    {
        QFileInfo fileInfo(sourcePath);
        if (fileInfo.exists())
        {
            // 使用文件的最后修改时间
            QDateTime fileTime = fileInfo.lastModified();
            CaptureTime = fileTime.toString("yyyy_MM_dd_HH_mm_ss");
            Logger::Log("CaptureImageSave | Using file modification time as filename: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            // 如果文件不存在（理论上不应该发生），使用当前时间作为最后的fallback
            std::time_t currentTime = std::time(nullptr);
            std::tm *timeInfo = std::localtime(&currentTime);
            char buffer[80];
            std::strftime(buffer, 80, "%Y_%m_%dT%H_%M_%S", timeInfo);
            CaptureTime = QString::fromStdString(buffer);
            Logger::Log("CaptureImageSave | Using current timestamp as filename: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
    }

    CaptureTime.replace(QRegExp("[^a-zA-Z0-9]"), "_");
    QString resultFileName = CaptureTime + ".fits";
    Logger::Log("CaptureImageSave | Generated filename: " + resultFileName.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD

    // 直接使用 ImageSaveBaseDirectory（无论是默认路径还是U盘路径）
    QString destinationDirectory = ImageSaveBaseDirectory + "/CaptureImage";
    QString destinationPath = destinationDirectory + "/" + QString(buffer) + "/" + resultFileName;

    // 判断是否为U盘路径（使用saveMode参数）
    bool isUSBSave = (saveMode != "local");

    // 使用通用函数检查存储空间并创建目录
    QString dirPathToCreate = isUSBSave ? (destinationDirectory + "/" + QString(buffer)) : QString();
    int checkResult = checkStorageSpaceAndCreateDirectory(
        sourcePath,
        destinationDirectory,
        dirPathToCreate,
        "CaptureImageSave",
        isUSBSave,
        [this]() { createCaptureDirectory(); }
    );
    if (checkResult != 0)
    {
        return checkResult;
    }

    // 检查文件是否已存在
    if (QFile::exists(destinationPath))
    {
        Logger::Log("The file already exists, there is no need to save it again:" + destinationPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Repeat");
        return 0;
    }

    // 使用通用函数保存文件
    int saveResult = saveImageFile(sourcePath, destinationPath, "CaptureImageSave", isUSBSave);
    if (saveResult != 0)
    {
        return saveResult;
    }

    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Success");
    Logger::Log("CaptureImageSave | File saved successfully: " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    return 0;
}

int MainWindow::solveFailedImageSave(const QString& imagePath)
{
    // Logger::Log("solveFailedImageSave...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("solveFailedImageSave | Starting save process, imagePath: " + imagePath.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // 如果未提供路径，使用默认路径
    QString sourcePathStr = imagePath.isEmpty() ? "/dev/shm/ccd_simulator.fits" : imagePath;
    const char *sourcePath = sourcePathStr.toLocal8Bit().constData();

    Logger::Log("solveFailedImageSave | Using source path: " + sourcePathStr.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if (!QFile::exists(sourcePathStr))
    {
        Logger::Log("solveFailedImageSave | 文件不存在: " + sourcePathStr.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
        return 1;
    }

    Logger::Log("solveFailedImageSave | Source file exists, file size: " + std::to_string(QFileInfo(sourcePathStr).size()) + " bytes", LogLevel::INFO, DeviceType::MAIN);

    QString CaptureTime = Tools::getFitsCaptureTime(sourcePath);
    Logger::Log("solveFailedImageSave | getFitsCaptureTime returned: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // 如果无法从 FITS 文件获取时间，优先使用文件的修改时间
    if (CaptureTime.isEmpty())
    {
        QFileInfo fileInfo(sourcePathStr);
        if (fileInfo.exists())
        {
            // 使用文件的最后修改时间
            QDateTime fileTime = fileInfo.lastModified();
            CaptureTime = fileTime.toString("yyyy_MM_dd_HH_mm_ss");
            Logger::Log("solveFailedImageSave | Using file modification time as filename: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            // 如果文件不存在（理论上不应该发生），使用当前时间作为最后的fallback
            std::time_t currentTime = std::time(nullptr);
            std::tm *timeInfo = std::localtime(&currentTime);
            char buffer[80];
            std::strftime(buffer, 80, "%Y_%m_%dT%H_%M_%S", timeInfo);
            CaptureTime = QString::fromStdString(buffer);
            Logger::Log("solveFailedImageSave | Using current timestamp as filename: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
    }

    CaptureTime.replace(QRegExp("[^a-zA-Z0-9]"), "_");
    QString resultFileName = CaptureTime + ".fits";
    Logger::Log("solveFailedImageSave | Generated filename: " + resultFileName.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD

    // 指定目标目录
    QString destinationDirectory = ImageSaveBaseDirectory + "/solveFailedImage";

    QString destinationPath = destinationDirectory + "/" + buffer + "/" + resultFileName;

    // 判断是否为U盘路径（使用saveMode参数）
    bool isUSBSave = (saveMode != "local");

    // 使用通用函数检查存储空间并创建目录
    // 注意：传入 QString 而不是 const char*，确保路径正确传递
    QString dirPathToCreate = isUSBSave ? (destinationDirectory + "/" + QString(buffer)) : QString();

    // 在调用前再次确认文件存在（因为文件可能在检查后被删除）
    if (!QFile::exists(sourcePathStr))
    {
        Logger::Log("solveFailedImageSave | Source file no longer exists before checkStorageSpaceAndCreateDirectory: " + sourcePathStr.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
        return 1;
    }

    int checkResult = checkStorageSpaceAndCreateDirectory(
        sourcePathStr,  // 使用 QString 而不是 const char*
        destinationDirectory,
        dirPathToCreate,
        "solveFailedImageSave",
        isUSBSave,
        [this]() { createsolveFailedImageDirectory(); }
    );
    if (checkResult != 0)
    {
        Logger::Log("solveFailedImageSave | checkStorageSpaceAndCreateDirectory failed with code: " + std::to_string(checkResult), LogLevel::ERROR, DeviceType::MAIN);
        return checkResult;
    }

    // 检查文件是否已存在
    // if (QFile::exists(destinationPath))
    // {
    //     qWarning() << "The file already exists, there is no need to save it again:" << destinationPath;
    //     emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Repeat");
    //     return 0;
    // }

    // 使用通用函数保存文件
    // 在保存前再次确认源文件存在
    if (!QFile::exists(sourcePathStr))
    {
        Logger::Log("solveFailedImageSave | Source file no longer exists before saveImageFile: " + sourcePathStr.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
        return 1;
    }

    Logger::Log("solveFailedImageSave | Attempting to save to: " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    int saveResult = saveImageFile(sourcePathStr, destinationPath, "solveFailedImageSave", isUSBSave);  // 使用 QString 而不是 const char*
    if (saveResult != 0)
    {
        Logger::Log("solveFailedImageSave | saveImageFile failed with error code: " + std::to_string(saveResult), LogLevel::ERROR, DeviceType::MAIN);
        return saveResult;
    }

    // 验证文件是否真的被保存了
    if (QFile::exists(destinationPath))
    {
        Logger::Log("solveFailedImageSave | File saved successfully to: " + destinationPath.toStdString() + ", size: " + std::to_string(QFileInfo(destinationPath).size()) + " bytes", LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("solveFailedImageSave | WARNING: saveImageFile returned success but destination file does not exist: " + destinationPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
    }

    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Success");
    qDebug() << "CaptureImageSaveStatus Goto Complete...";
    return 0;
}

bool MainWindow::directoryExists(const std::string &path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool MainWindow::createCaptureDirectory()
{
    Logger::Log("createCaptureDirectory start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/CaptureImage/";

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD
    std::string folderName = basePath + buffer;

    // 如果目录不存在，则创建
    if (!std::filesystem::exists(folderName))
    {
        if (std::filesystem::create_directory(folderName))
        {
            Logger::Log("createCaptureDirectory | Folder created successfully: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("createCaptureDirectory | An error occurred while creating the folder.", LogLevel::INFO, DeviceType::MAIN);
        }
    }
    else
    {
        Logger::Log("createCaptureDirectory | The folder already exists: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
    }
    return true;
}

bool MainWindow::createsolveFailedImageDirectory()
{
    Logger::Log("createCaptureDirectory start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/solveFailedImage/";

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD
    std::string folderName = basePath + buffer;

    // 如果目录不存在，则创建
    if (!std::filesystem::exists(folderName))
    {
        if (std::filesystem::create_directory(folderName))
        {
            Logger::Log("createCaptureDirectory | Folder created successfully: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("createCaptureDirectory | An error occurred while creating the folder.", LogLevel::INFO, DeviceType::MAIN);
        }
    }
    else
    {
        Logger::Log("createCaptureDirectory | The folder already exists: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
    }
    return true;
}

int MainWindow::checkStorageSpaceAndCreateDirectory(const QString &sourcePath,
                                                     const QString &destinationDirectory,
                                                     const QString &dirPathToCreate,
                                                     const QString &functionName,
                                                     bool isUSBSave,
                                                     std::function<void()> createLocalDirectoryFunc)
{
    Logger::Log(functionName.toStdString() + " | checkStorageSpaceAndCreateDirectory | saveMode: " + saveMode.toStdString() +
               ", isUSBSave: " + std::string(isUSBSave ? "true" : "false") +
               ", ImageSaveBaseDirectory: " + ImageSaveBaseDirectory.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // 先获取源文件大小（在空间检查之前）
    QFileInfo sourceFileInfo(sourcePath);
    if (!sourceFileInfo.exists())
    {
        Logger::Log(functionName.toStdString() + " | Source file does not exist.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
        return 1;
    }
    long long fileSize = sourceFileInfo.size();

    if (isUSBSave)
    {
        // 从ImageSaveBaseDirectory提取U盘挂载点（去掉/QUARCS_ImageSave）
        QString usb_mount_point = ImageSaveBaseDirectory;
        usb_mount_point.replace("/QUARCS_ImageSave", "");

        Logger::Log(functionName.toStdString() + " | USB save mode | ImageSaveBaseDirectory: " + ImageSaveBaseDirectory.toStdString() +
                   ", extracted USB mount point: " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        // 检查U盘空间和可写性
        QStorageInfo storageInfo(usb_mount_point);
        if (!storageInfo.isValid() || !storageInfo.isReady())
        {
            Logger::Log(functionName.toStdString() + " | USB drive is not valid or not ready: " + usb_mount_point.toStdString() +
                       " (isValid: " + std::string(storageInfo.isValid() ? "true" : "false") +
                       ", isReady: " + std::string(storageInfo.isReady() ? "true" : "false") + ")", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:USB-NotAvailable");
            return 1;
        }

        if (storageInfo.isReadOnly())
        {
            const QString password = "quarcs";
            if (!remountReadWrite(usb_mount_point, password))
            {
                Logger::Log(functionName.toStdString() + " | Failed to remount USB as read-write.", LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("CaptureImageSaveStatus:USB-ReadOnly");
                return 1;
            }
        }

        // 检查U盘剩余空间（在创建目录之前）
        long long remaining_space = getUSBSpace(usb_mount_point);
        if (remaining_space == -1 || remaining_space <= 0)
        {
            Logger::Log(functionName.toStdString() + " | USB drive has no available space.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:USB-NoSpace");
            return 1;
        }

        // 预留至少100MB的缓冲空间，避免写入时空间不足
        const long long RESERVE_SPACE = 100 * 1024 * 1024; // 100MB
        long long available_space = remaining_space - RESERVE_SPACE;
        if (available_space < 0)
        {
            available_space = 0;
        }

        // 检查空间是否足够（文件大小必须小于可用空间，已预留缓冲）
        if (fileSize > available_space)
        {
            Logger::Log(functionName.toStdString() + " | Insufficient USB space. Required: " + QString::number(fileSize).toStdString() +
                       " bytes, Available: " + QString::number(remaining_space).toStdString() +
                       " bytes (reserved: " + QString::number(RESERVE_SPACE).toStdString() + " bytes)", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:USB-NoSpace");
            return 1;
        }

        // 创建目录（使用sudo）- 在空间检查通过后
        // 安全检查：避免在 /media/quarcs 路径下创建任何文件夹，避免被错误识别为U盘
        QString normalizedPath = QDir(dirPathToCreate).absolutePath();

        // 检查路径是否在 /media/quarcs 下
        if (normalizedPath.startsWith("/media/quarcs/"))
        {
            // 提取 /media/quarcs/ 之后的部分
            QString pathAfterMedia = normalizedPath.mid(14); // 去掉 "/media/quarcs/"

            // 检查路径格式：应该是 /media/quarcs/某个U盘名/...
            int firstSlash = pathAfterMedia.indexOf('/');
            if (firstSlash > 0)
            {
                QString usbName = pathAfterMedia.left(firstSlash);
                // 检查这个U盘名是否在映射表中（有效的U盘挂载点）
                if (!usbMountPointsMap.contains(usbName))
                {
                    Logger::Log(functionName.toStdString() + " | Security check failed: Attempting to create directory in /media/quarcs/ but USB name '" + usbName.toStdString() + "' not found in mount points map. Path: " + dirPathToCreate.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                    return 1;
                }
                // 验证路径确实在U盘挂载点下
                QString expectedMountPoint = "/media/quarcs/" + usbName;
                if (!normalizedPath.startsWith(expectedMountPoint))
                {
                    Logger::Log(functionName.toStdString() + " | Security check failed: Path does not match expected mount point. Path: " + dirPathToCreate.toStdString() + ", Expected mount point: " + expectedMountPoint.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                    return 1;
                }
            }
            else
            {
                // 路径格式不正确，可能是直接在 /media/quarcs/ 下创建文件夹
                Logger::Log(functionName.toStdString() + " | Security check failed: Invalid path format in /media/quarcs/. Path: " + dirPathToCreate.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                return 1;
            }
        }
        // 额外检查：确保路径不是直接在 /media/quarcs 下（没有子目录）
        else if (normalizedPath == "/media/quarcs")
        {
            Logger::Log(functionName.toStdString() + " | Security check failed: Attempting to create directory directly at /media/quarcs. Path: " + dirPathToCreate.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }

        const QString password = "quarcs";
        QProcess mkdirProcess;
        mkdirProcess.start("sudo", {"-S", "mkdir", "-p", dirPathToCreate});
        if (!mkdirProcess.waitForStarted() || !mkdirProcess.write((password + "\n").toUtf8()))
        {
            Logger::Log(functionName.toStdString() + " | Failed to create directory: " + dirPathToCreate.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }
        mkdirProcess.closeWriteChannel();
        mkdirProcess.waitForFinished(-1);
    }
    else
    {
        // 默认位置：先检查空间（在创建目录之前）
        QString localPath = QString::fromStdString(ImageSaveBasePath);
        Logger::Log(functionName.toStdString() + " | Local save mode | checking local path: " + localPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        long long remaining_space = getUSBSpace(localPath);
        if (remaining_space == -1 || remaining_space <= 0)
        {
            Logger::Log(functionName.toStdString() + " | Local storage has no available space.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:NoSpace");
            return 1;
        }

        // 预留至少100MB的缓冲空间，避免写入时空间不足
        const long long RESERVE_SPACE = 100 * 1024 * 1024; // 100MB
        long long available_space = remaining_space - RESERVE_SPACE;
        if (available_space < 0)
        {
            available_space = 0;
        }

        // 检查空间是否足够（文件大小必须小于可用空间，已预留缓冲）
        if (fileSize > available_space)
        {
            Logger::Log(functionName.toStdString() + " | Insufficient local storage space. Required: " + QString::number(fileSize).toStdString() +
                       " bytes, Available: " + QString::number(remaining_space).toStdString() +
                       " bytes (reserved: " + QString::number(RESERVE_SPACE).toStdString() + " bytes)", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:NoSpace");
            return 1;
        }

        // 创建目录 - 在空间检查通过后
        if (createLocalDirectoryFunc)
        {
            createLocalDirectoryFunc();
        }
    }

    return 0;
}

int MainWindow::saveImageFile(const QString &sourcePath,
                              const QString &destinationPath,
                              const QString &functionName,
                              bool isUSBSave)
{
    if (isUSBSave)
    {
        // U盘保存使用sudo cp命令
        const QString password = "quarcs";
        QProcess cpProcess;
        cpProcess.start("sudo", {"-S", "cp", sourcePath, destinationPath});
        if (!cpProcess.waitForStarted() || !cpProcess.write((password + "\n").toUtf8()))
        {
            Logger::Log(functionName.toStdString() + " | Failed to execute sudo cp command.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }
        cpProcess.closeWriteChannel();
        cpProcess.waitForFinished(-1);

        if (cpProcess.exitCode() != 0)
        {
            QByteArray stderrOutput = cpProcess.readAllStandardError();
            Logger::Log(functionName.toStdString() + " | Failed to copy file to USB: " + QString::fromUtf8(stderrOutput).toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }

        Logger::Log(functionName.toStdString() + " | File saved to USB: " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        // 默认位置保存使用普通文件操作
        // 将相对路径转换为绝对路径
        QString absoluteDestinationPath = destinationPath;
        if (!QDir::isAbsolutePath(destinationPath))
        {
            absoluteDestinationPath = QDir::currentPath() + "/" + destinationPath;
            Logger::Log(functionName.toStdString() + " | Converted relative path to absolute: " + destinationPath.toStdString() + " -> " + absoluteDestinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        const QByteArray destinationPathBytes = absoluteDestinationPath.toUtf8();
        const char *destinationPathChar = destinationPathBytes.constData();
        const QByteArray sourcePathBytes = sourcePath.toUtf8();
        const char *sourcePathChar = sourcePathBytes.constData();

        // 确保目标目录存在
        std::filesystem::path destPath(destinationPathChar);
        std::filesystem::path destDir = destPath.parent_path();
        if (!destDir.empty())
        {
            if (!std::filesystem::exists(destDir))
            {
                try {
                    if (!std::filesystem::create_directories(destDir))
                    {
                        Logger::Log(functionName.toStdString() + " | Failed to create destination directory: " + destDir.string(), LogLevel::ERROR, DeviceType::MAIN);
                        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                        return 1;
                    }
                    Logger::Log(functionName.toStdString() + " | Created destination directory: " + destDir.string(), LogLevel::INFO, DeviceType::MAIN);
                } catch (const std::filesystem::filesystem_error& e) {
                    Logger::Log(functionName.toStdString() + " | Exception creating directory: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                    return 1;
                }
            }
            else
            {
                // 检查目录是否可写
                try {
                    std::filesystem::perms dirPerms = std::filesystem::status(destDir).permissions();
                    if ((dirPerms & std::filesystem::perms::owner_write) == std::filesystem::perms::none)
                    {
                        Logger::Log(functionName.toStdString() + " | Destination directory is not writable: " + destDir.string(), LogLevel::ERROR, DeviceType::MAIN);
                        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                        return 1;
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    Logger::Log(functionName.toStdString() + " | Exception checking directory permissions: " + std::string(e.what()), LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }

        // 检查目标文件是否已存在（处理竞态条件）
        if (std::filesystem::exists(destPath))
        {
            Logger::Log(functionName.toStdString() + " | Target file already exists, attempting to remove: " + std::string(destinationPathChar), LogLevel::WARNING, DeviceType::MAIN);
            try {
                if (!std::filesystem::remove(destPath))
                {
                    Logger::Log(functionName.toStdString() + " | Failed to remove existing file: " + std::string(destinationPathChar), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                    return 1;
                }
                Logger::Log(functionName.toStdString() + " | Removed existing file: " + std::string(destinationPathChar), LogLevel::INFO, DeviceType::MAIN);
            } catch (const std::filesystem::filesystem_error& e) {
                Logger::Log(functionName.toStdString() + " | Exception removing existing file: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                return 1;
            }
        }

        std::ifstream sourceFile(sourcePathChar, std::ios::binary);
        if (!sourceFile.is_open())
        {
            Logger::Log(functionName.toStdString() + " | Unable to open source file: " + sourcePath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }

        std::ofstream destinationFile(destinationPathChar, std::ios::binary | std::ios::trunc);
        if (!destinationFile.is_open())
        {
            std::string dirInfo = "unknown";
            try {
                if (std::filesystem::exists(destDir))
                {
                    bool writable = (std::filesystem::status(destDir).permissions() & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
                    dirInfo = "exists: yes, writable: " + std::string(writable ? "yes" : "no");
                }
                else
                {
                    dirInfo = "exists: no";
                }
            } catch (...) {
                dirInfo = "exists: unknown (exception)";
            }
            Logger::Log(functionName.toStdString() + " | Unable to create or open target file: " + std::string(destinationPathChar) +
                       " | " + dirInfo,
                       LogLevel::ERROR, DeviceType::MAIN);
            sourceFile.close();
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }

        destinationFile << sourceFile.rdbuf();

        sourceFile.close();
        destinationFile.close();

        // 验证文件是否成功写入
        if (!std::filesystem::exists(destPath))
        {
            Logger::Log(functionName.toStdString() + " | File write completed but file does not exist: " + std::string(destinationPathChar), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }

        Logger::Log(functionName.toStdString() + " | File saved successfully to: " + std::string(destinationPathChar) +
                   " | File size: " + std::to_string(std::filesystem::file_size(destPath)) + " bytes",
                   LogLevel::INFO, DeviceType::MAIN);
    }

    return 0;
}

void MainWindow::DeleteImage(QStringList DelImgPath)
{
    std::string password = "quarcs"; // sudo 密码
    for (int i = 0; i < DelImgPath.size(); i++)
    {
        if (i < DelImgPath.size())
        {
            QString path = DelImgPath[i].trimmed();
            std::string pathForRm = path.toStdString();
            if (!path.isEmpty() && !QDir::isAbsolutePath(path))
                pathForRm = "./" + pathForRm;
            std::ostringstream commandStream;
            commandStream << "echo '" << password << "' | sudo -S rm -rf \"" << pathForRm << "\"";
            std::string command = commandStream.str();

            Logger::Log("DeleteImage | Deleted command:" + QString::fromStdString(command).toStdString(), LogLevel::INFO, DeviceType::MAIN);

            // 执行系统命令删除文件
            int result = system(command.c_str());

            if (result == 0)
            {
                Logger::Log("DeleteImage | Deleted file:" + DelImgPath[i].toStdString(), LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                Logger::Log("DeleteImage | Failed to delete file:" + DelImgPath[i].toStdString(), LogLevel::INFO, DeviceType::MAIN);
            }
        }
        else
        {
            Logger::Log("DeleteImage | Index out of range: " + std::to_string(i), LogLevel::WARNING, DeviceType::MAIN);
        }
    }
}

std::string MainWindow::GetAllFile()
{
    Logger::Log("GetAllFile start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string capturePath = ImageSaveBasePath + "/CaptureImage/";
    std::string planPath = ImageSaveBasePath + "/ScheduleImage/";
    std::string solveFailedImagePath = ImageSaveBasePath + "/solveFailedImage/";
    std::string resultString;
    std::string captureString = "CaptureImage{";
    std::string planString = "ScheduleImage{";
    std::string solveFailedImageString = "SolveFailedImage{";

    try
    {
        // 检查并处理 CaptureImage 目录
        if (std::filesystem::exists(capturePath) && std::filesystem::is_directory(capturePath))
        {
            for (const auto &entry : std::filesystem::directory_iterator(capturePath))
            {
                std::string fileName = entry.path().filename().string(); // 获取文件名（包含扩展名）
                captureString += fileName + ";";                         // 拼接为字符串
            }
        }
        else
        {
            Logger::Log("GetAllFile | CaptureImage directory does not exist or is not a directory: " + capturePath, LogLevel::WARNING, DeviceType::MAIN);
        }

        // 检查并处理 ScheduleImage 目录
        if (std::filesystem::exists(planPath) && std::filesystem::is_directory(planPath))
        {
            for (const auto &entry : std::filesystem::directory_iterator(planPath))
            {
                std::string folderName = entry.path().filename().string(); // 获取文件夹名
                planString += folderName + ";";
            }
        }
        else
        {
            Logger::Log("GetAllFile | ScheduleImage directory does not exist or is not a directory: " + planPath, LogLevel::WARNING, DeviceType::MAIN);
        }
        // 检查并处理 solveFailedImage 目录
        if (std::filesystem::exists(solveFailedImagePath) && std::filesystem::is_directory(solveFailedImagePath))
        {
            for (const auto &entry : std::filesystem ::directory_iterator(solveFailedImagePath))
            {
                std::string fileName = entry.path().filename().string(); // 获取文件名（包含扩展名）
                solveFailedImageString += fileName + ";";                // 拼接为字符串
            }
        }
        else
        {
            Logger::Log("GetAllFile | SolveFailedImage directory does not exist or is not a directory: " + solveFailedImagePath, LogLevel::WARNING, DeviceType::MAIN);
            // solveFailedImageString = "SolveFailedImage{}"; // 如果目录不存在，返回空字符串
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        Logger::Log("GetAllFile | Filesystem error: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    }
    catch (const std::exception &e)
    {
        Logger::Log("GetAllFile | General error: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    }

    resultString = captureString + "}:" + planString + "}:" + solveFailedImageString + '}';
    Logger::Log("GetAllFile finish!", LogLevel::INFO, DeviceType::MAIN);
    return resultString;
}

void MainWindow::GetImageFiles(std::string ImageFolder)
{
    Logger::Log("GetImageFiles start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/" + ImageFolder + "/";
    std::string ImageFilesNameString = "";

    try
    {
        // 检查目录是否存在
        if (!std::filesystem::exists(basePath))
        {
            Logger::Log("GetImageFiles | Directory does not exist: " + basePath, LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ImageFilesName:");
            Logger::Log("GetImageFiles finish! (Directory not found)", LogLevel::INFO, DeviceType::MAIN);
            return;
        }

        if (!std::filesystem::is_directory(basePath))
        {
            Logger::Log("GetImageFiles | Path is not a directory: " + basePath, LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ImageFilesName:");
            Logger::Log("GetImageFiles finish! (Not a directory)", LogLevel::INFO, DeviceType::MAIN);
            return;
        }

        for (const auto &entry : std::filesystem::directory_iterator(basePath))
        {
            std::string fileName = entry.path().filename().string(); // 获取文件名（包含扩展名）
            ImageFilesNameString += fileName + ";";                  // 拼接为字符串
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        Logger::Log("GetImageFiles | Filesystem error: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ImageFilesName:");
        Logger::Log("GetImageFiles finish! (Filesystem error)", LogLevel::INFO, DeviceType::MAIN);
        return;
    }
    catch (const std::exception &e)
    {
        Logger::Log("GetImageFiles | General error: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ImageFilesName:");
        Logger::Log("GetImageFiles finish! (General error)", LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    Logger::Log("GetImageFiles | Image Files:" + QString::fromStdString(ImageFilesNameString).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient("ImageFilesName:" + QString::fromStdString(ImageFilesNameString));
    Logger::Log("GetImageFiles finish!", LogLevel::INFO, DeviceType::MAIN);
}

QStringList MainWindow::parseString(const std::string &input, const std::string &imgFilePath)
{
    QStringList paths;
    QString baseString;
    size_t pos = input.find('{');
    if (pos != std::string::npos)
    {
        baseString = QString::fromStdString(input.substr(0, pos));
        std::string content = input.substr(pos + 1);
        size_t endPos = content.find('}');
        if (endPos != std::string::npos)
        {
            content = content.substr(0, endPos);

            // 去掉末尾的分号（如果有的话）
            if (!content.empty() && content.back() == ';')
            {
                content.pop_back();
            }

            QStringList parts = QString::fromStdString(content).split(';', Qt::SkipEmptyParts);
            for (const QString &part : parts)
            {
                QString path = QDir::toNativeSeparators(QString::fromStdString(imgFilePath) + "/" + baseString + "/" + part);
                paths.append(path);
            }
        }
    }
    return paths;
}

long long MainWindow::getUSBSpace(const QString &usb_mount_point)
{
    Logger::Log("getUSBSpace start ...", LogLevel::INFO, DeviceType::MAIN);
    struct statvfs stat;
    if (statvfs(usb_mount_point.toUtf8().constData(), &stat) == 0)
    {
        // 使用 f_bavail 而不是 f_bfree，因为 f_bavail 是普通用户实际可用的空间
        // f_bfree 可能包含系统保留的空间，实际用户可能无法使用
        long long free_space = static_cast<long long>(stat.f_bavail) * stat.f_frsize;
        Logger::Log("getUSBSpace | USB Space (available): " + std::to_string(free_space) + " bytes", LogLevel::INFO, DeviceType::MAIN);
        return free_space;
    }
    else
    {
        Logger::Log("getUSBSpace | Failed to obtain the space information of the USB flash drive.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:Failed to obtain the space information of the USB flash drive.");
        return -1;
    }
}

long long MainWindow::getTotalSize(const QStringList &filePaths)
{
    long long totalSize = 0;
    foreach (QString filePath, filePaths)
    {
        QFileInfo fileInfo(filePath);
        if (fileInfo.exists())
        {
            totalSize += fileInfo.size();
        }
    }
    return totalSize;
}

bool MainWindow::isMountReadOnly(const QString &mountPoint)
{
    struct statvfs fsinfo;
    auto mountPointStr = mountPoint.toUtf8().constData();
    Logger::Log("isMountReadOnly | Checking filesystem information for mount point:" + QString::fromUtf8(mountPointStr).toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if (statvfs(mountPointStr, &fsinfo) != 0)
    {
        Logger::Log("isMountReadOnly | Failed to get filesystem information for" + mountPoint.toStdString() + ":" + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(QString("getUSBFail:Failed to get filesystem information for %1, error: %2").arg(mountPoint).arg(strerror(errno)));
        return false;
    }

    Logger::Log("isMountReadOnly | Filesystem flags for" + mountPoint.toStdString() + ":" + std::to_string(fsinfo.f_flag), LogLevel::INFO, DeviceType::MAIN);
    return (fsinfo.f_flag & ST_RDONLY) != 0;
}

bool MainWindow::remountReadWrite(const QString &mountPoint, const QString &password)
{
    QProcess process;
    process.start("sudo", {"-S", "mount", "-o", "remount,rw", mountPoint});
    if (!process.waitForStarted() || !process.write((password + "\n").toUtf8()))
    {
        Logger::Log("remountReadWrite | Failed to execute command: sudo mount", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:Failed to execute command: sudo mount -o remount,rw usb.");
        return false;
    }
    process.closeWriteChannel();
    process.waitForFinished(-1);
    return process.exitCode() == 0;
}

void MainWindow::CopyImagesToUsb(QStringList CopyImgPath, QString usbName)
{
    QString usb_mount_point = "";

    // 如果提供了U盘名，优先使用它从映射表中查找
    if (!usbName.isEmpty() && usbMountPointsMap.contains(usbName))
    {
        usb_mount_point = usbMountPointsMap[usbName];
        Logger::Log("CopyImagesToUsb | Using specified USB from map: " + usbName.toStdString() + " -> " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }

    // 如果上面没有获取到，优先使用 ImageSaveBaseDirectory 指定的U盘路径
    if (usb_mount_point.isEmpty())
    {
        // 使用saveMode判断是否为U盘保存
        bool isUSBSave = (saveMode != "local");

        if (isUSBSave && ImageSaveBaseDirectory.contains("/QUARCS_ImageSave"))
        {
            // 从 ImageSaveBaseDirectory 提取U盘挂载点
            usb_mount_point = ImageSaveBaseDirectory;
            usb_mount_point.replace("/QUARCS_ImageSave", "");

            // 验证该U盘是否仍然存在且有效
            QStorageInfo storageInfo(usb_mount_point);
            if (!storageInfo.isValid() || !storageInfo.isReady())
            {
                Logger::Log("CopyImagesToUsb | Specified USB path is no longer valid: " + usb_mount_point.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                usb_mount_point = ""; // 重置，使用下面的逻辑重新获取
            }
            else
            {
                Logger::Log("CopyImagesToUsb | Using USB from ImageSaveBaseDirectory: " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            }
        }
    }

    // 如果上面没有获取到，尝试从映射表获取
    if (usb_mount_point.isEmpty())
    {
        if (usbMountPointsMap.size() == 1)
        {
            // 单个U盘，直接使用
            usb_mount_point = usbMountPointsMap.first();
            Logger::Log("CopyImagesToUsb | Using single USB from map: " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        else if (usbMountPointsMap.size() > 1)
        {
            // 多个U盘，如果 ImageSaveBaseDirectory 是U盘路径但提取失败，或者没有指定，需要用户选择
            emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Multiple");
            Logger::Log("CopyImagesToUsb | Multiple USB drives detected, please specify which one to use.", LogLevel::WARNING, DeviceType::MAIN);
            return;
        }
        else
        {
            // 映射表为空，尝试使用统一的U盘挂载点获取函数（作为后备）
            if (!getUSBMountPoint(usb_mount_point))
            {
                // 获取U盘名称用于错误消息
                QString base = "/media/";
                QString username = QDir::home().dirName();
                QString basePath = base + username;
                QDir baseDir(basePath);

                if (!baseDir.exists())
                {
                    emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Null");
                    Logger::Log("CopyImagesToUsb | Base directory does not exist.", LogLevel::WARNING, DeviceType::MAIN);
                }
                else
                {
                    QStringList filters;
                    filters << "*";
                    QStringList folderList = baseDir.entryList(filters, QDir::Dirs | QDir::NoDotAndDotDot);
                    folderList.removeAll("CDROM");

                    if (folderList.size() == 0)
                    {
                        emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Null");
                        Logger::Log("CopyImagesToUsb | No USB drive found.", LogLevel::WARNING, DeviceType::MAIN);
                    }
                    else
                    {
                        emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Multiple");
                        Logger::Log("CopyImagesToUsb | Multiple USB drives detected.", LogLevel::WARNING, DeviceType::MAIN);
                    }
                }
                return;
            }
        }
    }

    const QString password = "quarcs"; // sudo 密码

    QStorageInfo storageInfo(usb_mount_point);
    if (storageInfo.isValid() && storageInfo.isReady())
    {
        if (storageInfo.isReadOnly())
        {
            // 处理1: 该路径为只读设备
            if (!remountReadWrite(usb_mount_point, password))
            {
                Logger::Log("CopyImagesToUsb | Failed to remount filesystem as read-write.", LogLevel::WARNING, DeviceType::MAIN);
                return;
            }
            Logger::Log("CopyImagesToUsb | Filesystem remounted as read-write successfully.", LogLevel::INFO, DeviceType::MAIN);
        }
        Logger::Log("CopyImagesToUsb | This path is for writable devices.", LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("CopyImagesToUsb | The specified path is not a valid file system or is not ready.", LogLevel::WARNING, DeviceType::MAIN);
    }
    // 先统计需要移动的所有文件的总大小
    long long totalSize = getTotalSize(CopyImgPath);
    if (totalSize <= 0)
    {
        Logger::Log("CopyImagesToUsb | No valid files to move or total size is 0.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:No valid files to move!");
        return;
    }

    // 检查U盘剩余空间
    long long remaining_space = getUSBSpace(usb_mount_point);
    if (remaining_space == -1 || remaining_space <= 0)
    {
        Logger::Log("CopyImagesToUsb | USB drive has no available space or is not accessible.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:USB drive has no available space!");
        return;
    }

    // 检查空间是否足够（总文件大小必须小于剩余空间）
    if (totalSize > remaining_space)
    {
        Logger::Log("CopyImagesToUsb | Insufficient storage space. Required: " + QString::number(totalSize).toStdString() +
                   " bytes, Available: " + QString::number(remaining_space).toStdString() + " bytes", LogLevel::WARNING, DeviceType::MAIN);
        QString errorMsg = QString("Not enough storage space! Required: %1 MB, Available: %2 MB")
                          .arg(QString::number(totalSize / (1024.0 * 1024.0), 'f', 2))
                          .arg(QString::number(remaining_space / (1024.0 * 1024.0), 'f', 2));
        emit wsThread->sendMessageToClient("getUSBFail:" + errorMsg);
        return;
    }
    QString folderName = "QUARCS_ImageSave";
    QString folderPath = usb_mount_point + "/" + folderName;
    QString basePath = QString::fromStdString(ImageSaveBasePath).trimmed();
    if (basePath.endsWith('/'))
        basePath.chop(1);

    int sumMoveImage = 0;
    for (const auto &imgPath : CopyImgPath)
    {
        if (!imgPath.startsWith(basePath))
        {
            Logger::Log("CopyImagesToUsb | path is error! (not under ImageSaveBasePath): " + imgPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }
        QString relativePath = imgPath.mid(basePath.length()).trimmed();
        if (relativePath.startsWith('/'))
            relativePath = relativePath.mid(1);
        int lastSlash = relativePath.lastIndexOf('/');
        if (lastSlash == -1)
        {
            Logger::Log("CopyImagesToUsb | path is error! (no directory part): " + imgPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }
        QString relativeDir = relativePath.left(lastSlash + 1);
        QString destinationPath = folderPath + "/" + relativeDir;

        // 安全检查：避免在 /media/quarcs 路径下创建任何文件夹，避免被错误识别为U盘
        QString normalizedDestPath = QDir(destinationPath).absolutePath();
        if (normalizedDestPath.startsWith("/media/quarcs/"))
        {
            // 提取 /media/quarcs/ 之后的部分
            QString pathAfterMedia = normalizedDestPath.mid(14); // 去掉 "/media/quarcs/"

            // 检查路径格式：应该是 /media/quarcs/某个U盘名/...
            int firstSlash = pathAfterMedia.indexOf('/');
            if (firstSlash > 0)
            {
                QString usbName = pathAfterMedia.left(firstSlash);
                // 检查这个U盘名是否在映射表中（有效的U盘挂载点）
                if (!usbMountPointsMap.contains(usbName))
                {
                    Logger::Log("CopyImagesToUsb | Security check failed: Attempting to create directory in /media/quarcs/ but USB name '" + usbName.toStdString() + "' not found in mount points map. Path: " + destinationPath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
                    continue;
                }
                // 验证路径确实在U盘挂载点下
                QString expectedMountPoint = "/media/quarcs/" + usbName;
                if (!normalizedDestPath.startsWith(expectedMountPoint))
                {
                    Logger::Log("CopyImagesToUsb | Security check failed: Path does not match expected mount point. Path: " + destinationPath.toStdString() + ", Expected mount point: " + expectedMountPoint.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
                    continue;
                }
            }
            else
            {
                // 路径格式不正确，可能是直接在 /media/quarcs/ 下创建文件夹
                Logger::Log("CopyImagesToUsb | Security check failed: Invalid path format in /media/quarcs/. Path: " + destinationPath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
                continue;
            }
        }
        // 额外检查：确保路径不是直接在 /media/quarcs 下（没有子目录）
        else if (normalizedDestPath == "/media/quarcs")
        {
            Logger::Log("CopyImagesToUsb | Security check failed: Attempting to create directory directly at /media/quarcs. Path: " + destinationPath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }

        QProcess process;
        process.start("sudo", {"-S", "mkdir", "-p", destinationPath});
        if (!process.waitForStarted() || !process.write((password + "\n").toUtf8()))
        {
            Logger::Log("CopyImagesToUsb | Failed to execute command: sudo mkdir.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        process.closeWriteChannel();
        process.waitForFinished(-1);

        process.start("sudo", {"-S", "cp", "-r", imgPath, destinationPath});
        if (!process.waitForStarted() || !process.write((password + "\n").toUtf8()))
        {
            Logger::Log("CopyImagesToUsb | Failed to execute command: sudo cp.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        process.closeWriteChannel();
        process.waitForFinished(-1);

        // Read the standard error output
        QByteArray stderrOutput = process.readAllStandardError();

        if (process.exitCode() == 0)
        {
            Logger::Log("CopyImagesToUsb | Copied file: " + imgPath.toStdString() + " to " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("CopyImagesToUsb | Failed to copy file: " + imgPath.toStdString() + " to " + destinationPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            // Print the error reason
            Logger::Log("CopyImagesToUsb | Error: " + QString::fromUtf8(stderrOutput).toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        sumMoveImage++;
        emit wsThread->sendMessageToClient("HasMoveImgnNUmber:succeed:" + QString::number(sumMoveImage));
    }
}

void MainWindow::USBCheck()
{
    // 清空之前的U盘映射表
    usbMountPointsMap.clear();

    QString base = "/media/";
    QString username = QDir::home().dirName();
    QString basePath = base + username;
    QDir baseDir(basePath);

    if (!baseDir.exists())
    {
        Logger::Log("USBCheck | Base directory does not exist.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("USBCheck:Null, Null");
        return;
    }

    // 获取所有文件夹，排除"."和".."，并且排除"CDROM"
    QStringList filters;
    filters << "*";
    QStringList folderList = baseDir.entryList(filters, QDir::Dirs | QDir::NoDotAndDotDot);
    folderList.removeAll("CDROM");

    if (folderList.size() == 0)
    {
        emit wsThread->sendMessageToClient("USBCheck:Null, Null");
        Logger::Log("USBCheck | No USB drive found.", LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    // 遍历所有U盘，验证并存储到映射表
    QStringList validUsbList;
    for (const QString &folderName : folderList)
    {
        QString usb_mount_point = basePath + "/" + folderName;
        QStorageInfo storageInfo(usb_mount_point);

        // 验证这是否是一个真正挂载的存储设备
        if (storageInfo.isValid() && storageInfo.isReady())
        {
            // 存储U盘信息：U盘名 -> U盘路径
            usbMountPointsMap[folderName] = usb_mount_point;
            validUsbList.append(folderName);

            Logger::Log("USBCheck | Found USB: " + folderName.toStdString() + " -> " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
    }

    if (validUsbList.size() == 0)
    {
        emit wsThread->sendMessageToClient("USBCheck:Null, Null");
        Logger::Log("USBCheck | No valid USB drive found.", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    else if (validUsbList.size() == 1)
    {
        // 单个U盘：发送U盘名和剩余空间
        QString usbName = validUsbList.at(0);
        QString usb_mount_point = usbMountPointsMap[usbName];
        long long remaining_space = getUSBSpace(usb_mount_point);
        if (remaining_space == -1)
        {
            Logger::Log("USBCheck | Check whether a USB flash drive or portable hard drive is inserted!", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("USBCheck:Null, Null");
            return;
        }
        QString message = "USBCheck:" + usbName + "," + QString::number(remaining_space);
        Logger::Log("USBCheck | " + message.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(message);
    }
    else
    {
        // 多个U盘：发送所有U盘信息
        QString message = "USBCheck:Multiple";
        QStringList usbInfoList;
        for (const QString &usbName : validUsbList)
        {
            QString usb_mount_point = usbMountPointsMap[usbName];
            long long remaining_space = getUSBSpace(usb_mount_point);
            if (remaining_space != -1)
            {
                usbInfoList.append(usbName + "," + QString::number(remaining_space));
            }
        }
        if (usbInfoList.size() > 0)
        {
            message = message + ":" + usbInfoList.join(":");
        }
        Logger::Log("USBCheck | Multiple USB drives: " + message.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(message);
    }
}

bool MainWindow::getUSBMountPoint(QString &usb_mount_point)
{
    QString base = "/media/";
    QString username = QDir::home().dirName();
    QString basePath = base + username;
    QDir baseDir(basePath);

    if (!baseDir.exists())
    {
        Logger::Log("getUSBMountPoint | Base directory does not exist.", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }

    // 获取所有文件夹，排除"."和".."，并且排除"CDROM"
    QStringList filters;
    filters << "*";
    QStringList folderList = baseDir.entryList(filters, QDir::Dirs | QDir::NoDotAndDotDot);
    folderList.removeAll("CDROM");

    // 检查剩余文件夹数量是否为1
    if (folderList.size() == 1)
    {
        usb_mount_point = basePath + "/" + folderList.at(0);

        // 验证这是否是一个真正挂载的存储设备
        QStorageInfo storageInfo(usb_mount_point);
        if (!storageInfo.isValid() || !storageInfo.isReady())
        {
            Logger::Log("getUSBMountPoint | The directory exists but is not a valid mounted storage device.", LogLevel::WARNING, DeviceType::MAIN);
            return false;
        }

        Logger::Log("getUSBMountPoint | USB mount point:" + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
    else if (folderList.size() == 0)
    {
        Logger::Log("getUSBMountPoint | No USB drive found.", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    else
    {
        Logger::Log("getUSBMountPoint | Multiple USB drives detected.", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
}

void MainWindow::GetUSBFiles(const QString &usbName, const QString &relativePath)
{
    Logger::Log("GetUSBFiles start ...", LogLevel::INFO, DeviceType::MAIN);

    // 必须传入U盘名
    if (usbName.isEmpty())
    {
        Logger::Log("GetUSBFiles | USB name is required.", LogLevel::WARNING, DeviceType::MAIN);
        QJsonObject errorObj;
        errorObj["error"] = "USB name is required";
        errorObj["path"] = "";
        errorObj["files"] = QJsonArray();
        QJsonDocument errorDoc(errorObj);
        emit wsThread->sendMessageToClient("USBFilesList:" + errorDoc.toJson(QJsonDocument::Compact));
        return;
    }

    // 根据U盘名从映射表获取挂载点路径
    if (!usbMountPointsMap.contains(usbName))
    {
        Logger::Log("GetUSBFiles | Specified USB name not found: " + usbName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        QJsonObject errorObj;
        errorObj["error"] = QString("USB drive not found: %1").arg(usbName);
        errorObj["path"] = "";
        errorObj["files"] = QJsonArray();
        QJsonDocument errorDoc(errorObj);
        emit wsThread->sendMessageToClient("USBFilesList:" + errorDoc.toJson(QJsonDocument::Compact));
        return;
    }

    QString usb_mount_point = usbMountPointsMap[usbName];
    Logger::Log("GetUSBFiles | Using USB: " + usbName.toStdString() + " -> " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // 构建完整路径
    QString fullPath = usb_mount_point;

    // 清理路径，防止路径遍历攻击
    QString cleanPath = relativePath;
    cleanPath.replace("..", ""); // 移除路径遍历
    cleanPath.replace("//", "/"); // 移除双斜杠
    if (cleanPath.startsWith("/"))
    {
        cleanPath = cleanPath.mid(1); // 移除开头的斜杠
    }
    if (!cleanPath.isEmpty())
    {
        fullPath = usb_mount_point + "/" + cleanPath;
    }

    // 验证目录是否存在
    QDir targetDir(fullPath);
    if (!targetDir.exists())
    {
        Logger::Log("GetUSBFiles | Target directory does not exist: " + fullPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        QJsonObject errorObj;
        errorObj["error"] = "Directory not found";
        errorObj["path"] = "/" + relativePath;
        errorObj["files"] = QJsonArray();
        QJsonDocument errorDoc(errorObj);
        emit wsThread->sendMessageToClient("USBFilesList:" + errorDoc.toJson(QJsonDocument::Compact));
        return;
    }

    // 获取文件列表
    QJsonArray filesArray;
    QFileInfoList entries = targetDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name | QDir::DirsFirst);

    for (const QFileInfo &entry : entries)
    {
        QJsonObject fileObj;
        fileObj["name"] = entry.fileName();
        fileObj["isDirectory"] = entry.isDir();
        if (!entry.isDir())
        {
            fileObj["size"] = static_cast<qint64>(entry.size());
        }
        filesArray.append(fileObj);
    }

    // 构建返回结果
    QJsonObject result;
    QString displayPath = relativePath.isEmpty() ? "/" : ("/" + relativePath);
    result["path"] = displayPath;
    result["files"] = filesArray;

    QJsonDocument doc(result);
    QString jsonString = doc.toJson(QJsonDocument::Compact);

    Logger::Log("GetUSBFiles | Found " + QString::number(filesArray.size()).toStdString() + " items in " + fullPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient("USBFilesList:" + jsonString);
    Logger::Log("GetUSBFiles finish!", LogLevel::INFO, DeviceType::MAIN);
}
