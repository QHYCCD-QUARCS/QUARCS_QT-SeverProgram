#include "mainwindow.h"

#if 0
// ============================================================================
// PHD2 已移除（2026-01）：原 PHD2 进程/共享内存/导星控制逻辑全部下线。
// 保留代码仅作历史参考，不参与编译与运行。
// ============================================================================

void MainWindow::InitPHD2()
{
    Logger::Log("InitPHD2 start ...", LogLevel::INFO, DeviceType::MAIN);
    isGuideCapture = true;

    if (!cmdPHD2) cmdPHD2 = new QProcess();
    static bool phdSignalsConnected = false;
    if (!phdSignalsConnected)
    {
        connect(cmdPHD2, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &MainWindow::onPhd2Exited);
        connect(cmdPHD2, &QProcess::errorOccurred,
                this, &MainWindow::onPhd2Error);
        phdSignalsConnected = true;
    }

    bool connected = false;
    int retryCount = 1; // 设定重试次数
    while (retryCount > 0 && !connected)
    {
        // 启动前强制结束残留进程并清理共享内存
        // 以避免“初次启动时无法关闭导致启动失败”的问题
        // 1) 若之前有 QProcess 实例，先尝试优雅结束并回收，避免僵尸进程
        if (cmdPHD2->state() != QProcess::NotRunning) {
            cmdPHD2->terminate();
            if (!cmdPHD2->waitForFinished(1500)) {
                cmdPHD2->kill();
                cmdPHD2->waitForFinished(1000);
            }
        }
        // 2) 系统级强杀（多种匹配方式）
        // 注意：有的系统进程名是 phd2.bin，这里同时匹配 phd2 和 phd2.bin
        QProcess::execute("pkill", QStringList() << "-TERM" << "-x" << "phd2");
        QProcess::execute("pkill", QStringList() << "-TERM" << "-x" << "phd2.bin");
        QThread::msleep(200);
        QProcess::execute("pkill", QStringList() << "-KILL" << "-x" << "phd2");
        QProcess::execute("pkill", QStringList() << "-KILL" << "-x" << "phd2.bin");
        QThread::msleep(100);
        // 3) 宽匹配（包含路径/命令行）
        QProcess::execute("pkill", QStringList() << "-TERM" << "-f" << "phd2");
        QThread::msleep(150);
        QProcess::execute("pkill", QStringList() << "-KILL" << "-f" << "phd2");
        QThread::msleep(150);
        // 4) 轮询确认已无残留进程（最多 1s）
        {
            QElapsedTimer waitKill;
            waitKill.start();
            while (waitKill.elapsed() < 1000) {
                int rc = QProcess::execute("pgrep", QStringList() << "-f" << "phd2");
                if (rc != 0) break; // 无匹配
                QThread::msleep(100);
            }
        }

        // 5) 启动前清空 PHD2 日志目录，以规避“损坏/异常 GuidingLog 导致启动卡死”的问题
        //    目标目录：/home/quarcs/Documents/PHD2
        {
            const QString phd2LogDirPath = QStringLiteral("/home/quarcs/Documents/PHD2");
            QDir phd2LogDir(phd2LogDirPath);
            if (phd2LogDir.exists())
            {
                // 递归删除整个日志目录及其内容，然后重新创建一个空目录
                if (!phd2LogDir.removeRecursively())
                {
                    Logger::Log("InitPHD2 | failed to clear PHD2 log dir: " + phd2LogDirPath.toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
            // 确保目录最终存在（即使之前不存在或被删除）
            if (!phd2LogDir.mkpath("."))
            {
                Logger::Log("InitPHD2 | failed to recreate PHD2 log dir: " + phd2LogDirPath.toStdString(),
                            LogLevel::WARNING, DeviceType::MAIN);
            }
            else
            {
                Logger::Log("InitPHD2 | PHD2 log dir cleared: " + phd2LogDirPath.toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }
        // 清理共享内存段（key=0x90）
        key_t cleanup_key = 0x90;
        int cleanup_id = shmget(cleanup_key, BUFSZ_PHD, 0666);
        if (cleanup_id != -1) shmctl(cleanup_id, IPC_RMID, NULL);

        // 重新生成共享内存，避免之前的进程遗留问题
        key_phd = 0x90;                                           // 重新设置共享内存的键值
        shmid_phd = shmget(key_phd, BUFSZ_PHD, IPC_CREAT | 0666); // 获取共享内存
        if (shmid_phd < 0)
        {
            Logger::Log("InitPHD2 | shared memory phd shmget ERROR", LogLevel::ERROR, DeviceType::MAIN);
            continue;
        }

        // 映射共享内存
        sharedmemory_phd = (char *)shmat(shmid_phd, NULL, 0);
        if (sharedmemory_phd == NULL)
        {
            Logger::Log("InitPHD2 | shared memory phd map ERROR", LogLevel::ERROR, DeviceType::MAIN);
            continue;
        }

        // 读取共享内存数据（避免将共享内存当作以\\0 结尾字符串打印，可能越界）
        Logger::Log("InitPHD2 | shared memory mapped", LogLevel::INFO, DeviceType::MAIN);

        // 启动 phd2 进程（显式指定实例号 1）
        cmdPHD2->start("phd2", QStringList() << "-i" << "1");
        phd2ExpectedRunning = true;

        // 等待最多 10 秒尝试连接
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 2000)
        {
            usleep(1000);
            qApp->processEvents();
            if (connectPHD() == true)
            {
                connected = true;
                break;
            }
        }

        if (!connected)
        {
            Logger::Log("InitPHD2 | Failed to connect to phd2. Retrying...", LogLevel::WARNING, DeviceType::MAIN);
            retryCount--; // 如果连接失败，重试次数减 1
        }
    }

    if (!connected)
    {
        Logger::Log("InitPHD2 | Failed to connect to phd2 after retries.", LogLevel::ERROR, DeviceType::MAIN);
    }

    Logger::Log("InitPHD2 finished.", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::onPhd2Exited(int exitCode, QProcess::ExitStatus exitStatus)
{
    Logger::Log("PHD2 exited. code=" + std::to_string(exitCode) +
                " status=" + std::to_string((int)exitStatus), LogLevel::WARNING, DeviceType::GUIDER);
    if (phd2ExpectedRunning)
    {
        // 进程异常结束时，尝试发送一次“停止循环拍摄”命令以收敛前端状态
        call_phd_StopLooping();

        // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
        // emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        phd2ExpectedRunning = false;
        // 清理共享内存段，避免前端继续读到旧数据
        key_t key = 0x90;
        int id = shmget(key, BUFSZ_PHD, 0666);
        if (id != -1) shmctl(id, IPC_RMID, NULL);
        // TODO(PHD2): 提示前端是否重启（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2ClosedUnexpectedly:是否重新启动PHD2?");
    }
}

void MainWindow::onPhd2Error(QProcess::ProcessError error)
{
    Logger::Log("PHD2 process error: " + std::to_string((int)error), LogLevel::ERROR, DeviceType::GUIDER);
    if (phd2ExpectedRunning)
    {
        // 进程错误时，尝试发送“停止循环拍摄”命令以确保状态一致
        call_phd_StopLooping();
        // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
        // emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        phd2ExpectedRunning = false;
        key_t key = 0x90;
        int id = shmget(key, BUFSZ_PHD, 0666);
        if (id != -1) shmctl(id, IPC_RMID, NULL);
        // TODO(PHD2): 提示前端是否重启（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2ClosedUnexpectedly:是否重新启动PHD2?");
    }
}
bool MainWindow::connectPHD(void)
{
    Logger::Log("connectPHD start ...", LogLevel::INFO, DeviceType::MAIN);
    QString versionName = "";
    call_phd_GetVersion(versionName);

    Logger::Log("connectPHD | version:" + versionName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    if (versionName != "")
    {
        // init stellarium operation
        Logger::Log("connectPHD Success!", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
    else
    {
        Logger::Log("connectPHD | there is no openPHD2 running", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("connectPHD failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
}

void MainWindow::disconnectFocuserIfConnected()
{
    if (dpFocuser && dpFocuser->isConnected())
    {
        DisconnectDevice(indi_Client, dpFocuser->getDeviceName(), "Focuser");
    }
    else if (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[DeviceSlot::Focuser].isSDKConnect &&
             sdkFocuserHandle != nullptr)
    {
        SdkManager::instance().closeByHandle(sdkFocuserHandle);
        sdkFocuserHandle = nullptr;
        sdkFocuserPort.clear();
        systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect = false;
        systemdevicelist.system_devices[DeviceSlot::Focuser].isBind = false;
    }
}

bool MainWindow::call_phd_GetVersion(QString &versionName)
{
    Logger::Log("call_phd_GetVersion start ...", LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_GetVersion | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        versionName = "";
        return false;
    }

    unsigned int baseAddress;
    unsigned int vendcommand;
    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x01;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    // 放宽首次连接等待时长，避免 PHD2 在树莓派等设备上启动/初始化较慢导致的超时
    // 最长等待 10 秒，让 PHD2 有充分时间写回版本信息
    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 2000)
    {
        QThread::msleep(2);
    }

    // 如果超过 10 秒仍未收到响应，则认为超时
    if (t.elapsed() >= 2000)
    {
        versionName = "";
        Logger::Log("call_phd_GetVersion | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_GetVersion failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    else
    {
        unsigned char addr = 0;
        uint16_t length;
        memcpy(&length, sharedmemory_phd + baseAddress + addr, sizeof(uint16_t));
        addr = addr + sizeof(uint16_t);
        // qDebug()<<length;

        if (length > 0 && length < 1024)
        {
            for (int i = 0; i < length; i++)
            {
                versionName.append(sharedmemory_phd[baseAddress + addr + i]);
            }
            Logger::Log("call_phd_GetVersion | version:" + versionName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            Logger::Log("call_phd_GetVersion success.", LogLevel::INFO, DeviceType::MAIN);
            return true;
            // qDebug()<<versionName;
        }
        else
        {
            versionName = "";
            Logger::Log("call_phd_GetVersion | version is empty", LogLevel::ERROR, DeviceType::MAIN);
            Logger::Log("call_phd_GetVersion failed.", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
    }
}

uint32_t MainWindow::call_phd_StartLooping(void)
{
    Logger::Log("call_phd_StartLooping start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StartLooping | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x03;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // 避免忙等占满 CPU，增加适度休眠
        QThread::msleep(100);
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StartLooping | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StartLooping failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StartLooping success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StopLooping(void)
{
    Logger::Log("call_phd_StopLooping start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StopLooping | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x04;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // 避免忙等占满 CPU，增加适度休眠
        QThread::msleep(100);
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StopLooping | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StopLooping failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StopLooping success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_AutoFindStar(void)
{
    Logger::Log("call_phd_AutoFindStar start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_AutoFindStar | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x05;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_AutoFindStar | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_AutoFindStar failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_AutoFindStar success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StartGuiding(void)
{
    Logger::Log("call_phd_StartGuiding start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StartGuiding | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x06;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StartGuiding | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StartGuiding failed.", LogLevel::ERROR, DeviceType::GUIDER);
        // 在启动导星失败时，发送关闭循环拍摄的命令
        // call_phd_StopLooping();
        // 通知前端状态：循环曝光已关闭
        // emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        isGuiding = false;
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StartGuiding success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StopGuiding(void)
{
    Logger::Log("call_phd_StopGuiding start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StopGuiding | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x17; // 与 PHD2 端 myframe.cpp 中定义的“Stop Guiding Only”命令码保持一致

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StopGuiding | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StopGuiding failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StopGuiding success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

void MainWindow::pauseGuidingBeforeMountMove()
{
    // 仅在当前逻辑导星开关为 ON 时，才在移动前主动停止导星，并记录状态以便之后恢复
    wasGuidingBeforeMountMove = false;

    if (isGuiding)
    {
        Logger::Log("pauseGuidingBeforeMountMove | guiding is ON, stop guiding before mount move.",
                    LogLevel::INFO, DeviceType::GUIDER);
        wasGuidingBeforeMountMove = true;
        // 仅停止导星，不关闭循环曝光，避免影响 PHD2 的 Loop 按钮语义
        call_phd_StopGuiding();
        // 这里不去强制修改 isGuiding / 循环开关，由 PHD2 端自身状态机与前端 UI 控制
    }
}

void MainWindow::resumeGuidingAfterMountMove()
{
    if (!wasGuidingBeforeMountMove)
    {
        // 移动前没有在导星，无需恢复
        return;
    }

    Logger::Log("resumeGuidingAfterMountMove | mount move finished, resume guiding.",
                LogLevel::INFO, DeviceType::GUIDER);

    // 参考 GuiderSwitch=true 的逻辑：如需清校准则先清，再根据是否已选星决定是否自动寻星，最后启动导星
    if (ClearCalibrationData)
    {
        ClearCalibrationData = false;
        call_phd_ClearCalibration();
        Logger::Log("resumeGuidingAfterMountMove | clear calibration data before restart guiding.",
                    LogLevel::INFO, DeviceType::GUIDER);
    }

    if (!glPHD_isSelected)
    {
        Logger::Log("resumeGuidingAfterMountMove | no selected star, call AutoFindStar before guiding.",
                    LogLevel::INFO, DeviceType::GUIDER);
        call_phd_AutoFindStar();
    }

    call_phd_StartGuiding();
    // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
    // emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
}

uint32_t MainWindow::call_phd_checkStatus(unsigned char &status)
{
    Logger::Log("call_phd_checkStatus start ...", LogLevel::DEBUG, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_checkStatus | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        status = 0;
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x07;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();
    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        // timeout
        status = 0;
        Logger::Log("call_phd_checkStatus | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_checkStatus failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    else
    {
        status = sharedmemory_phd[3];
        Logger::Log("call_phd_checkStatus | status:" + std::to_string(status), LogLevel::DEBUG, DeviceType::GUIDER);
        Logger::Log("call_phd_checkStatus success.", LogLevel::DEBUG, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_setExposureTime(unsigned int expTime)
{
    Logger::Log("call_phd_setExposureTime start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_setExposureTime | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;
    Logger::Log("call_phd_setExposureTime | expTime:" + std::to_string(expTime), LogLevel::INFO, DeviceType::GUIDER);

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0b;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &expTime, sizeof(unsigned int));
    addr = addr + sizeof(unsigned int);

    sharedmemory_phd[0] = 0x01; // enable command

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_setExposureTime | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_setExposureTime failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return QHYCCD_ERROR; // timeout
    }
    else
    {
        Logger::Log("call_phd_setExposureTime success.", LogLevel::INFO, DeviceType::GUIDER);
        return QHYCCD_SUCCESS;
    }
}

uint32_t MainWindow::call_phd_whichCamera(std::string Camera)
{
    Logger::Log("call_phd_whichCamera start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_whichCamera | Camera:" + Camera, LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_whichCamera | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return QHYCCD_ERROR;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0d;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    int length = Camera.length() + 1;

    unsigned char addr = 0;
    // memcpy(sharedmemory_phd + baseAddress + addr, &index, sizeof(int));
    // addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, &length, sizeof(int));
    addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, Camera.c_str(), length);
    addr = addr + length;

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_whichCamera | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_whichCamera failed.", LogLevel::ERROR, DeviceType::MAIN);
        return QHYCCD_ERROR; // timeout
    }
    else
    {
        Logger::Log("call_phd_whichCamera success.", LogLevel::INFO, DeviceType::MAIN);
        return QHYCCD_SUCCESS;
    }
}

uint32_t MainWindow::call_phd_ChackControlStatus(int sdk_num)
{
    Logger::Log("call_phd_ChackControlStatus start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_ChackControlStatus | sdk_num:" + std::to_string(sdk_num), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_ChackControlStatus | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0e;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &sdk_num, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_ChackControlStatus | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_ChackControlStatus failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_ChackControlStatus success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_ClearCalibration(void)
{
    Logger::Log("call_phd_ClearCalibration start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_ClearCalibration | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x02;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_ClearCalibration | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_ClearCalibration failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_ClearCalibration success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}
uint32_t MainWindow::call_phd_StarClick(int x, int y)
{
    Logger::Log("call_phd_StarClick start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_StarClick | x:" + std::to_string(x) + ", y:" + std::to_string(y), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        // 说明：当前工程已默认使用内置导星（GuiderCore），模拟导星/不启动PHD2时这里会被前端误触发。
        // 为避免干扰导星日志，这里降级为 WARNING。
        Logger::Log("call_phd_StarClick | shared memory not ready (PHD2 not running?)", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0f;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &x, sizeof(int));
    addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, &y, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StarClick | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_StarClick failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StarClick success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_FocalLength(int FocalLength)
{
    Logger::Log("call_phd_FocalLength start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_FocalLength | FocalLength:" + std::to_string(FocalLength), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_FocalLength | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x10;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &FocalLength, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_FocalLength | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_FocalLength failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_FocalLength success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_MultiStarGuider(bool isMultiStar)
{
    Logger::Log("call_phd_MultiStarGuider start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_MultiStarGuider | isMultiStar:" + std::to_string(isMultiStar), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_MultiStarGuider | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x11;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &isMultiStar, sizeof(bool));
    addr = addr + sizeof(bool);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_MultiStarGuider | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_MultiStarGuider failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_MultiStarGuider success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CameraPixelSize(double PixelSize)
{
    Logger::Log("call_phd_CameraPixelSize start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CameraPixelSize | PixelSize:" + std::to_string(PixelSize), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_CameraPixelSize | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x12;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &PixelSize, sizeof(double));
    addr = addr + sizeof(double);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CameraPixelSize | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CameraPixelSize failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CameraPixelSize success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CameraGain(int Gain)
{
    Logger::Log("call_phd_CameraGain start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CameraGain | Gain:" + std::to_string(Gain), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_CameraGain | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x13;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Gain, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CameraGain | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CameraGain failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CameraGain success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CalibrationDuration(int StepSize)
{
    Logger::Log("call_phd_CalibrationDuration start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CalibrationDuration | StepSize:" + std::to_string(StepSize), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_CalibrationDuration | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x14;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &StepSize, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CalibrationDuration | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CalibrationDuration failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CalibrationDuration success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_RaAggression(int Aggression)
{
    Logger::Log("call_phd_RaAggression start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_RaAggression | Aggression:" + std::to_string(Aggression), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_RaAggression | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x15;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Aggression, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_RaAggression | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_RaAggression failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_RaAggression success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_DecAggression(int Aggression)
{
    Logger::Log("call_phd_DecAggression start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_DecAggression | Aggression:" + std::to_string(Aggression), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_DecAggression | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x16;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Aggression, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_DecAggression | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_DecAggression failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_DecAggression success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

// ===================== 读取端：共享内存像素解码（完整实现） =====================
// 说明：
// 1) 保持你原有的共享内存布局不变：
//    - [0 .. 1023]  : 预留区（新增 V2 头 ShmHdrV2 放在这里，兼容旧读端）
//    - [1024 ..]    : 你原有的头/状态/导星数据（currentPHDSizeX/Y、bitDepth、各项 guide 数据等）
//    - [2047]       : 帧完成标志位（0x01=写入中，0x02=完成，0x00=已读）
//    - [2048 .. end]: 像素数据（RAW / RLE 压缩 / NEAREST 缩放）
// 2) 本实现自动识别新头（若存在），按 coding 解码；若无新头，则回退旧逻辑。
// 3) 不改变你已有的“读取的内容”（导星/状态字段），仅在像素拷贝前做安全边界检查与解码。


// ===== RLE 解压（速度优先的简单实现）=====
static bool rle_decompress_8(const uint8_t* src, size_t n, uint8_t* dst, size_t outPixels) {
    size_t si=0, di=0;
    while (si+1 <= n && di < outPixels) {
        if (si + 1 > n) return false;
        uint8_t v = src[si++];
        if (si >= n) return false;
        uint8_t run = src[si++];
        if ((size_t)di + run > outPixels) return false;
        std::memset(dst + di, v, run);
        di += run;
    }
    return di == outPixels;
}

static bool rle_decompress_16(const uint8_t* src, size_t n, uint16_t* dst, size_t outPixels) {
    size_t si=0, di=0;
    while (si+4 <= n && di < outPixels) {
        uint16_t v, run;
        std::memcpy(&v,   src+si, 2); si += 2;
        std::memcpy(&run, src+si, 2); si += 2;
        if ((size_t)di + run > outPixels) return false;
        for (uint16_t k=0;k<run;++k) dst[di++] = v;
    }
    return di == outPixels;
}

// ====== 完整的读取函数（在你的 MainWindow 类内）=====
void MainWindow::ShowPHDdata()
{
    // 已弃用 PHD2：保留函数签名以减少改动，但不再读取共享内存，避免刷屏日志。
    return;

    // 修复：增强共享内存指针安全检查
    // 早退：共享内存可用且帧完成
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("ShowPHDdata | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return;
    }

    // 修复：验证共享内存大小，确保kFlagOff在有效范围内
    const size_t total_size = (size_t)BUFSZ;
    if (kFlagOff >= total_size) {
        Logger::Log("ShowPHDdata | kFlagOff out of bounds", LogLevel::ERROR, DeviceType::GUIDER);
        return;
    }

    if (sharedmemory_phd[kFlagOff] != 0x02) {
        // 没有新帧
        return;
    }

    // ---------- 读图像原始头（与旧协议完全一致） ----------
    unsigned int currentPHDSizeX = 1;
    unsigned int currentPHDSizeY = 1;
    unsigned int bitDepth        = 8;

    unsigned int mem_offset = kHeaderOff;

    auto ensure = [&](size_t need) -> bool {
        // 头部区域必须在 payload 前结束
        return (mem_offset + need <= kPayloadOff && mem_offset + need <= total_size);
    };

    if (!ensure(sizeof(unsigned int))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&currentPHDSizeX, sharedmemory_phd + mem_offset, sizeof(unsigned int));
    mem_offset += sizeof(unsigned int);

    if (!ensure(sizeof(unsigned int))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&currentPHDSizeY, sharedmemory_phd + mem_offset, sizeof(unsigned int));
    mem_offset += sizeof(unsigned int);

    if (!ensure(sizeof(unsigned char))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&bitDepth, sharedmemory_phd + mem_offset, sizeof(unsigned char));
    mem_offset += sizeof(unsigned char);

    if (!(bitDepth == 8 || bitDepth == 16)) {
        Logger::Log("ShowPHDdata | invalid bitDepth: " + std::to_string(bitDepth), LogLevel::WARNING, DeviceType::GUIDER);
        sharedmemory_phd[kFlagOff] = 0x00;
        return;
    }

    /* ------------------------------  新增：先读 V2 头，决定本帧尺寸  ------------------------------ */
    ShmHdrV2 v2{};
    bool hasV2 = false;
    if (total_size >= sizeof(ShmHdrV2)) {
        std::memcpy(&v2, sharedmemory_phd, sizeof(ShmHdrV2));
        hasV2 = (v2.magic == SHM_MAGIC && v2.version == SHM_VER);
    }

    // 本帧用于 UI/WS 的“实际尺寸/位深”
    uint32_t dispW = hasV2 ? v2.outW : currentPHDSizeX;
    uint32_t dispH = hasV2 ? v2.outH : currentPHDSizeY;
    uint16_t useDepth = hasV2 ? v2.bitDepth : (uint16_t)bitDepth;

    // 合法性兜底
    if (dispW == 0 || dispH == 0 || !(useDepth==8 || useDepth==16)) {
        // 回退到旧头
        hasV2 = false;
        dispW = currentPHDSizeX;
        dispH = currentPHDSizeY;
        useDepth = (uint16_t)bitDepth;
    }

    // 记录原始/输出尺寸与缩放倍数，供坐标换算使用
    glPHD_OrigImageSizeX = hasV2 ? (int)v2.origW : (int)currentPHDSizeX;
    glPHD_OrigImageSizeY = hasV2 ? (int)v2.origH : (int)currentPHDSizeY;
    glPHD_OutImageSizeX  = (int)dispW;
    glPHD_OutImageSizeY  = (int)dispH;
    {
        double sx = (glPHD_OutImageSizeX  > 0) ? (double)glPHD_OrigImageSizeX / (double)glPHD_OutImageSizeX  : 1.0;
        double sy = (glPHD_OutImageSizeY  > 0) ? (double)glPHD_OrigImageSizeY / (double)glPHD_OutImageSizeY  : 1.0;
        int s = (int)std::lround((sx + sy) * 0.5);
        if (s < 1) s = 1;
        glPHD_ImageScale = s;
    }

    // ---------- 跳过你原有的 3 个 int 字段（sdk_*） ----------
    if (!ensure(sizeof(int))) { sharedmemory_phd[kFlagOff]=0x00; return; }  mem_offset += sizeof(int);
    if (!ensure(sizeof(int))) { sharedmemory_phd[kFlagOff]=0x00; return; }  mem_offset += sizeof(int);
    if (!ensure(sizeof(int))) { sharedmemory_phd[kFlagOff]=0x00; return; }  mem_offset += sizeof(int);

    // ---------- 读取导星/状态数据（保持不变，不缺少） ----------
    unsigned int guideDataIndicatorAddress = (unsigned int)mem_offset;
    if (!ensure(sizeof(unsigned char))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    unsigned char guideDataIndicator = *(unsigned char*)(sharedmemory_phd + mem_offset);
    mem_offset += sizeof(unsigned char);

    double dRa=0, dDec=0, SNR=0, MASS=0, RMSErrorX=0, RMSErrorY=0, RMSErrorTotal=0, PixelRatio=1;
    int RADUR=0, DECDUR=0; char RADIR=0, DECDIR=0; bool StarLostAlert=false, InGuiding=false;

    auto safe_copy = [&](void* dst, size_t n) -> bool {
        if (!ensure(n)) { sharedmemory_phd[kFlagOff]=0x00; return false; }
        std::memcpy(dst, sharedmemory_phd + mem_offset, n);
        mem_offset += n;
        return true;
    };

    if (!safe_copy(&dRa, sizeof(double))) return;
    if (!safe_copy(&dDec, sizeof(double))) return;
    if (!safe_copy(&SNR, sizeof(double))) return;
    if (!safe_copy(&MASS, sizeof(double))) return;
    if (!safe_copy(&RADUR, sizeof(int))) return;
    if (!safe_copy(&DECDUR, sizeof(int))) return;
    if (!safe_copy(&RADIR, sizeof(char))) return;
    if (!safe_copy(&DECDIR, sizeof(char))) return;
    if (!safe_copy(&RMSErrorX, sizeof(double))) return;
    if (!safe_copy(&RMSErrorY, sizeof(double))) return;
    if (!safe_copy(&RMSErrorTotal, sizeof(double))) return;
    if (!safe_copy(&PixelRatio, sizeof(double))) return;
    if (!safe_copy(&StarLostAlert, sizeof(bool))) return;
    if (!safe_copy(&InGuiding, sizeof(bool))) return;

    // 你的 1024+200 区域：锁星、十字、MultiStar（保持不变）
    mem_offset = kHeaderOff + 200;
    auto ensure_at = [&](size_t off, size_t n)->bool {
        return (off + n <= kPayloadOff && off + n <= total_size);
    };

    bool isSelected=false, showLockedCross=false;
    double StarX=0, StarY=0, LockedPositionX=0, LockedPositionY=0;
    unsigned char MultiStarNumber=0;
    unsigned short MultiStarX[32]={0}, MultiStarY[32]={0};

    if (!ensure_at(mem_offset, sizeof(bool))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&isSelected, sharedmemory_phd + mem_offset, sizeof(bool)); mem_offset += sizeof(bool);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&StarX, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&StarY, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(bool))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&showLockedCross, sharedmemory_phd + mem_offset, sizeof(bool)); mem_offset += sizeof(bool);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&LockedPositionX, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&LockedPositionY, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(unsigned char))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&MultiStarNumber, sharedmemory_phd + mem_offset, sizeof(unsigned char)); mem_offset += sizeof(unsigned char);
    MultiStarNumber = std::min<unsigned char>(MultiStarNumber, 32);

    if (!ensure_at(mem_offset, sizeof(MultiStarX))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(MultiStarX, sharedmemory_phd + mem_offset, sizeof(MultiStarX)); mem_offset += sizeof(MultiStarX);

    if (!ensure_at(mem_offset, sizeof(MultiStarY))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(MultiStarY, sharedmemory_phd + mem_offset, sizeof(MultiStarY)); mem_offset += sizeof(MultiStarY);

    // 清除导星数据指示位（保持你的行为）
    sharedmemory_phd[guideDataIndicatorAddress] = 0x00;

    // ---------- 将导星/锁星信息分发到 UI/WS（保持你的逻辑） ----------
    glPHD_isSelected         = isSelected;
    glPHD_StarX              = StarX;
    glPHD_StarY              = StarY;
    glPHD_CurrentImageSizeX  = dispW;   // UI 显示尺寸（合并/缩放后）
    glPHD_CurrentImageSizeY  = dispH;   // UI 显示尺寸（合并/缩放后）
    glPHD_LockPositionX      = LockedPositionX;
    glPHD_LockPositionY      = LockedPositionY;
    glPHD_ShowLockCross      = showLockedCross;

    glPHD_Stars.clear();
    // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
    // emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
    const double mapRatioX = (glPHD_OrigImageSizeX > 0) ? (double)glPHD_OutImageSizeX / (double)glPHD_OrigImageSizeX : 1.0;
    const double mapRatioY = (glPHD_OrigImageSizeY > 0) ? (double)glPHD_OutImageSizeY / (double)glPHD_OrigImageSizeY : 1.0;
    for (int i = 1; i < MultiStarNumber; i++) {
        if (i > 12) break;
        int outX = (int)std::lround(MultiStarX[i] * mapRatioX);
        int outY = (int)std::lround(MultiStarY[i] * mapRatioY);
        if (outX < 0) outX = 0;
        if (outY < 0) outY = 0;
        if (outX >= glPHD_OutImageSizeX) outX = glPHD_OutImageSizeX - 1;
        if (outY >= glPHD_OutImageSizeY) outY = glPHD_OutImageSizeY - 1;
        QPoint p; p.setX(outX); p.setY(outY);
        glPHD_Stars.push_back(p);
        // TODO(PHD2): 前端多星位置同步（暂不发送）
        // emit wsThread->sendMessageToClient(
        //     "PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
        //     QString::number(glPHD_CurrentImageSizeY) + ":" +
        //     QString::number(outX) + ":" + QString::number(outY));
    }

    if (glPHD_isSelected) {
        // TODO(PHD2): 前端锁星框显示（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
        int outStarX = (int)std::lround(glPHD_StarX * mapRatioX);
        int outStarY = (int)std::lround(glPHD_StarY * mapRatioY);
        if (outStarX < 0) outStarX = 0;
        if (outStarY < 0) outStarY = 0;
        if (outStarX >= glPHD_OutImageSizeX) outStarX = glPHD_OutImageSizeX - 1;
        if (outStarY >= glPHD_OutImageSizeY) outStarY = glPHD_OutImageSizeY - 1;
        // TODO(PHD2): 前端锁星框位置（暂不发送）
        // emit wsThread->sendMessageToClient(
        //     "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
        //     QString::number(glPHD_CurrentImageSizeY) + ":" +
        //     QString::number(outStarX) + ":" + QString::number(outStarY));
    } else {
        // TODO(PHD2): 前端锁星框隐藏（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarBoxView:false");
    }

    if (glPHD_ShowLockCross) {
        // TODO(PHD2): 前端锁星十字显示（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarCrossView:true");
        int outLockX = (int)std::lround(glPHD_LockPositionX * mapRatioX);
        int outLockY = (int)std::lround(glPHD_LockPositionY * mapRatioY);
        if (outLockX < 0) outLockX = 0;
        if (outLockY < 0) outLockY = 0;
        if (outLockX >= glPHD_OutImageSizeX) outLockX = glPHD_OutImageSizeX - 1;
        if (outLockY >= glPHD_OutImageSizeY) outLockY = glPHD_OutImageSizeY - 1;
        // TODO(PHD2): 前端锁星十字位置（暂不发送）
        // emit wsThread->sendMessageToClient(
        //     "PHD2StarCrossPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
        //     QString::number(glPHD_CurrentImageSizeY) + ":" +
        //     QString::number(outLockX) + ":" + QString::number(outLockY));
    } else {
        // TODO(PHD2): 前端锁星十字隐藏（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarCrossView:false");
    }

    // ---------- 导星状态/曲线 ----------
    // 不要用 (dRa/dDec != 0) 判断“是否有新导星数据”：
    // - 误差为 0 是完全合法的（导星很稳时经常出现），此时 RMS/曲线也应持续更新。
    // - shared memory 中 guideDataIndicator 用来标记“本帧有新导星数据”，读取后会被清零。
    if (sharedmemory_phd[kFlagOff] == 0x02 && bitDepth > 0 && currentPHDSizeX > 0 && currentPHDSizeY > 0) {
        unsigned char phdstatu;
        call_phd_checkStatus(phdstatu);

        Logger::Log("ShowPHDdata | dRa:" + std::to_string(dRa) + ", dDec:" + std::to_string(dDec) +
                    ", guideDataIndicator:" + std::to_string((int)guideDataIndicator),
                    LogLevel::DEBUG, DeviceType::GUIDER);

        if (dRa != 0 || dDec != 0) {
            QPointF tmp; tmp.setX(-dRa * PixelRatio); tmp.setY(dDec * PixelRatio);
            glPHD_rmsdate.append(tmp);
            // TODO(PHD2): 前端散点图数据（暂不发送）
            // emit wsThread->sendMessageToClient("AddScatterChartData:" +
            //     QString::number(-dRa * PixelRatio) + ":" + QString::number(-dDec * PixelRatio));

            if (InGuiding) {
                // TODO(PHD2): 前端导星状态（暂不发送）
                // emit wsThread->sendMessageToClient("GuiderStatus:InGuiding");
                // emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
            } else {
                // TODO(PHD2): 前端导星状态（暂不发送）
                // emit wsThread->sendMessageToClient("GuiderStatus:InCalibration");
                // emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
            }

            if (StarLostAlert) {
                Logger::Log("ShowPHDdata | send GuiderStatus:StarLostAlert",
                            LogLevel::DEBUG, DeviceType::GUIDER);
                // TODO(PHD2): 前端丢星告警（暂不发送）
                // emit wsThread->sendMessageToClient("GuiderStatus:StarLostAlert");
                // emit wsThread->sendMessageToClient("GuiderUpdateStatus:2");
            }

            // TODO(PHD2): 前端 RMS 曲线（暂不发送）
            // emit wsThread->sendMessageToClient("AddRMSErrorData:" +
            //     QString::number(RMSErrorX, 'f', 3) + ":" + QString::number(RMSErrorX, 'f', 3));
        }

        for (int i = 0; i < glPHD_rmsdate.size(); i++) {
            if (i == glPHD_rmsdate.size() - 1) {
                // TODO(PHD2): 前端折线图数据（暂不发送）
                // emit wsThread->sendMessageToClient("AddLineChartData:" + QString::number(i) + ":" +
                //     QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(glPHD_rmsdate[i].y()));
                if (i > 50)
                    ; // TODO(PHD2): 前端折线图范围（暂不发送）
                else
                    ; // TODO(PHD2): 前端折线图范围（暂不发送）
            }
        }
    }

    // ===================== 像素数据读取 / 解码 =====================
    const size_t payload_cap_bytes = (total_size > kPayloadOff) ? (total_size - kPayloadOff) : 0;
    if (payload_cap_bytes == 0) { sharedmemory_phd[kFlagOff] = 0x00; return; }

    cv::Mat PHDImg;
    std::unique_ptr<uint8_t[]> buf;

    if (hasV2) {
        Logger::Log("V2 hdr: coding=" + std::to_string(v2.coding) +
                    " out=" + std::to_string(v2.outW) + "x" + std::to_string(v2.outH) +
                    " depth=" + std::to_string(v2.bitDepth) +
                    " payload=" + std::to_string(v2.payloadSize),
                    LogLevel::DEBUG, DeviceType::GUIDER);

        const uint16_t coding   = v2.coding;
        const uint16_t v2Depth  = v2.bitDepth;
        const size_t   bpp      = (v2Depth == 16) ? 2 : 1;
        const uint32_t outW     = v2.outW;
        const uint32_t outH     = v2.outH;
        const size_t   outPix   = (size_t)outW * (size_t)outH;
        const size_t   need     = outPix * bpp;
        const size_t   payLen   = (size_t)v2.payloadSize;

        if (!(v2Depth==8 || v2Depth==16) || outW==0 || outH==0 || payLen > payload_cap_bytes) {
            // 头异常，退回旧逻辑
            hasV2 = false;
        } else {
            const uint8_t* payload = (const uint8_t*)(sharedmemory_phd + kPayloadOff);

            if (coding == CODING_RAW || coding == CODING_NEAREST) {
                if (need > payload_cap_bytes) { sharedmemory_phd[kFlagOff] = 0x00; return; }
                buf.reset(new uint8_t[need]);
                std::memcpy(buf.get(), payload, need);
                if (v2Depth == 16) PHDImg.create(outH, outW, CV_16UC1);
                else               PHDImg.create(outH, outW, CV_8UC1);
                PHDImg.data = buf.get();
            } else if (coding == CODING_RLE) {
                buf.reset(new uint8_t[need]);
                bool ok = (v2Depth==8)
                    ? rle_decompress_8(payload, payLen, buf.get(), outPix)
                    : rle_decompress_16(payload, payLen, (uint16_t*)buf.get(), outPix);
                if (!ok) { sharedmemory_phd[kFlagOff] = 0x00; return; }
                if (v2Depth == 16) PHDImg.create(outH, outW, CV_16UC1);
                else               PHDImg.create(outH, outW, CV_8UC1);
                PHDImg.data = buf.get();
            } else {
                hasV2 = false;
            }
        }
    }

    if (!hasV2) {
        Logger::Log("Legacy path (no V2 header)", LogLevel::DEBUG, DeviceType::GUIDER);
        const size_t need = (size_t)currentPHDSizeX * (size_t)currentPHDSizeY * (bitDepth / 8);
        if (need == 0 || need > payload_cap_bytes) {
            Logger::Log("ShowPHDdata | legacy frame too large or zero", LogLevel::WARNING, DeviceType::GUIDER);
            sharedmemory_phd[kFlagOff] = 0x00;
            return;
        }
        buf.reset(new uint8_t[need]);
        std::memcpy(buf.get(), sharedmemory_phd + kPayloadOff, need);
        if (bitDepth == 16) PHDImg.create(currentPHDSizeY, currentPHDSizeX, CV_16UC1);
        else                PHDImg.create(currentPHDSizeY, currentPHDSizeX, CV_8UC1);
        PHDImg.data = buf.get();
    }

    // 像素完整拷贝/解码完成后再清 2047 标志，避免半帧被清
    sharedmemory_phd[kFlagOff] = 0x00;

    // ===== 你的后处理：拉伸/保存/显示（保持不变）=====
    uint16_t B = 0;
    uint16_t W = (PHDImg.depth() == CV_8U) ? 255 : 65535;  // 根据图像位深度设置默认最大值

    cv::Mat image_raw8;
    image_raw8.create(PHDImg.rows, PHDImg.cols, CV_8UC1);

    if (AutoStretch == true) {
        Tools::GetAutoStretch(PHDImg, 0, B, W);
    } else {
        B = 0;
        W = (PHDImg.depth() == CV_8U) ? 255 : 65535;  // 根据图像位深度设置默认最大值
    }
    Tools::Bit16To8_Stretch(PHDImg, image_raw8, B, W);
    saveGuiderImageAsJPG(image_raw8);

}



#endif // QUARCS_ENABLE_EXTERNAL_PHD2

#if QUARCS_ENABLE_EXTERNAL_PHD2
void MainWindow::getTimeNow(int index)
{
    // 获取当前时间点
    auto now = std::chrono::system_clock::now();

    // 将当前时间点转换为毫秒
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // 将毫秒时间戳转换为时间类型（std::time_t）
    std::time_t time_now = ms / 1000; // 将毫秒转换为秒

    // 使用 std::strftime 函数将时间格式化为字符串
    char buffer[80];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&time_now));

    // 添加毫秒部分
    std::string formatted_time = buffer + std::to_string(ms % 1000);

    // 打印带有当前时间的输出
    // std::cout << "TIME(ms): " << formatted_time << "," << index << std::endl;
}

void MainWindow::onPHDControlGuideTimeout()
{
    // Logger::Log("PHD2 Control Guide is Timeout !", LogLevel::DEBUG, DeviceType::MAIN);
    GetPHD2ControlInstruct();
}

void MainWindow::GetPHD2ControlInstruct()
{
    // Logger::Log("GetPHD2ControlInstruct start ...", LogLevel::DEBUG, DeviceType::MAIN);
    std::lock_guard<std::mutex> lock(receiveMutex);

    unsigned int mem_offset;

    int sdk_direction = 0;
    int sdk_duration = 0;
    int sdk_num = 0;
    int zero = 0;
    mem_offset = 1024;

    mem_offset = mem_offset + sizeof(unsigned int);
    mem_offset = mem_offset + sizeof(unsigned int);
    mem_offset = mem_offset + sizeof(unsigned char);

    int ControlInstruct = 0;

    memcpy(&ControlInstruct, sharedmemory_phd + mem_offset, sizeof(int));
    // Logger::Log("GetPHD2ControlInstruct | get ControlInstruct:" + std::to_string(ControlInstruct), LogLevel::DEBUG, DeviceType::MAIN);
    int mem_offset_sdk_num = mem_offset;
    mem_offset = mem_offset + sizeof(int);

    sdk_num = (ControlInstruct >> 24) & 0xFFF;       // 取前12位
    sdk_direction = (ControlInstruct >> 12) & 0xFFF; // 取中间12位
    sdk_duration = ControlInstruct & 0xFFF;          // 取后12位

    if (sdk_num != 0)
    {
        getTimeNow(sdk_num);
        // Logger::Log("GetPHD2ControlInstruct | PHD2ControlTelescope:" + std::to_string(sdk_num) + "," + std::to_string(sdk_direction) + "," + std::to_string(sdk_duration), LogLevel::DEBUG, DeviceType::MAIN);
    }
    if (sdk_duration != 0)
    {
        MainWindow::ControlGuideEx(sdk_direction, sdk_duration, QStringLiteral("PHD2"));

        memcpy(sharedmemory_phd + mem_offset_sdk_num, &zero, sizeof(int));
        // Logger::Log("GetPHD2ControlInstruct | set ControlInstruct to 0", LogLevel::DEBUG, DeviceType::MAIN);
        call_phd_ChackControlStatus(sdk_num); // set pFrame->ControlStatus = 0;
    }
    // Logger::Log("GetPHD2ControlInstruct finish!", LogLevel::DEBUG, DeviceType::MAIN);
}
#endif

