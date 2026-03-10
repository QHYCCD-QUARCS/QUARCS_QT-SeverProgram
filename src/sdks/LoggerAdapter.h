#ifndef SDK_LOGGER_ADAPTER_H
#define SDK_LOGGER_ADAPTER_H

#include "SdkCommon.h"
#include "../Logger.h"

// 适配现有 Logger 到 ISdkLogger 接口
class LoggerAdapter : public ISdkLogger {
public:
    explicit LoggerAdapter(DeviceType deviceType = DeviceType::MAIN)
        : m_device(deviceType)
    {}

    void log(SdkLogLevel level, const std::string& message) override
    {
        LogLevel logLevel;
        switch (level) {
        case SdkLogLevel::Debug:   logLevel = LogLevel::DEBUG; break;
        case SdkLogLevel::Info:    logLevel = LogLevel::INFO; break;
        case SdkLogLevel::Warning: logLevel = LogLevel::WARNING; break;
        case SdkLogLevel::Error:   logLevel = LogLevel::ERROR; break;
        default:                   logLevel = LogLevel::INFO; break;
        }
        Logger::Log(message, logLevel, m_device);
    }

private:
    DeviceType m_device;
};

#endif // SDK_LOGGER_ADAPTER_H


