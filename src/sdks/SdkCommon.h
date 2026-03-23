#ifndef SDKCOMMON_H
#define SDKCOMMON_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <any>

/**
 * @file SdkCommon.h
 * @brief SDK管理框架的通用类型定义
 * 
 * 本文件定义了SDK管理框架中使用的所有通用类型，包括：
 * - 命令类型和结构
 * - 设备状态和错误码
 * - 设备信息和能力描述
 * - 日志接口
 * 
 * 设计目标：
 * - 统一不同厂商SDK的接口差异
 * - 提供类型安全的命令和结果传递
 * - 支持灵活的参数类型（通过std::any）
 * - 便于扩展新的命令和设备类型
 */

/**
 * @enum SdkCommandType
 * @brief 统一的命令类型枚举
 * 
 * 定义了SDK框架支持的标准命令类型。每个驱动可以根据需要实现这些命令，
 * 也可以使用Custom类型定义自己的命令。
 * 
 * 使用示例：
 * @code
 * // 标准命令
 * SdkCommand cmd{SdkCommandType::StartExposure, "StartExposure", exposureParams};
 * 
 * // 自定义命令
 * SdkCommand customCmd{SdkCommandType::Custom, "GetTemperature", {}};
 * @endcode
 */
enum class SdkCommandType {
    OpenDevice,      ///< 打开设备命令（通常由框架自动处理）
    CloseDevice,     ///< 关闭设备命令（通常由框架自动处理）
    StartExposure,   ///< 开始曝光命令（相机设备常用）
    StopExposure,    ///< 停止曝光命令（相机设备常用）
    Custom           ///< 自定义命令：通过name字段进一步区分具体命令
};

/**
 * @enum SdkDeviceType
 * @brief 设备类型枚举
 * 
 * 用于区分不同类型的设备（相机、赤道仪、电调等），便于应用层正确识别和处理设备。
 * 每个驱动应在capabilities()中返回正确的设备类型。
 */
enum class SdkDeviceType {
    Unknown,      ///< 未知类型（未设置或无法确定）
    Camera,       ///< 相机设备（CCD/CMOS相机）
    Mount,        ///< 赤道仪设备（望远镜控制系统）
    Focuser,      ///< 电调设备（电动调焦器）
    FilterWheel,  ///< 滤镜轮设备
    Guider,       ///< 导星设备
    Rotator,      ///< 旋转器设备
    Dome,         ///< 圆顶设备
    Weather,      ///< 天气监测设备
    GPS,          ///< GPS设备
    Auxiliary     ///< 辅助设备（其他类型）
};

/**
 * @enum SdkDeviceState
 * @brief 设备状态枚举
 * 
 * 用于跟踪设备当前状态，便于统一管理和查询。
 * 设备状态由SdkManager自动维护，也可以通过updateDeviceState()手动更新。
 */
enum class SdkDeviceState {
    Unknown,  ///< 未知状态（设备未注册或状态不明）
    Closed,   ///< 已关闭状态
    Open,     ///< 已打开状态（可以执行命令）
    Error     ///< 错误状态（设备出现异常）
};

/**
 * @enum SdkErrorCode
 * @brief 统一错误码枚举
 * 
 * 定义了框架中可能出现的各种错误类型，用于统一错误处理。
 * 每个SdkResult都包含一个errorCode字段，便于程序化处理错误。
 * 
 * 错误码与success字段的关系：
 * - Success: success = true
 * - 其他错误码: success = false
 */
enum class SdkErrorCode {
    Success = 0,         ///< 操作成功
    DriverNotFound,      ///< 驱动未找到（驱动未注册或名称错误）
    DeviceNotFound,      ///< 设备未找到（设备未打开或ID错误）
    DeviceBusy,          ///< 设备忙碌（设备正在执行其他操作）
    InvalidParameter,    ///< 参数无效（参数类型或值不正确）
    OperationFailed,     ///< 操作失败（SDK调用失败）
    NotImplemented       ///< 功能未实现（驱动未实现该功能）
};

/**
 * @struct SdkCommand
 * @brief 统一的命令结构
 * 
 * 所有SDK操作都通过SdkCommand传递，包含命令类型、名称和参数。
 * 使用std::any作为payload，支持任意类型的参数，由驱动和调用方约定类型。
 * 
 * 使用示例：
 * @code
 * // 带参数的命令
 * struct ExposureParams {
 *     double exposureTime;
 *     bool isDark;
 * };
 * SdkCommand cmd{
 *     SdkCommandType::StartExposure,
 *     "StartExposure",
 *     ExposureParams{5.0, false}
 * };
 * 
 * // 无参数的命令
 * SdkCommand cmd2{
 *     SdkCommandType::Custom,
 *     "GetTemperature",
 *     {}
 * };
 * @endcode
 */
struct SdkCommand {
    SdkCommandType type;    ///< 命令类型（标准命令或自定义命令）
    std::string    name;    ///< 具体命令名称，例如 "GetTemperature"、"SetGain" 等
    std::any       payload; ///< 命令参数（可选），类型由驱动和调用方约定
};

