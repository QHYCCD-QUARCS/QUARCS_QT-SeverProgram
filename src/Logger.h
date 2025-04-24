#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <string>
#include <fstream>
#include <map>
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <string>
#include <filesystem> // 包含文件系统库
#include <QDebug>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "websocketthread.h"

enum LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

enum DeviceType {
    MAIN,
    CAMERA,
    GUIDER,
    FOCUSER,
    MOUNT,
    CFW
};

class Logger {
private:
    static std::map<DeviceType, std::unique_ptr<std::ofstream>> logFiles;
    static std::mutex logMutex;  // 添加一个静态互斥锁
    static std::mutex readyMutex;  // 新增一个互斥锁用于保护 readyToFlush
    static bool readyToFlush;  // 改为普通的 bool 变量
    static std::condition_variable logCond;
    static std::thread flushThread;
    static const unsigned int maxLogSize ; // 设定最大日志文件大小为100MB
    static bool shouldLogDebug; // 添加静态成员变量控制 DEBUG 日志
    static void RotateLogs(DeviceType device);
    static std::string BuildLogEntry(const std::string& message, LogLevel level, DeviceType device);
    static void FlushLogs();
    static void MonitorFlush();
public:
    static void Initialize();
    static void Close();
    static void Log(const std::string& message, LogLevel level, DeviceType device);
    static std::string ReadLog(DeviceType device); // 新增读取日志的方法
    static void SetDebugLogging(bool enable); // 添加方法设置 DEBUG 日志记录
    static WebSocketThread *wsThread;

};

#endif // LOGGER_H
