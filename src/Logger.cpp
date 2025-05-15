#include "Logger.h"
#include <mutex>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <thread>


std::map<DeviceType, std::unique_ptr<std::ofstream>> Logger::logFiles;
const unsigned int Logger::maxLogSize = 104857600; // 设定最大日志文件大小为100MB
std::mutex Logger::logMutex;  // 添加一个静态互斥锁
bool Logger::shouldLogDebug = true; // 初始化默认不记录 DEBUG 日志
bool Logger::readyToFlush = false;
std::mutex Logger::readyMutex;
std::condition_variable Logger::logCond;
std::thread Logger::flushThread;
WebSocketThread *Logger::wsThread = nullptr;

void Logger::Initialize() {
    std::cout << "************************************" << std::endl;
    // 使用 std::filesystem 检查日志目录是否存在
    std::string logDir = "logs";
    if (!std::filesystem::exists(logDir)) {
        std::filesystem::create_directories(logDir); // 如果目录不存在，则创建
    }

    // 为每种设备类型打开日志文件
    for (int deviceType = MAIN; deviceType <= CFW; ++deviceType) {
        std::string deviceName;
        switch (static_cast<DeviceType>(deviceType)) {
            case MAIN: deviceName = "MAIN"; break;
            case CAMERA: deviceName = "CAMERA"; break;
            case GUIDER: deviceName = "GUIDER"; break;
            case FOCUSER: deviceName = "FOCUSER"; break;
            case MOUNT: deviceName = "MOUNT"; break;
            case CFW: deviceName = "CFW"; break;
            default:
                std::cerr << "Unknown device type: " << deviceType << std::endl;
                continue; // 跳过未知设备类型
        }
        std::string logPath = logDir + "/" + deviceName + ".log";
        qDebug() << "Opening log file at path: " << logPath.c_str(); // 增加调试输出
        logFiles[static_cast<DeviceType>(deviceType)] = std::make_unique<std::ofstream>(logPath, std::ios::out | std::ios::app);
        if (!logFiles[static_cast<DeviceType>(deviceType)]->is_open()) {
            std::cerr << "Failed to open log file: " << logPath << std::endl;
            continue; // 这里可以考虑抛出异常或者返回错误状态
        }
        std::cout << "Log file opened: " << logPath << std::endl;
    }
    std::cout << "************************************" << std::endl;
    flushThread = std::thread(&Logger::MonitorFlush);
}

void Logger::Close() {
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        readyToFlush = true;
    }
    logCond.notify_one();
    if (flushThread.joinable())
        flushThread.join();
    for (auto& file : logFiles) {
        file.second->close();
    }
}

void Logger::RotateLogs(DeviceType device) {
    std::string deviceName;
    switch (device) {
        case MAIN: deviceName = "MAIN"; break;
        case CAMERA: deviceName = "CAMERA"; break;
        case GUIDER: deviceName = "GUIDER"; break;
        case FOCUSER: deviceName = "FOCUSER"; break;
        case MOUNT: deviceName = "MOUNT"; break;
        case CFW: deviceName = "CFW"; break;
        default:
            std::cerr << "Unknown device type: " << static_cast<int>(device) << std::endl;
            return; // 如果设备类型未知，则不进行操作
    }
    std::string oldLogPath = "logs/" + deviceName + ".log";
    std::string newLogPath = oldLogPath + ".old";
    logFiles[device]->close();
    std::rename(oldLogPath.c_str(), newLogPath.c_str());
    logFiles[device] = std::make_unique<std::ofstream>(oldLogPath, std::ios::out | std::ios::app);
}

void Logger::SetDebugLogging(bool enable) {
    shouldLogDebug = enable;
}