/**
 * @struct SdkResult
 * @brief 统一的执行结果结构
 * 
 * 所有SDK操作都返回SdkResult，包含执行状态、错误信息和返回数据。
 * 
 * 使用示例：
 * @code
 * SdkResult result = SdkManager::instance().callById("MainCamera", cmd);
 * if (result.success) {
 *     // 操作成功，从payload中提取返回数据
 *     double temperature = std::any_cast<double>(result.payload);
 * } else {
 *     // 操作失败，检查错误码和错误信息
 *     qDebug() << "错误:" << result.message.c_str();
 *     if (result.errorCode == SdkErrorCode::DeviceBusy) {
 *         // 处理设备忙碌的情况
 *     }
 * }
 * @endcode
 */
struct SdkResult {
    bool         success{false};                    ///< 操作是否成功
    SdkErrorCode errorCode{SdkErrorCode::Success}; ///< 错误码（成功时为Success）
    std::string  message;                           ///< 错误或描述信息（人类可读）
    std::any     payload;                           ///< 返回数据（类型由驱动和调用方约定）
};

/**
 * @struct SdkCommandInfo
 * @brief 命令信息描述结构
 * 
 * 用于描述驱动支持的命令，便于查询和了解驱动功能。
 * 通过SdkManager::listCommands()可以获取驱动支持的所有命令。
 */
struct SdkCommandInfo {
    std::string name;        ///< 命令名称（与SdkCommand.name对应）
    std::string description; ///< 命令用途说明（人类可读的描述）
};

/**
 * @typedef SdkDeviceHandle
 * @brief 通用设备句柄类型
 * 
 * 设备句柄由各SDK驱动自己管理，框架只负责传递。
 * 句柄的生命周期由驱动和调用方共同管理：
 * - 通过openDevice()获取句柄
 * - 通过closeDevice()释放句柄
 * - 建议使用SdkDevice包装类自动管理生命周期
 */
using SdkDeviceHandle = void*;

/**
 * @struct SdkDeviceCapabilities
 * @brief 设备能力描述结构
 * 
 * 用于描述驱动或设备支持的功能和限制，便于应用层了解设备能力。
 * 通过SdkManager::capabilities()可以获取驱动的能力描述。
 */
struct SdkDeviceCapabilities {
    SdkDeviceType deviceType{SdkDeviceType::Unknown};       ///< 设备类型（相机、赤道仪、电调等）
    std::vector<std::string> supportedCommands;              ///< 支持的命令名称列表
    bool supportsMultiDevice{false};                         ///< 是否支持同时打开多个设备实例
    std::unordered_map<std::string, std::any> parameters;   ///< 额外能力信息（如参数范围、限制等）
};

/**
 * @struct SdkDeviceInfo
 * @brief 统一的设备信息结构
 * 
 * 用于设备注册表和设备发现流程，包含设备的完整信息。
 * 设备信息由SdkManager维护，可以通过getDevice()和listDevices()查询。
 */
struct SdkDeviceInfo {
    std::string      deviceId;    ///< 设备唯一标识（如序列号、设备路径等）
    std::string      driverName;  ///< 驱动名称（用于查找对应的驱动）
    std::string      description; ///< 设备描述信息（可选，用于显示）
    SdkDeviceHandle  handle{nullptr}; ///< 设备句柄（由驱动管理）
    SdkDeviceState   state{SdkDeviceState::Unknown}; ///< 设备当前状态
    std::any         metadata;     ///< 设备元数据（可选，如能力信息、版本号等）
};

/**
 * @enum SdkLogLevel
 * @brief 日志级别枚举
 * 
 * 定义了SDK框架使用的日志级别，与标准日志系统对应。
 */
enum class SdkLogLevel {
    Debug,    ///< 调试信息（详细的执行过程）
    Info,     ///< 一般信息（正常操作记录）
    Warning,  ///< 警告信息（可能的问题）
    Error     ///< 错误信息（操作失败）
};

/**
 * @class ISdkLogger
 * @brief SDK日志接口
 * 
 * 定义了SDK框架的日志接口，允许应用层提供自定义的日志实现。
 * 如果不设置自定义日志器，框架会使用默认的Logger系统。
 * 
 * 使用示例：
 * @code
 * class MyLogger : public ISdkLogger {
 * public:
 *     void log(SdkLogLevel level, const std::string& message) override {
 *         // 自定义日志处理
 *     }
 * };
 * 
 * MyLogger logger;
 * SdkManager::instance().setLogger(&logger);
 * @endcode
 */
class ISdkLogger {
public:
    virtual ~ISdkLogger() = default;
    
    /**
     * @brief 记录日志
     * @param level 日志级别
     * @param message 日志消息
     */
    virtual void log(SdkLogLevel level, const std::string& message) = 0;
};

