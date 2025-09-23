#include <iostream>
#include <functional>
#include <QElapsedTimer>

#include "Logger.h"


struct MountState{
    bool isParked = false;
    bool isSlewing = false;
    bool isTracking = false;
    bool isGuiding = false;
    bool isHoming = false;
    bool isMoving = false;  // 移动中(可能是翻转，也可能是移动)
    bool isNS_Moving = false;
    bool isWE_Moving = false;

    double Home_RA_Hours = 6.0;
    double Home_DEC_Degree = 90.0;

    double Target_RA_Hours = 0.0;
    double Target_DEC_Degree = 0.0;

    // 获取并打印当前状态
    void printCurrentState() const {
        Logger::Log("=== 望远镜支架当前状态 ===", LogLevel::INFO, DeviceType::MOUNT);
        Logger::Log(std::string("停放状态: ") + (isParked ? "是" : "否"), LogLevel::INFO, DeviceType::MOUNT);
        Logger::Log(std::string("goto状态: ") + (isSlewing ? "是" : "否"), LogLevel::INFO, DeviceType::MOUNT);
        Logger::Log(std::string("跟踪状态: ") + (isTracking ? "是" : "否"), LogLevel::INFO, DeviceType::MOUNT);
        Logger::Log(std::string("导星状态: ") + (isGuiding ? "是" : "否"), LogLevel::INFO, DeviceType::MOUNT);
        Logger::Log(std::string("HOMING状态: ") + (isHoming ? "是" : "否"), LogLevel::INFO, DeviceType::MOUNT);
        Logger::Log("Home位置 - RA: " + std::to_string(Home_RA_Hours) + " 小时, DEC: " + std::to_string(Home_DEC_Degree) + " 度", LogLevel::INFO, DeviceType::MOUNT);
        Logger::Log("目标位置 - RA: " + std::to_string(Target_RA_Hours) + " 小时, DEC: " + std::to_string(Target_DEC_Degree) + " 度", LogLevel::INFO, DeviceType::MOUNT);
        Logger::Log("========================", LogLevel::INFO, DeviceType::MOUNT);
    }
    
    // 获取状态摘要字符串
    std::string getStateSummary() const {
        std::string summary = "支架状态: ";
        summary += (isParked ? "已停放 " : "");
        summary += (isSlewing ? "指向中 " : "");
        summary += (isTracking ? "跟踪中 " : "");
        summary += (isGuiding ? "导星中 " : "");
        if (!isSlewing && !isTracking && !isGuiding && !isHoming && !isNS_Moving && !isWE_Moving) {
            summary += "空闲";
        }
        return summary;
    }

    bool isMovingNow() const {
        return isSlewing || isHoming || isNS_Moving || isWE_Moving || isMoving;
    }
};