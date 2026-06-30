#include "mainwindow.h"

void MainWindow::initializeStorageAndWebPaths()
{
    // 默认：系统家目录下 ~/images；可通过环境变量 QUARCS_IMAGE_SAVE_ROOT 覆盖
    {
        QString base = QDir::cleanPath(QDir::homePath() + "/images");
        if (const char *env = std::getenv("QUARCS_IMAGE_SAVE_ROOT"))
        {
            const std::string v(env);
            if (!v.empty())
            {
                base = QDir::cleanPath(QString::fromStdString(v));
                Logger::Log("MainWindow | QUARCS_IMAGE_SAVE_ROOT=" + v, LogLevel::INFO, DeviceType::MAIN);
            }
        }
        ImageSaveBasePath = base.toStdString();
        ImageSaveBaseDirectory = base;
        Logger::Log("MainWindow | ImageSaveBasePath=" + ImageSaveBasePath, LogLevel::INFO, DeviceType::MAIN);
    }

    if (const char *env = std::getenv("QUARCS_WEB_IMG_ROOT"))
    {
        const std::string v(env);
        if (!v.empty())
        {
            vueImagePath = v;
            Logger::Log("MainWindow | QUARCS_WEB_IMG_ROOT=" + vueImagePath, LogLevel::INFO, DeviceType::MAIN);
        }
    }
    if (!vueImagePath.empty() && vueImagePath.back() != '/')
        vueImagePath.push_back('/');

    {
        const QString imgRoot = QString::fromStdString(vueImagePath);
        QString target = QString::fromStdString(tilePyramidPath);
        if (!target.endsWith('/')) target += '/';
        const QString linkPath = imgRoot + QStringLiteral("capture-tiles");

        if (!target.isEmpty()) {
            QDir tdir(target);
            if (!tdir.exists()) {
                if (QDir().mkpath(target)) {
                    Logger::Log("Tile tiles root created: " + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                } else {
                    Logger::Log("Failed to create tile tiles root: " + target.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }

        if (!imgRoot.isEmpty() && !target.isEmpty()) {
            QFileInfo fi(linkPath);
            if (fi.exists() || fi.isSymLink()) {
                if (fi.isSymLink()) {
                    const QString cur = fi.symLinkTarget();
                    QString curNorm = cur;
                    if (!curNorm.endsWith('/')) curNorm += '/';
                    if (curNorm == target) {
                        Logger::Log("Tile mapping OK: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                    LogLevel::INFO, DeviceType::MAIN);
                    } else {
                        const int rc = QProcess::execute(QStringLiteral("ln"),
                                                         QStringList() << QStringLiteral("-sfn") << target << linkPath);
                        if (rc == 0) {
                            Logger::Log("Tile mapping fixed: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                        LogLevel::INFO, DeviceType::MAIN);
                        } else {
                            Logger::Log("Tile mapping exists but points elsewhere: " + linkPath.toStdString() +
                                            " -> " + cur.toStdString() + " (expected " + target.toStdString() + "), rc=" + std::to_string(rc),
                                        LogLevel::WARNING, DeviceType::MAIN);
                        }
                    }
                } else {
                    Logger::Log("Tile mapping not created because path already exists (not symlink): " + linkPath.toStdString() +
                                    " . Consider removing it or configure nginx alias: /img/capture-tiles/ -> " + target.toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            } else {
                QDir().mkpath(QFileInfo(linkPath).absolutePath());
                const int rc = QProcess::execute(QStringLiteral("ln"),
                                                 QStringList() << QStringLiteral("-sfn") << target << linkPath);
                if (rc == 0) {
                    Logger::Log("Tile mapping created: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                LogLevel::INFO, DeviceType::MAIN);
                } else {
                    Logger::Log("Failed to create tile mapping: " + linkPath.toStdString() + " -> " + target.toStdString() +
                                    ", rc=" + std::to_string(rc) + ". Permission issue? (need write access to " + imgRoot.toStdString() + ")",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }
    }

    {
        const QString imgRoot = QString::fromStdString(vueImagePath);
        QString base = QString::fromStdString(ImageSaveBasePath);
        if (!QDir::isAbsolutePath(base))
            base = QDir::cleanPath(QDir::current().absoluteFilePath(base));
        QString target = QDir::cleanPath(base + "/downloads/");
        if (const char *env = std::getenv("QUARCS_DOWNLOADS_ROOT"))
        {
            const std::string v(env);
            if (!v.empty())
            {
                target = QString::fromStdString(v);
                Logger::Log("MainWindow | QUARCS_DOWNLOADS_ROOT=" + v, LogLevel::INFO, DeviceType::MAIN);
            }
        }
        if (!target.endsWith('/')) target += '/';
        const QString linkPath = imgRoot + QStringLiteral("downloads");

        if (!target.isEmpty())
        {
            QDir tdir(target);
            if (!tdir.exists())
            {
                if (QDir().mkpath(target))
                {
                    Logger::Log("Downloads root created: " + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("Failed to create downloads root: " + target.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }

        if (!imgRoot.isEmpty() && !target.isEmpty())
        {
            QFileInfo fi(linkPath);
            if (fi.exists() || fi.isSymLink())
            {
                if (fi.isSymLink())
                {
                    const QString cur = fi.symLinkTarget();
                    QString curNorm = cur;
                    if (!curNorm.endsWith('/')) curNorm += '/';
                    if (curNorm == target)
                    {
                        Logger::Log("Downloads mapping OK: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                    LogLevel::INFO, DeviceType::MAIN);
                    }
                    else
                    {
                        const int rc = QProcess::execute(QStringLiteral("ln"),
                                                         QStringList() << QStringLiteral("-sfn") << target << linkPath);
                        if (rc == 0)
                        {
                            Logger::Log("Downloads mapping fixed: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                        LogLevel::INFO, DeviceType::MAIN);
                        }
                        else
                        {
                            Logger::Log("Downloads mapping exists but points elsewhere: " + linkPath.toStdString() +
                                            " -> " + cur.toStdString() + " (expected " + target.toStdString() + "), rc=" + std::to_string(rc),
                                        LogLevel::WARNING, DeviceType::MAIN);
                        }
                    }
                }
                else
                {
                    Logger::Log("Downloads mapping not created because path already exists (not symlink): " + linkPath.toStdString() +
                                    " . Consider removing it or configure web server to serve it.",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
            else
            {
                QDir().mkpath(QFileInfo(linkPath).absolutePath());
                const int rc = QProcess::execute(QStringLiteral("ln"),
                                                 QStringList() << QStringLiteral("-sfn") << target << linkPath);
                if (rc == 0)
                {
                    Logger::Log("Downloads mapping created: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("Failed to create downloads mapping: " + linkPath.toStdString() + " -> " + target.toStdString() +
                                    ", rc=" + std::to_string(rc) + ". Permission issue? (need write access to " + imgRoot.toStdString() + ")",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }
    }
}

void MainWindow::initializeWebSocketBridge()
{
    wsThread = new WebSocketThread(websockethttpUrl, websockethttpsUrl);
    connect(wsThread, &WebSocketThread::receivedMessage, this, &MainWindow::onMessageReceived);
    wsThread->start();
    Logger::wsThread = wsThread;
    instance = this;
}