void Logger::Log(const std::string& message, LogLevel level, DeviceType device) {
    if (level == DEBUG && !shouldLogDebug) return;

    std::lock_guard<std::mutex> lock(logMutex);  // 使用 lock_guard 管理互斥锁

    // 检查文件大小，如果需要则轮转日志
    if (device != MAIN && logFiles[device]->tellp() > maxLogSize) {
        qDebug() << "Rotating log file for device " << device;
        RotateLogs(device);
    }

    std::string logEntry = BuildLogEntry(message, level, device);
    errno = 0;  // 重置 errno
    qDebug() << logEntry.c_str();
    std::string levelStr;
    switch (level) {
        case DEBUG: levelStr = "debug"; break;
        case INFO: levelStr = "info"; break;
        case WARNING: levelStr = "warning"; break;
        case ERROR: levelStr = "error"; break;
        default: levelStr = "unknown"; break;
    }

    if (wsThread != nullptr) {
        emit wsThread->sendMessageToClient("SendDebugMessage|" + QString::fromStdString(levelStr) + "|" + QString::fromStdString(message));
    }
    *(logFiles[device]) << logEntry;
    if (logFiles[device]->bad()) {
        std::cerr << "Bad file stream state encountered for device " << device << std::endl;
    }
    if (logFiles[device]->fail()) {
        std::cerr << "Failed to write to log file for device " << device << ": " << strerror(errno) << std::endl;
        logFiles[device]->clear();  // 清除错误标志
    }

    // 检查主日志文件的写入状态
    if (device != MAIN) {
        *(logFiles[MAIN]) << logEntry;
        if (logFiles[MAIN]->fail()) {
            std::cerr << "Failed to write to main log file for device " << MAIN << ": " << strerror(errno) << std::endl;
            logFiles[MAIN]->clear();  // 清除错误标志
        }
    }

    // 添加额外的错误检查以确保所有设备的日志都被正确处理
    for (auto& logFile : logFiles) {
        if (logFile.second->fail()) {
            std::cerr << "General write failure in log file for device " << logFile.first << ": " << strerror(errno) << std::endl;
            logFile.second->clear();  // 清除错误标志
        }
    }

    
}

std::string Logger::ReadLog(DeviceType device) {
    std::string deviceName;
    switch (static_cast<DeviceType>(device)) {
            case MAIN: deviceName = "MAIN"; break;
            case CAMERA: deviceName = "CAMERA"; break;
            case GUIDER: deviceName = "GUIDER"; break;
            case FOCUSER: deviceName = "FOCUSER"; break;
            case MOUNT: deviceName = "MOUNT"; break;
            case CFW: deviceName = "CFW"; break;
    }
    std::ifstream file("logs/" + deviceName + ".log");
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return content;
}

std::string Logger::BuildLogEntry(const std::string& message, LogLevel level, DeviceType device) {
    std::string deviceName;
    switch (device) {
        case MAIN: deviceName = "MAIN"; break;
        case CAMERA: deviceName = "CAMERA"; break;
        case GUIDER: deviceName = "GUIDER"; break;
        case FOCUSER: deviceName = "FOCUSER"; break;
        case MOUNT: deviceName = "MOUNT"; break;
        case CFW: deviceName = "CFW"; break;
        default:
            std::cerr << "Unknown device type: " << static_cast<int>(device) << std::endl;
            deviceName = "MAIN";
    }
    // 获取当前时间
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm = *std::localtime(&now_c);

    // 转换日志级别为字符串
    std::string levelStr;
    switch (level) {
        case DEBUG: levelStr = "DEBUG"; break;
        case INFO: levelStr = "INFO"; break;
        case WARNING: levelStr = "WARNING"; break;
        case ERROR: levelStr = "ERROR"; break;
        default: levelStr = "UNKNOWN"; break;
    }

    // 构建日志条目
    std::ostringstream stream;
    stream << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S") << " | "
           << levelStr << " | "
           << "Device " << deviceName << ": " << message;

    return stream.str();
}

void Logger::MonitorFlush() {
    using namespace std::chrono;
    auto lastFlush = steady_clock::now();
    std::unique_lock<std::mutex> lock(logMutex, std::defer_lock);

    while (true) {
        lock.lock();
        if (logCond.wait_for(lock, seconds(1), [] {
            std::lock_guard<std::mutex> readyLock(readyMutex);
            return readyToFlush;
        })) {
            break;
        }
        if (duration_cast<seconds>(steady_clock::now() - lastFlush) >= seconds(1)) {
            FlushLogs();
            lastFlush = steady_clock::now();
        }
        lock.unlock();
    }
}

void Logger::FlushLogs() {
    for (auto& file : logFiles) {
        if (file.second->is_open()) {
            file.second->flush();
        }
    }
}