/**
 * @struct SdkControlParamInfo
 * @brief 统一的控制参数信息结构
 * 
 * 用于描述设备控制参数（如增益、偏置、USB流量等）的范围和当前值。
 * 所有SDK驱动返回的参数信息都应使用此统一结构。
 * 
 * 使用示例：
 * @code
 * SdkCommand cmd{SdkCommandType::Custom, "GetGain", {}};
 * SdkResult result = SdkManager::instance().callById("MainCamera", cmd);
 * if (result.success) {
 *     SdkControlParamInfo info = std::any_cast<SdkControlParamInfo>(result.payload);
 *     qDebug() << "Gain range:" << info.minValue << "-" << info.maxValue;
 *     qDebug() << "Current gain:" << info.current;
 * }
 * @endcode
 */
struct SdkControlParamInfo {
    double minValue{0.0};   ///< 参数最小值
    double maxValue{0.0};   ///< 参数最大值
    double step{0.0};       ///< 参数步进
    double current{0.0};    ///< 当前值
};

/**
 * @struct SdkFocuserOpenParam
 * @brief 统一的电调设备打开参数结构
 * 
 * 用于打开电调设备时传递串口参数。
 * 所有SDK驱动的电调设备都应使用此统一结构作为打开参数。
 * 
 * 使用示例：
 * @code
 * SdkFocuserOpenParam param;
 * param.port = "/dev/ttyACM0";
 * param.baudRate = 9600;
 * param.timeoutMs = 3000;
 * 
 * SdkResult result = SdkManager::instance().openDevice("Focuser", "focuser_id", param);
 * @endcode
 */
struct SdkFocuserOpenParam {
    std::string port;           ///< 串口路径，例如 "/dev/ttyACM0"
    int         baudRate{9600}; ///< 波特率，与前端/系统配置一致
    int         timeoutMs{3000}; ///< 超时时间（毫秒）
};

/**
 * @file SdkCommon.h (续)
 * @brief SDK 通用设备类型定义（统一管理，避免直接包含驱动头文件）
 * 
 * 以下类型定义用于相机和电调设备，通过统一管理避免应用层直接依赖驱动实现细节。
 */

/**
 * @struct SdkFrameData
 * @brief SDK 相机单帧数据
 */
struct SdkFrameData {
    int                     width{0};      ///< 图像宽度（像素）
    int                     height{0};     ///< 图像高度（像素）
    unsigned int            bpp{0};        ///< 位深度
    unsigned int            channels{0};   ///< 通道数
    std::vector<uint16_t>   pixels;        ///< 像素数据（16 位单通道）
    // 零拷贝/共享缓冲区路径（可选）：
    // - 当希望避免把 SDK buffer 再拷贝到 pixels 时，驱动可以把原始图像数据放在 rawBuffer 中，
    //   并填写 rawBytes（有效字节数）。
    // - 约定：若 pixels 为空且 rawBuffer!=nullptr，则消费端优先从 rawBuffer 读取数据。
    // - rawBuffer 的生命周期由 shared_ptr 管理，确保跨线程传递时不会被提前释放。
    std::shared_ptr<std::vector<unsigned char>> rawBuffer; ///< 原始像素 buffer（由 SDK 直接写入）
    size_t                  rawBytes{0};   ///< rawBuffer 中的有效字节数（通常 = width*height*channels*(bpp/8)）
};

/**
 * @struct SdkAreaInfo
 * @brief SDK 相机区域信息（如 OverScan / 有效区域）
 */
struct SdkAreaInfo {
    unsigned int startX{0};  ///< 起始 X 坐标
    unsigned int startY{0};  ///< 起始 Y 坐标
    unsigned int sizeX{0};   ///< 区域宽度
    unsigned int sizeY{0};   ///< 区域高度
};

/**
 * @struct SdkChipInfo
 * @brief SDK 相机芯片信息
 */
struct SdkChipInfo {
    double       chipWidthMM{0.0};    ///< 芯片宽度（毫米）
    double       chipHeightMM{0.0};   ///< 芯片高度（毫米）
    double       pixelWidthUM{0.0};   ///< 像素宽度（微米）
    double       pixelHeightUM{0.0};  ///< 像素高度（微米）
    unsigned int maxImageSizeX{0};     ///< 最大图像宽度（像素）
    unsigned int maxImageSizeY{0};     ///< 最大图像高度（像素）
    unsigned int bpp{0};               ///< 位深度
};

/**
 * @struct SdkFocuserVersion
 * @brief SDK 电调版本信息
 */
struct SdkFocuserVersion {
    std::string id;          ///< 设备 ID
    int         version{0};  ///< 固件版本
    int         boardVersion{0}; ///< 板卡版本
};

/**
 * @struct SdkFocuserRelMoveParam
 * @brief SDK 电调相对移动参数
 */
struct SdkFocuserRelMoveParam {
    bool outward{false}; ///< 方向：true=向外，false=向内
    int  steps{0};       ///< 移动步数
};

#endif // SDKCOMMON_H


