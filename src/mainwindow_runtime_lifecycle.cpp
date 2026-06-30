#include "mainwindow.h"

void MainWindow::initializeRuntimeWorkersAndTimers()
{
    // 电调控制初始化
    focusMoveTimer = new QTimer(this);
    connect(focusMoveTimer, &QTimer::timeout, this, &MainWindow::HandleFocuserMovementDataPeriodically);

    // SDK 曝光定时器初始化
    sdkExposureTimer = new QTimer(this);
    sdkExposureTimer->setSingleShot(false);
    connect(sdkExposureTimer, &QTimer::timeout, this, &MainWindow::onSdkExposureTimerTimeout);

    // SDK 主相机 Live 循环取帧定时器初始化（仅 QHYCCD SDK 使用）
    sdkMainLiveTimer = new QTimer(this);
    sdkMainLiveTimer->setSingleShot(false);
    sdkMainLiveTimer->setInterval(33);
    connect(sdkMainLiveTimer, &QTimer::timeout, this, &MainWindow::onSdkMainLiveTimerTimeout);

    // SDK 主相机 Live 后处理定时器（主线程）：从“最新帧邮箱”取最新帧做 FITS/PNG/瓦片
    sdkMainLiveProcessTimer = new QTimer(this);
    sdkMainLiveProcessTimer->setSingleShot(false);
    sdkMainLiveProcessTimer->setInterval(50);
    connect(sdkMainLiveProcessTimer, &QTimer::timeout, this, &MainWindow::onSdkMainLiveProcessTimerTimeout);

    // SDK 导星曝光定时器初始化（独立于主相机 SDK 曝光）
    sdkGuiderExposureTimer = new QTimer(this);
    sdkGuiderExposureTimer->setSingleShot(false);
    connect(sdkGuiderExposureTimer, &QTimer::timeout, this, &MainWindow::onSdkGuiderExposureTimerTimeout);

    // SDK 串行执行线程（所有阻塞式 SDK 调用都应投递到对应线程）
    sdkCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkCamWorker"));
    sdkMainCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkMainCameraWorker"));
    sdkGuiderCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkGuiderCameraWorker"));
    sdkPoleCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkPoleCameraWorker"));
    sdkFocuserExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkFocuserWorker"));

    emit wsThread->sendMessageToClient("ServerInitSuccess");
    Logger::Log("ServerInitSuccess", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::shutdownRuntimeWorkersAndTimers()
{
    if (sdkExposureTimer)
        sdkExposureTimer->stop();
    if (sdkMainLiveTimer)
        sdkMainLiveTimer->stop();
    if (sdkMainLiveProcessTimer)
        sdkMainLiveProcessTimer->stop();
    if (sdkGuiderExposureTimer)
        sdkGuiderExposureTimer->stop();
    if (focusMoveTimer)
        focusMoveTimer->stop();

    cleanupQhySdkPoolAndResource("MainWindow::~MainWindow", "All");

    sdkPoleCamExec.reset();
    sdkGuiderCamExec.reset();
    sdkMainCamExec.reset();
    sdkCamExec.reset();
    sdkFocuserExec.reset();

    cleanupSdkMainLiveShm();
}

MainWindow::~MainWindow()
{
    shutdownRuntimeWorkersAndTimers();

    if (guiderCoreThread && guiderCoreThread->isRunning() && guiderCore)
    {
        QMetaObject::invokeMethod(guiderCore, [core = guiderCore]() {
            core->stopGuiding();
            core->stopLoop();
        }, Qt::BlockingQueuedConnection);
        guiderCoreThread->quit();
        guiderCoreThread->wait(3000);
        guiderCore = nullptr;
    }

    if (polarAlignment != nullptr)
    {
        polarAlignment->stopPolarAlignment();
        delete polarAlignment;
        polarAlignment = nullptr;
    }

    system("pkill indiserver");
    system("rm -f /tmp/myFIFO");

    Logger::wsThread = nullptr;

    wsThread->quit();
    wsThread->wait();
    delete wsThread;
    wsThread = nullptr;

    instance = nullptr;
}
