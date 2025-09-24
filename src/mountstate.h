#include <iostream>
#include <functional>
#include <QElapsedTimer>

#include "Logger.h"
#include "tools.h"


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

    // 观测地经纬度（度）。经度东经为正，西经为负；纬度北纬为正，南纬为负
    double Longitude_Degree = 0.0;
    double Latitude_Degree = 0.0;

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


        // HOME（平衡杆朝下、指向天极）理论上时角 = 6h，所以 RA = LST - 6h
        Logger::Log("HOME位(平衡杆朝下)理论RA : " + std::to_string(Home_RA_Hours) + " 小时 ,DEC : " + std::to_string(Home_DEC_Degree) + " 度", LogLevel::INFO, DeviceType::MOUNT);
        Logger::Log("========================", LogLevel::INFO, DeviceType::MOUNT);
    }

    void updateHomeRAHours(double lat,double lon) {
        Longitude_Degree = lon;
        Latitude_Degree = lat;
        QDateTime utc = Tools::getSystemTimeUTC();
        Home_RA_Hours = computeHomeRAHours(utc, lon);
        // Logger::Log("更新HOME位(平衡杆朝下)理论RA : " + std::to_string(Home_RA_Hours) + " 小时 ,DEC : " + std::to_string(Home_DEC_Degree) + " 度", LogLevel::INFO, DeviceType::MOUNT);
    }

    // 计算 HOME 位的 RA（小时）。传入 UTC 时间与经度（度）
    // HOME 位时角为 6h，所以 RA = LST - 6h
    double computeHomeRAHours(QDateTime datetimeUTC, double longitude_degree) const {
        double lon_rad = Tools::DegreeToRad(longitude_degree);
        double lst_deg = Tools::getLST_Degree(datetimeUTC, lon_rad);
        double lst_hours = Tools::DegreeToHour(lst_deg);
        double home_ra_hours = lst_hours - 6.0;
        
        // 确保 RA 在 0-24 小时范围内
        if (home_ra_hours < 0.0) {
            home_ra_hours += 24.0;
        }
        
        return home_ra_hours;
    }

    // 计算给定目标 RA 的时角（小时）。传入 UTC 时间、经度（度）与目标 RA（小时）
    double computeHAHours(QDateTime datetimeUTC, double longitude_degree, double target_ra_hours) const {
        double lon_rad = Tools::DegreeToRad(longitude_degree);
        double lst_deg = Tools::getLST_Degree(datetimeUTC, lon_rad);
        double ha_deg = Tools::getHA_Degree(Tools::HourToRad(target_ra_hours), lst_deg);
        return Tools::DegreeToHour(ha_deg);
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