#include "QHYCCD.h"
#include "../LoggerAdapter.h"
#include "../SdkManager.h"

#include <cstring>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <unordered_map>

// 使用注册宏将驱动名与实现绑定（只在此翻译单元生成一次）
REGISTER_SDK_DRIVER(SDK_DRIVER_NAME_INDI_QHY_CCD, QhyCameraDriver)

namespace {
struct LiveBufferCache {
    // 说明：
    // - Live 拉帧时 SDK 会直接写入 buffer.data()。
    // - 主线程处理链路希望“零拷贝”直接读取该 buffer，因此需要确保：当某帧的 buffer
    //   仍被主线程持有时，下一帧不会写入同一块内存导致数据被覆盖。
    // - 用一个小型缓冲池（默认最多 2 块）来实现“尽可能零拷贝”的同时防止覆盖。
    std::vector<std::shared_ptr<std::vector<unsigned char>>> buffers;
    uint32_t length = 0; // 当前配置下的期望长度（GetQHYCCDMemLength）
};

std::mutex g_liveBufMu;
std::unordered_map<qhyccd_handle*, LiveBufferCache> g_liveBufByHandle;

// 节流：避免 Live 高频触发时刷屏
static std::atomic<long long> g_lastLiveCopyWarnMs{0};

// Live buffer 申请策略：
// - length 变化（全屏 <-> ROI / bin / bpp / channels 变化）则清空旧池并重新分配
// - 优先选择“未被外部持有”的 buffer（use_count==1），以保证零拷贝帧不会被覆盖
// - 池满且都被持有时：仍返回一个 buffer，但标记 exclusive=false，调用方应回退到拷贝路径
static std::shared_ptr<std::vector<unsigned char>> acquireLiveBuffer(qhyccd_handle* handle,
                                                                     uint32_t length,
                                                                     bool* outExclusive) {
    static constexpr size_t kMaxPool = 2; // 双缓冲足以覆盖“主线程处理慢但仍想持续拉帧”的典型场景
    std::lock_guard<std::mutex> lk(g_liveBufMu);
    auto& c = g_liveBufByHandle[handle];

    if (outExclusive) *outExclusive = true;

    if (c.length != length) {
        // 尺寸变化：释放旧池（全屏/ROI 切换时可回收大内存），重新开始
        c.buffers.clear();
        c.length = length;
    }

    // 1) 找可复用（未被外部持有）的 buffer
    for (auto& b : c.buffers) {
        if (b && b->size() == length && b.use_count() == 1) {
            return b;
        }
    }

    // 2) 池未满：新建一块
    if (c.buffers.size() < kMaxPool) {
        auto b = std::make_shared<std::vector<unsigned char>>(static_cast<size_t>(length));
        c.buffers.push_back(b);
        return b;
    }

    // 3) 池满且都在用：返回任意一块，但提示调用方不要走零拷贝（否则可能被覆盖）
    if (outExclusive) *outExclusive = false;
    if (!c.buffers.empty() && c.buffers[0]) {
        if (c.buffers[0]->size() != length) {
            c.buffers[0]->resize(static_cast<size_t>(length));
        }
        return c.buffers[0];
    }
    // 兜底：不应发生（池满却为空），强制分配
    auto b = std::make_shared<std::vector<unsigned char>>(static_cast<size_t>(length));
    c.buffers.clear();
    c.buffers.push_back(b);
    if (outExclusive) *outExclusive = true;
    return b;
}

static void eraseLiveBuffer(qhyccd_handle* handle) {
    std::lock_guard<std::mutex> lk(g_liveBufMu);
    g_liveBufByHandle.erase(handle);
}

static std::string qhyBayerIdToCfaString(unsigned int bayerId) {
    switch (bayerId) {
        case BAYER_GB:
            return "GBRG";
        case BAYER_GR:
            return "GRBG";
        case BAYER_BG:
            return "BGGR";
        case BAYER_RG:
            return "RGGB";
        default:
            return std::string();
    }
}
} // namespace

QhyCameraDriver::QhyCameraDriver()
{
}

QhyCameraDriver::~QhyCameraDriver()
{
}

std::vector<std::string> QhyCameraDriver::driverNames() const
{
    // 支持两个调用名
    return { SDK_DRIVER_NAME_INDI_QHY_CCD, SDK_DRIVER_NAME_QHYCCD };
}

std::vector<SdkCommandInfo> QhyCameraDriver::commandList() const
{
    return {
        {"GetSdkVersion",                "获取 QHYCCD SDK 版本号字符串"},
        {"GetFirmwareVersion",           "获取当前相机固件版本信息"},
        {"InitSdkResource",              "初始化 QHYCCD SDK 全局资源（无需句柄）"},
        {"ReleaseSdkResource",           "释放 QHYCCD SDK 全局资源（无需句柄）"},
        {"ScanCameras",                  "扫描当前连接的 QHYCCD 相机数量"},
        {"GetCameraIdByIndex",           "根据索引获取相机 ID，payload 传入 int 索引"},
        {"SetReadMode",                  "设置读出模式（SetQHYCCDReadMode），payload 传入 int readMode（通常为 0）"},
        {"SetStreamMode",                "设置图像流模式，例如单帧模式，payload 传入 int 模式值"},
        {"InitCamera",                   "初始化相机（InitQHYCCD），需已打开相机句柄"},
        {"GetOverScanArea",              "获取相机 OverScan 区域信息，返回 SdkAreaInfo"},
        {"GetEffectiveArea",             "获取相机有效成像区域信息，返回 SdkAreaInfo"},
        {"GetChipInfo",                  "获取芯片物理尺寸、像素尺寸、最大分辨率等信息，返回 SdkChipInfo"},
        {"SetDDR",                       "设置相机 DDR 模式（CONTROL_DDR），payload 传入 double（通常为 1.0）"},
        {"SetDebayerOnOff",              "设置去拜耳开关（SetQHYCCDDebayerOnOff），payload 传入 bool"},
        {"SetUsbTraffic",                "设置 USB 传输级别，payload 传入 double usbTraffic"},
        {"SetGain",                      "设置增益，payload 传入 double gain"},
        {"SetOffset",                    "设置偏置，payload 传入 double offset"},
        {"SetExposure",                  "设置曝光时间（微秒），payload 传入 double exposure"},
        {"GetUsbTraffic",                "获取 USBTraffic 的最小值/最大值/步进/当前值（SdkControlParamInfo）"},
        {"GetGain",                      "获取增益的最小值/最大值/步进/当前值（SdkControlParamInfo）"},
        {"GetOffset",                    "获取偏置的最小值/最大值/步进/当前值（SdkControlParamInfo）"},
        {"GetExposure",                  "获取曝光时间（微秒）的最小值/最大值/步进/当前值（SdkControlParamInfo）"},
        {"SetResolution",                "设置 ROI/分辨率，payload 传入 SdkAreaInfo"},
        {"SetBinMode",                   "设置 Bin 模式，payload 传入 std::pair<int,int> (binX, binY)"},
        {"SetBitsMode",                  "设置位深模式，payload 传入 int bits（一般为 16）"},
        {"GetBitsMode",                  "获取位深模式的最小值/最大值/步进/当前值（SdkControlParamInfo）"},
        {"StartSingleExposure",          "启动单帧曝光（ExpQHYCCDSingleFrame）"},
        {"GetMemLength",                 "获取单帧图像所需的缓冲区长度，返回 uint32_t"},
        {"GetSingleFrame",               "读取单帧图像数据，返回 SdkFrameData（16 位单通道）"},
        {"BeginLive",                    "进入 Live 连续采集（BeginQHYCCDLive），需先 SetStreamMode=1"},
        {"GetLiveFrame",                 "读取一帧 Live 图像数据（GetQHYCCDLiveFrame），返回 SdkFrameData"},
        {"GetLiveFrameFast",             "读取一帧 Live 图像数据（GetQHYCCDLiveFrame），复用缓冲区且不拷贝 pixels（仅返回 meta）"},
        {"StopLive",                     "停止 Live 连续采集（StopQHYCCDLive）"},
        {"EnableBurstMode",              "开启/关闭 Burst 子模式（EnableQHYCCDBurstMode），payload=bool"},
        {"SetBurstStartEnd",             "设置 Burst start/end（SetQHYCCDBurstModeStartEnd），payload=std::pair<int,int>"},
        {"ResetFrameCounter",            "复位帧计数器（ResetQHYCCDFrameCounter）"},
        {"SetBurstIDLE",                 "进入 Burst IDLE 状态（SetQHYCCDBurstIDLE）"},
        {"ReleaseBurstIDLE",             "释放 Burst IDLE 触发输出（ReleaseQHYCCDBurstIDLE）"},
        {"SetBurstPatchNumber",          "设置 Burst 补包数据量（SetQHYCCDBurstModePatchNumber），payload=uint32_t"},
        {"CheckSingleFrameModeAvailable","检测是否支持单帧模式（IsQHYCCDControlAvailable CAM_SINGLEFRAMEMODE），返回 bool"},
        {"IsColorCamera",                "检测当前相机是否为彩色相机（CAM_IS_COLOR），返回 bool"},
        {"GetCameraCfa",                 "按 CAM_IS_COLOR -> SetQHYCCDDebayerOnOff(true) -> CAM_COLOR 获取 CFA，返回 std::string"},
        {"GetCurrentTemperature",        "获取当前 CMOS/CCD 温度（CONTROL_CURTEMP），返回 double"},
        {"SetCoolerTargetTemperature",   "设置制冷目标温度（CONTROL_COOLER），payload 传入 double 摄氏度"},
        {"GetCoolerTargetTemperature",   "获取制冷目标温度的最小值/最大值/步进/当前值（SdkControlParamInfo）"},
        {"GetCoolerPower",               "获取当前制冷 PWM 功率百分比（GetQHYCCDParam CONTROL_CURPWM），返回 double"},
        {"CancelExposure",               "取消当前曝光与读出（CancelQHYCCDExposingAndReadout）"},
        {"IsCFWPlugged",                 "检测相机是否连接了滤镜轮（CFW），返回 bool"},
        {"GetCFWSlotsNum",               "获取滤镜轮的槽位数量（CONTROL_CFWSLOTSNUM），返回 int"},
        {"GetCFWPosition",               "获取当前滤镜轮位置（CONTROL_CFWPORT），返回 int（0 开始）"},
        {"SetCFWPosition",               "设置滤镜轮位置（CONTROL_CFWPORT），payload 传入 int 位置（0 开始）"},
        {"SendOrderToCFW",               "发送自定义命令到滤镜轮，payload 传入 std::string order"},
        {"GetCFWStatus",                 "获取滤镜轮状态字符串，返回 std::string"}
    };
}

SdkResult QhyCameraDriver::makeResultFromRet(const std::string& action, unsigned int ret) const
{
    SdkResult r;
    // QHYCCD_READ_DIRECTLY 不是错误：表示可立即读取帧数据
    if (ret == QHYCCD_SUCCESS || ret == QHYCCD_READ_DIRECTLY) {
        r.success = true;
        r.errorCode = SdkErrorCode::Success;
        r.message = (ret == QHYCCD_READ_DIRECTLY)
                        ? (action + " success (READ_DIRECTLY)")
                        : (action + " success");
    } else {
        r.success = false;
        r.errorCode = SdkErrorCode::OperationFailed;
        r.message = action + " failed, error code: " + std::to_string(ret);
    }
    return r;
}

SdkResult QhyCameraDriver::openDevice(const std::any& openParam)
{
    SdkResult r;

    // 1. 初始化 SDK 资源（只初始化一次）
    if (!m_resourceInited) {
        unsigned int ret = InitQHYCCDResource();
        if (ret != QHYCCD_SUCCESS) {
            r.success = false;
            r.message = "InitQHYCCDResource failed, error code: " + std::to_string(ret);
            return r;
        }
        m_resourceInited = true;
    }

    // 2. 期望 openParam 为 std::string，表示 cameraId
    std::string camId;
    if (!extractParam<std::string>(openParam, camId, r, "cameraId")) {
        return r;
    }

    // OpenQHYCCD 需要 char* 而非 const char*，创建可修改副本
    std::vector<char> camIdBuf(camId.begin(), camId.end());
    camIdBuf.push_back('\0');
    qhyccd_handle *handle = OpenQHYCCD(camIdBuf.data());
    if (handle == nullptr) {
        r.success = false;
        r.errorCode = SdkErrorCode::DeviceNotFound;
        r.message = "OpenQHYCCD failed for cameraId: " + camId;
        return r;
    }

    r.success = true;
    r.message = "OpenQHYCCD success for cameraId: " + camId;
    r.payload = static_cast<SdkDeviceHandle>(handle);
    return r;
}

SdkResult QhyCameraDriver::closeDevice(SdkDeviceHandle device)
{
    SdkResult r;
    qhyccd_handle *handle = static_cast<qhyccd_handle*>(device);
    if (!handle) {
        r.success = false;
        r.errorCode = SdkErrorCode::InvalidParameter;
        r.message = "closeDevice: handle is null";
        return r;
    }

    unsigned int ret = CloseQHYCCD(handle);
    // 清理该句柄对应的 Live 缓冲区缓存，避免长期占用大内存
    eraseLiveBuffer(handle);
    return makeResultFromRet("CloseQHYCCD", ret);
}

SdkResult QhyCameraDriver::execute(SdkDeviceHandle device, const SdkCommand& cmd)
{
    SdkResult r;

    qhyccd_handle *handle = static_cast<qhyccd_handle*>(device);

    // 根据命令名称拆分具体功能
    if (cmd.type == SdkCommandType::Custom) {
        const std::string& name = cmd.name;

        // 1. SDK 版本号
        if (name == "GetSdkVersion") {
            unsigned int YMDS[4] = {0};
            unsigned char sVersion[80];
            std::memset(sVersion, 0, sizeof(sVersion));
            GetQHYCCDSDKVersion(&YMDS[0], &YMDS[1], &YMDS[2], &YMDS[3]);

            char buf[80] = {0};
            if ((YMDS[1] < 10) && (YMDS[2] < 10)) {
                std::sprintf(buf, "V20%d0%d0%d_%d", YMDS[0], YMDS[1], YMDS[2], YMDS[3]);
            } else if ((YMDS[1] < 10) && (YMDS[2] > 10)) {
                std::sprintf(buf, "V20%d0%d%d_%d", YMDS[0], YMDS[1], YMDS[2], YMDS[3]);
            } else if ((YMDS[1] > 10) && (YMDS[2] < 10)) {
                std::sprintf(buf, "V20%d%d0%d_%d", YMDS[0], YMDS[1], YMDS[2], YMDS[3]);
            } else {
                std::sprintf(buf, "V20%d%d%d_%d", YMDS[0], YMDS[1], YMDS[2], YMDS[3]);
            }

            r.success = true;
            r.message = std::string("QHYCCD SDK version: ") + buf;
            r.payload = std::string(buf);
            return r;
        }

        // 2. 固件版本（需要已打开的相机句柄）
        if (name == "GetFirmwareVersion") {
            if (!handle) {
                r.success = false;
                r.message = "GetFirmwareVersion requires a valid device handle";
                return r;
            }
            unsigned char fwv[32]{};
            unsigned char FWInfo[256]{};
            unsigned int ret = GetQHYCCDFWVersion(handle, fwv);
            if (ret == QHYCCD_SUCCESS) {
                if ((fwv[0] >> 4) <= 9) {
                    std::sprintf(reinterpret_cast<char*>(FWInfo),
                                 "FW 20%d_%d_%d",
                                 ((fwv[0] >> 4) + 0x10),
                                 (fwv[0] & ~0xf0),
                                 fwv[1]);
                } else {
                    std::sprintf(reinterpret_cast<char*>(FWInfo),
                                 "FW 20%d_%d_%d",
                                 (fwv[0] >> 4),
                                 (fwv[0] & ~0xf0),
                                 fwv[1]);
                }
                r.success = true;
                r.message = std::string(reinterpret_cast<char*>(FWInfo));
                r.payload = std::string(reinterpret_cast<char*>(FWInfo));
            } else {
                r.success = false;
                r.message = "GetQHYCCDFWVersion failed, error code: " + std::to_string(ret);
            }
            return r;
        }

        // 3. 初始化 / 释放 SDK 资源（不依赖句柄）
        if (name == "InitSdkResource") {
            unsigned int ret = InitQHYCCDResource();
            if (ret == QHYCCD_SUCCESS) {
                m_resourceInited = true;
            }
            return makeResultFromRet("InitQHYCCDResource", ret);
        }

        if (name == "ReleaseSdkResource") {
            unsigned int ret = ReleaseQHYCCDResource();
            if (ret == QHYCCD_SUCCESS) {
                m_resourceInited = false;
            }
            return makeResultFromRet("ReleaseQHYCCDResource", ret);
        }

        // 4. 扫描相机数量
        if (name == "ScanCameras") {
            int camCount = ScanQHYCCD();
            r.success = (camCount >= 0);
            r.message = "ScanQHYCCD, camera count = " + std::to_string(camCount);
            r.payload = camCount;
            return r;
        }

        // 5. 根据索引获取 cameraId
        if (name == "GetCameraIdByIndex") {
            int index = 0;
            if (!extractParam<int>(cmd.payload, index, r, "index")) {
                return r;
            }
            char camId[32] = {0};
            unsigned int ret = GetQHYCCDId(index, camId);
            if (ret == QHYCCD_SUCCESS) {
                r.success = true;
                r.message = std::string("GetQHYCCDId success, index = ")
                            + std::to_string(index) + ", id = " + camId;
                r.payload = std::string(camId);
            } else {
                r.success = false;
                r.message = "GetQHYCCDId failed, error code: " + std::to_string(ret);
            }
            return r;
        }

        // 5-扩展. 设置读出模式（SetQHYCCDReadMode）
        if (name == "SetReadMode") {
            if (!handle) {
                r.success = false;
                r.message = "SetReadMode requires a valid device handle";
                return r;
            }
            int readMode = 0;
            if (!extractParam<int>(cmd.payload, readMode, r, "readMode")) {
                return r;
            }
            const auto t0 = std::chrono::steady_clock::now();
            Logger::Log("QHYCCD SetReadMode | start | handle=" + std::to_string((uintptr_t)handle) +
                        ", readMode=" + std::to_string(readMode),
                        LogLevel::DEBUG, DeviceType::CAMERA);
            unsigned int ret = SetQHYCCDReadMode(handle, static_cast<uint32_t>(readMode));
            const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            Logger::Log("QHYCCD SetReadMode | finish | handle=" + std::to_string((uintptr_t)handle) +
                        ", ret=" + std::to_string(ret) +
                        ", costMs=" + std::to_string(dt),
                        (ret == QHYCCD_SUCCESS) ? LogLevel::DEBUG : LogLevel::ERROR,
                        DeviceType::CAMERA);
            return makeResultFromRet("SetQHYCCDReadMode", ret);
        }

        // 6. 设置流模式（例：单帧模式）
        if (name == "SetStreamMode") {
            if (!handle) {
                r.success = false;
                r.message = "SetStreamMode requires a valid device handle";
                return r;
            }
            int mode = 0;
            if (!extractParam<int>(cmd.payload, mode, r, "mode")) {
                return r;
            }
            const auto t0 = std::chrono::steady_clock::now();
            Logger::Log("QHYCCD SetStreamMode | start | handle=" + std::to_string((uintptr_t)handle) +
                        ", mode=" + std::to_string(mode),
                        LogLevel::DEBUG, DeviceType::CAMERA);
            unsigned int ret = SetQHYCCDStreamMode(handle, mode);
            const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            Logger::Log("QHYCCD SetStreamMode | finish | handle=" + std::to_string((uintptr_t)handle) +
                        ", ret=" + std::to_string(ret) +
                        ", costMs=" + std::to_string(dt),
                        (ret == QHYCCD_SUCCESS) ? LogLevel::DEBUG : LogLevel::ERROR,
                        DeviceType::CAMERA);
            return makeResultFromRet("SetQHYCCDStreamMode", ret);
        }

        // 7. 初始化相机（InitQHYCCD）
        if (name == "InitCamera") {
            if (!handle) {
                r.success = false;
                r.message = "InitCamera requires a valid device handle";
                return r;
            }
            const auto t0 = std::chrono::steady_clock::now();
            Logger::Log("QHYCCD InitCamera | start | handle=" + std::to_string((uintptr_t)handle),
                        LogLevel::DEBUG, DeviceType::CAMERA);
            unsigned int ret = InitQHYCCD(handle);
            const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            Logger::Log("QHYCCD InitCamera | finish | handle=" + std::to_string((uintptr_t)handle) +
                        ", ret=" + std::to_string(ret) +
                        ", costMs=" + std::to_string(dt),
                        (ret == QHYCCD_SUCCESS) ? LogLevel::DEBUG : LogLevel::ERROR,
                        DeviceType::CAMERA);
            return makeResultFromRet("InitQHYCCD", ret);
        }

        // 8. 获取 OverScan 区域
        if (name == "GetOverScanArea") {
            if (!handle) {
                r.success = false;
                r.message = "GetOverScanArea requires a valid device handle";
                return r;
            }
            SdkAreaInfo info;
            unsigned int ret = GetQHYCCDOverScanArea(handle,
                                                     &info.startX,
                                                     &info.startY,
                                                     &info.sizeX,
                                                     &info.sizeY);
            r = makeResultFromRet("GetQHYCCDOverScanArea", ret);
            if (ret == QHYCCD_SUCCESS) {
                r.payload = info;
            }
            return r;
        }

        // 9. 获取有效区域（EffectiveArea）
        if (name == "GetEffectiveArea") {
            if (!handle) {
                r.success = false;
                r.message = "GetEffectiveArea requires a valid device handle";
                return r;
            }
            SdkAreaInfo info;
            unsigned int ret = GetQHYCCDEffectiveArea(handle,
                                                      &info.startX,
                                                      &info.startY,
                                                      &info.sizeX,
                                                      &info.sizeY);
            r = makeResultFromRet("GetQHYCCDEffectiveArea", ret);
            if (ret == QHYCCD_SUCCESS) {
                r.payload = info;
            }
            return r;
        }

        // 10. 获取芯片信息（ChipInfo）
        if (name == "GetChipInfo") {
            if (!handle) {
                r.success = false;
                r.message = "GetChipInfo requires a valid device handle";
                return r;
            }
            SdkChipInfo info;
            unsigned int ret = GetQHYCCDChipInfo(handle,
                                                 &info.chipWidthMM,
                                                 &info.chipHeightMM,
                                                 &info.maxImageSizeX,
                                                 &info.maxImageSizeY,
                                                 &info.pixelWidthUM,
                                                 &info.pixelHeightUM,
                                                 &info.bpp);
            r = makeResultFromRet("GetQHYCCDChipInfo", ret);
            if (ret == QHYCCD_SUCCESS) {
                r.payload = info;
            }
            return r;
        }

        // 10-扩展. DDR（CONTROL_DDR）
        if (name == "SetDDR") {
            if (!handle) {
                r.success = false;
                r.message = "SetDDR requires a valid device handle";
                return r;
            }
            double value = 0.0;
            if (!extractParam<double>(cmd.payload, value, r, "ddr")) {
                return r;
            }
            unsigned int ret = SetQHYCCDParam(handle, CONTROL_DDR, value);
            return makeResultFromRet("SetQHYCCDParam CONTROL_DDR", ret);
        }

        // 10-扩展. Debayer 开关（SetQHYCCDDebayerOnOff）
        if (name == "SetDebayerOnOff") {
            if (!handle) {
                r.success = false;
                r.message = "SetDebayerOnOff requires a valid device handle";
                return r;
            }
            bool on = false;
            if (!extractParam<bool>(cmd.payload, on, r, "debayerOn")) {
                return r;
            }
            // 部分单色机型/旧版 SDK 上直接调用 SetQHYCCDDebayerOnOff 可能触发底层崩溃。
            // 只有在明确检测到彩色相机时才下发该调用；否则视为“无需处理”的成功 no-op。
            const unsigned int colorRet = IsQHYCCDControlAvailable(handle, CAM_IS_COLOR);
            if (colorRet != QHYCCD_SUCCESS) {
                r.success = true;
                r.message = "Skip SetQHYCCDDebayerOnOff: camera is monochrome or CAM_IS_COLOR unsupported";
                return r;
            }
            unsigned int ret = SetQHYCCDDebayerOnOff(handle, on);
            return makeResultFromRet(std::string("SetQHYCCDDebayerOnOff ") + (on ? "true" : "false"), ret);
        }

        // 11. 设置 USB 传输（CONTROL_USBTRAFFIC）
        if (name == "SetUsbTraffic") {
            if (!handle) {
                r.success = false;
                r.message = "SetUsbTraffic requires a valid device handle";
                return r;
            }
            double value = 0.0;
            if (!extractParam<double>(cmd.payload, value, r, "usbTraffic")) {
                return r;
            }
            unsigned int ret = SetQHYCCDParam(handle, CONTROL_USBTRAFFIC, value);
            return makeResultFromRet("SetQHYCCDParam CONTROL_USBTRAFFIC", ret);
        }

        // 11-扩展. 获取 USBTraffic 的范围及当前值（CONTROL_USBTRAFFIC）
        if (name == "GetUsbTraffic") {
            if (!handle) {
                r.success = false;
                r.message = "GetUsbTraffic requires a valid device handle";
                return r;
            }
            double minV = 0.0, maxV = 0.0, stepV = 0.0;
            uint32_t ret = GetQHYCCDParamMinMaxStep(handle, CONTROL_USBTRAFFIC, &minV, &maxV, &stepV);
            double cur = GetQHYCCDParam(handle, CONTROL_USBTRAFFIC);
            if (ret != QHYCCD_SUCCESS || cur == QHYCCD_ERROR) {
                r.success = false;
                r.message = "GetQHYCCDParamMinMaxStep/CONTROL_USBTRAFFIC failed";
            } else {
                SdkControlParamInfo info;
                info.minValue = minV;
                info.maxValue = maxV;
                info.step     = stepV;
                info.current  = cur;
                r.success = true;
                r.payload = info;
                r.message = "USBTraffic param info acquired";
            }
            return r;
        }

        // 12. 设置增益（CONTROL_GAIN）
        if (name == "SetGain") {
            if (!handle) {
                r.success = false;
                r.message = "SetGain requires a valid device handle";
                return r;
            }
            double value = 0.0;
            if (!extractParam<double>(cmd.payload, value, r, "gain")) {
                return r;
            }
            unsigned int ret = SetQHYCCDParam(handle, CONTROL_GAIN, value);
            return makeResultFromRet("SetQHYCCDParam CONTROL_GAIN", ret);
        }

        // 12-扩展. 获取增益的范围及当前值（CONTROL_GAIN）
        if (name == "GetGain") {
            if (!handle) {
                r.success = false;
                r.message = "GetGain requires a valid device handle";
                return r;
            }
            double minV = 0.0, maxV = 0.0, stepV = 0.0;
            uint32_t ret = GetQHYCCDParamMinMaxStep(handle, CONTROL_GAIN, &minV, &maxV, &stepV);
            double cur = GetQHYCCDParam(handle, CONTROL_GAIN);
            if (ret != QHYCCD_SUCCESS || cur == QHYCCD_ERROR) {
                r.success = false;
                r.message = "GetQHYCCDParamMinMaxStep/CONTROL_GAIN failed";
            } else {
                SdkControlParamInfo info;
                info.minValue = minV;
                info.maxValue = maxV;
                info.step     = stepV;
                info.current  = cur;
                r.success = true;
                r.payload = info;
                r.message = "Gain param info acquired";
            }
            return r;
        }

        // 13. 设置偏置（CONTROL_OFFSET）
        if (name == "SetOffset") {
            if (!handle) {
                r.success = false;
                r.message = "SetOffset requires a valid device handle";
                return r;
            }
            double value = 0.0;
            if (!extractParam<double>(cmd.payload, value, r, "offset")) {
                return r;
            }
            unsigned int ret = SetQHYCCDParam(handle, CONTROL_OFFSET, value);
            return makeResultFromRet("SetQHYCCDParam CONTROL_OFFSET", ret);
        }

        // 13-扩展. 获取偏置的范围及当前值（CONTROL_OFFSET）
        if (name == "GetOffset") {
            if (!handle) {
                r.success = false;
                r.message = "GetOffset requires a valid device handle";
                return r;
            }
            double minV = 0.0, maxV = 0.0, stepV = 0.0;
            uint32_t ret = GetQHYCCDParamMinMaxStep(handle, CONTROL_OFFSET, &minV, &maxV, &stepV);
            double cur = GetQHYCCDParam(handle, CONTROL_OFFSET);
            if (ret != QHYCCD_SUCCESS || cur == QHYCCD_ERROR) {
                r.success = false;
                r.message = "GetQHYCCDParamMinMaxStep/CONTROL_OFFSET failed";
            } else {
                SdkControlParamInfo info;
                info.minValue = minV;
                info.maxValue = maxV;
                info.step     = stepV;
                info.current  = cur;
                r.success = true;
                r.payload = info;
                r.message = "Offset param info acquired";
            }
            return r;
        }

        // 14. 设置曝光时间（CONTROL_EXPOSURE，单位 us）
        if (name == "SetExposure") {
            if (!handle) {
                r.success = false;
                r.message = "SetExposure requires a valid device handle";
                return r;
            }
            double value = 0.0;
            if (!extractParam<double>(cmd.payload, value, r, "exposure")) {
                return r;
            }
            unsigned int ret = SetQHYCCDParam(handle, CONTROL_EXPOSURE, value);
            return makeResultFromRet("SetQHYCCDParam CONTROL_EXPOSURE", ret);
        }

        // 14-扩展. 获取曝光时间的范围及当前值（CONTROL_EXPOSURE）
        if (name == "GetExposure") {
            if (!handle) {
                r.success = false;
                r.message = "GetExposure requires a valid device handle";
                return r;
            }
            double minV = 0.0, maxV = 0.0, stepV = 0.0;
            uint32_t ret = GetQHYCCDParamMinMaxStep(handle, CONTROL_EXPOSURE, &minV, &maxV, &stepV);
            double cur = GetQHYCCDParam(handle, CONTROL_EXPOSURE);
            if (ret != QHYCCD_SUCCESS || cur == QHYCCD_ERROR) {
                r.success = false;
                r.message = "GetQHYCCDParamMinMaxStep/CONTROL_EXPOSURE failed";
            } else {
                SdkControlParamInfo info;
                info.minValue = minV;
                info.maxValue = maxV;
                info.step     = stepV;
                info.current  = cur;
                r.success = true;
                r.payload = info;
                r.message = "Exposure param info acquired";
            }
            return r;
        }

        // 15. 设置 ROI / 分辨率
        if (name == "SetResolution") {
            if (!handle) {
                r.success = false;
                r.message = "SetResolution requires a valid device handle";
                return r;
            }
            SdkAreaInfo roi;
            if (!extractParam<SdkAreaInfo>(cmd.payload, roi, r, "roi")) {
                return r;
            }
            unsigned int ret = SetQHYCCDResolution(handle,
                                                   roi.startX,
                                                   roi.startY,
                                                   roi.sizeX,
                                                   roi.sizeY);
            return makeResultFromRet("SetQHYCCDResolution", ret);
        }

        // 16. 设置 Bin 模式
        if (name == "SetBinMode") {
            if (!handle) {
                r.success = false;
                r.message = "SetBinMode requires a valid device handle";
                return r;
            }
            std::pair<int,int> bin;
            if (!extractParam<std::pair<int,int>>(cmd.payload, bin, r, "binMode")) {
                return r;
            }
            unsigned int ret = SetQHYCCDBinMode(handle, bin.first, bin.second);
            return makeResultFromRet("SetQHYCCDBinMode", ret);
        }

        // 17. 设置位深（BitsMode / CONTROL_TRANSFERBIT）
        if (name == "SetBitsMode") {
            if (!handle) {
                r.success = false;
                r.message = "SetBitsMode requires a valid device handle";
                return r;
            }
            int bits = 16;
            if (!extractParam<int>(cmd.payload, bits, r, "bits")) {
                return r;
            }
            // 某些机型不暴露 CONTROL_TRANSFERBIT；对这些机型跳过位深设置，
            // 避免在 SDK 内部走到不稳定分支。
            const unsigned int availRet = IsQHYCCDControlAvailable(handle, CONTROL_TRANSFERBIT);
            if (availRet == QHYCCD_ERROR) {
                r.success = true;
                r.message = "Skip SetQHYCCDBitsMode: CONTROL_TRANSFERBIT unsupported";
                return r;
            }
            unsigned int ret = SetQHYCCDBitsMode(handle, bits);
            return makeResultFromRet("SetQHYCCDBitsMode", ret);
        }

        // 17-扩展. 获取位深模式的范围及当前值（CONTROL_TRANSFERBIT）
        if (name == "GetBitsMode") {
            if (!handle) {
                r.success = false;
                r.message = "GetBitsMode requires a valid device handle";
                return r;
            }
            double minV = 0.0, maxV = 0.0, stepV = 0.0;
            uint32_t ret = GetQHYCCDParamMinMaxStep(handle, CONTROL_TRANSFERBIT, &minV, &maxV, &stepV);
            double cur = GetQHYCCDParam(handle, CONTROL_TRANSFERBIT);
            if (ret != QHYCCD_SUCCESS || cur == QHYCCD_ERROR) {
                r.success = false;
                r.message = "GetQHYCCDParamMinMaxStep/CONTROL_TRANSFERBIT failed";
            } else {
                SdkControlParamInfo info;
                info.minValue = minV;
                info.maxValue = maxV;
                info.step     = stepV;
                info.current  = cur;
                r.success = true;
                r.payload = info;
                r.message = "BitsMode param info acquired";
            }
            return r;
        }

        // 18. 单帧曝光（ExpQHYCCDSingleFrame）
        if (name == "StartSingleExposure") {
            if (!handle) {
                r.success = false;
                r.message = "StartSingleExposure requires a valid device handle";
                return r;
            }
            unsigned int ret = ExpQHYCCDSingleFrame(handle);
            // 注意：不要在驱动层 sleep/阻塞等待曝光完成。
            // 上层（Qt 定时器）会根据曝光时间进行轮询/等待，这里只负责触发曝光。
            return makeResultFromRet("ExpQHYCCDSingleFrame", ret);
        }

        // 19. 获取所需内存大小（GetQHYCCDMemLength）
        if (name == "GetMemLength") {
            if (!handle) {
                r.success = false;
                r.message = "GetMemLength requires a valid device handle";
                return r;
            }
            uint32_t length = GetQHYCCDMemLength(handle);
            r.success = (length > 0);
            r.message = "GetQHYCCDMemLength, length = " + std::to_string(length);
            r.payload = static_cast<uint32_t>(length);
            return r;
        }

        // 20. 获取单帧图像（GetQHYCCDSingleFrame，返回 SdkFrameData）
        if (name == "GetSingleFrame") {
            if (!handle) {
                r.success = false;
                r.message = "GetSingleFrame requires a valid device handle";
                return r;
            }

            // 关键修复：
            // QHY SDK 的 GetQHYCCDSingleFrame() 在部分机型/驱动版本下会阻塞等待曝光完成，
            // 如果上层“轮询定时器”在曝光未完成时调用它，会导致线程卡住（常见表现：Expected 100ms 但 Elapsed 60s+）。
            // 用 GetQHYCCDExposureRemaining() 做一次非阻塞探测：
            // - 返回值 > 100：曝光尚未结束（剩余时间），直接让上层继续轮询
            // - 返回值 <= 100：认为曝光结束，可以尝试读取帧
            //
            // 备注：qhyccd.h 注释约定 “100 or less 100,it means exposoure is over”
            uint32_t remaining = GetQHYCCDExposureRemaining(handle);
            Logger::Log("QHYCCD GetSingleFrame | exposure remaining query: handle=" +
                            std::to_string(reinterpret_cast<uintptr_t>(handle)) +
                            " remaining=" + std::to_string(remaining),
                        LogLevel::DEBUG, DeviceType::CAMERA);
            // SDK 可能用 0xFFFFFFFF 表示错误（QHYCCD_ERROR）
            if (remaining == QHYCCD_ERROR) {
                r.success = false;
                r.message = "GetQHYCCDExposureRemaining returned QHYCCD_ERROR (will retry)";
                return r;
            }
            if (remaining > 100) {
                r.success = false;
                r.message = "Exposure not finished, remaining=" + std::to_string(remaining) + "ms (will retry)";
                return r;
            }

            uint32_t length = GetQHYCCDMemLength(handle);
            Logger::Log("QHYCCD GetSingleFrame | mem length query: handle=" +
                            std::to_string(reinterpret_cast<uintptr_t>(handle)) +
                            " memLength=" + std::to_string(length),
                        LogLevel::DEBUG, DeviceType::CAMERA);
            if (length == 0) {
                r.success = false;
                r.message = "GetQHYCCDMemLength returned 0";
                return r;
            }

            // 某些机型/SDK 版本下，GetQHYCCDMemLength() 在全幅/ROI/硬件 Bin 切换后可能短暂返回过小值，
            // 直接把该长度传给 GetQHYCCDSingleFrame() 会导致 SDK 向外部缓冲区越界写，从而触发段错误。
            // 另外，若相机处于 debayer/RGB 输出，SDK 可能返回 16-bit x 3 channels。
            // 这里按“整幅 16-bit 3 通道”保守预留容量，先确保 SDK 写入安全；后续仍以返回的 roi/meta 校验有效字节。
            uint32_t safeLength = length;
            unsigned int chipRet = QHYCCD_ERROR;
            double chipW = 0.0;
            double chipH = 0.0;
            double pixelW = 0.0;
            double pixelH = 0.0;
            unsigned int chipBpp = 0;
            unsigned int maxImageX = 0;
            unsigned int maxImageY = 0;
            {
                chipRet = GetQHYCCDChipInfo(handle,
                                            &chipW,
                                            &chipH,
                                            &maxImageX,
                                            &maxImageY,
                                            &pixelW,
                                            &pixelH,
                                            &chipBpp);
                if (chipRet == QHYCCD_SUCCESS && maxImageX > 0 && maxImageY > 0) {
                    const unsigned long long fullFrameBytes =
                        static_cast<unsigned long long>(maxImageX) *
                        static_cast<unsigned long long>(maxImageY) *
                        sizeof(uint16_t) *
                        3ULL;
                    if (fullFrameBytes > static_cast<unsigned long long>(safeLength) &&
                        fullFrameBytes <= static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) {
                        safeLength = static_cast<uint32_t>(fullFrameBytes);
                    }
                }
            }
            Logger::Log("QHYCCD GetSingleFrame | chip info before read: handle=" +
                            std::to_string(reinterpret_cast<uintptr_t>(handle)) +
                            " chipRet=" + std::to_string(chipRet) +
                            " maxImage=" + std::to_string(maxImageX) + "x" + std::to_string(maxImageY) +
                            " chipMM=" + std::to_string(chipW) + "x" + std::to_string(chipH) +
                            " pixelUM=" + std::to_string(pixelW) + "x" + std::to_string(pixelH) +
                            " chipBpp=" + std::to_string(chipBpp) +
                            " memLength=" + std::to_string(length) +
                            " safeLength=" + std::to_string(safeLength),
                        LogLevel::INFO, DeviceType::CAMERA);

            auto buffer = std::make_shared<std::vector<unsigned char>>(static_cast<size_t>(safeLength));
            std::memset(buffer->data(), 0, buffer->size());
            Logger::Log("QHYCCD GetSingleFrame | about to call GetQHYCCDSingleFrame: handle=" +
                            std::to_string(reinterpret_cast<uintptr_t>(handle)) +
                            " bufferPtr=" + std::to_string(reinterpret_cast<uintptr_t>(buffer->data())) +
                            " bufferSize=" + std::to_string(buffer->size()) +
                            " remaining=" + std::to_string(remaining),
                        LogLevel::INFO, DeviceType::CAMERA);

            unsigned int roiSizeX = 0;
            unsigned int roiSizeY = 0;
            unsigned int bpp      = 0;
            unsigned int channels = 0;

            unsigned int ret = GetQHYCCDSingleFrame(handle,
                                                    &roiSizeX,
                                                    &roiSizeY,
                                                    &bpp,
                                                    &channels,
                                                    buffer->data());
            Logger::Log("QHYCCD GetSingleFrame | GetQHYCCDSingleFrame returned: ret=" +
                            std::to_string(ret) +
                            " roi=" + std::to_string(roiSizeX) + "x" + std::to_string(roiSizeY) +
                            " bpp=" + std::to_string(bpp) +
                            " channels=" + std::to_string(channels),
                        LogLevel::INFO, DeviceType::CAMERA);
            if (ret != QHYCCD_SUCCESS) {
                r.success = false;
                r.message = "GetQHYCCDSingleFrame failed, error code: " + std::to_string(ret);
                return r;
            }

            // 关键校验：某些情况下 SDK 会返回 ret=0 但 roiSizeX/roiSizeY/bpp/channels 为 0，
            // 这种帧是无效的，必须让上层继续轮询，而不是当作成功写入 FITS。
            if (roiSizeX == 0 || roiSizeY == 0 || bpp == 0 || channels == 0) {
                r.success = false;
                r.message = "GetQHYCCDSingleFrame returned invalid frame meta: "
                            "roi=" + std::to_string(roiSizeX) + "x" + std::to_string(roiSizeY) +
                            " bpp=" + std::to_string(bpp) + " channels=" + std::to_string(channels) +
                            " (will retry)";
                return r;
            }

            SdkFrameData frame;
            frame.width    = static_cast<int>(roiSizeX);
            frame.height   = static_cast<int>(roiSizeY);
            frame.bpp      = bpp;
            frame.channels = channels;

            // 当前示例只考虑 16 位单通道
            if (bpp != 16 || channels != 1) {
                r.success = false;
                r.message = "GetSingleFrame unsupported format: bpp=" + std::to_string(bpp) +
                            " channels=" + std::to_string(channels);
                return r;
            }

            const size_t pixelCount = static_cast<size_t>(roiSizeX) * static_cast<size_t>(roiSizeY);
            const size_t bytesNeeded = pixelCount * sizeof(uint16_t);
            if (bytesNeeded > buffer->size()) {
                r.success = false;
                r.message = "GetSingleFrame buffer too small: need " + std::to_string(bytesNeeded) +
                            " bytes, have " + std::to_string(buffer->size());
                return r;
            }
            frame.rawBuffer = buffer;
            frame.rawBytes  = bytesNeeded;
            Logger::Log("QHYCCD GetSingleFrame | frame prepared: bytesNeeded=" +
                            std::to_string(bytesNeeded) +
                            " rawBytes=" + std::to_string(frame.rawBytes) +
                            " width=" + std::to_string(frame.width) +
                            " height=" + std::to_string(frame.height),
                        LogLevel::DEBUG, DeviceType::CAMERA);

            r.success = true;
            r.message = "GetQHYCCDSingleFrame success";
            r.payload = frame;
            return r;
        }

        // 20-扩展. 进入 Live 模式（BeginQHYCCDLive）
        if (name == "BeginLive") {
            if (!handle) {
                r.success = false;
                r.message = "BeginLive requires a valid device handle";
                return r;
            }
            unsigned int ret = BeginQHYCCDLive(handle);
            return makeResultFromRet("BeginQHYCCDLive", ret);
        }

        // 20-扩展. 获取 Live 帧（GetQHYCCDLiveFrame，返回 SdkFrameData）
        if (name == "GetLiveFrame") {
            if (!handle) {
                r.success = false;
                r.message = "GetLiveFrame requires a valid device handle";
                return r;
            }

            uint32_t length = GetQHYCCDMemLength(handle);
            if (length == 0) {
                r.success = false;
                r.message = "GetQHYCCDMemLength returned 0";
                return r;
            }

            // 性能优化：复用缓冲区，避免每帧分配/清零大内存（行为更接近官方 demo）
            // 注意：SDK 会写满 buffer（至少写入 roi 对应的有效字节），这里不做 memset 以降低开销。
            bool exclusive = true;
            auto buffer = acquireLiveBuffer(handle, length, &exclusive);

            unsigned int roiSizeX = 0;
            unsigned int roiSizeY = 0;
            unsigned int bpp      = 0;
            unsigned int channels = 0;

            unsigned int ret = GetQHYCCDLiveFrame(handle,
                                                  &roiSizeX,
                                                  &roiSizeY,
                                                  &bpp,
                                                  &channels,
                                                  buffer->data());
            if (ret != QHYCCD_SUCCESS) {
                r.success = false;
                r.message = "GetQHYCCDLiveFrame failed, error code: " + std::to_string(ret);
                return r;
            }

            // 防御：SDK 有时返回 ret=0 但 meta 为 0，这种帧无效
            if (roiSizeX == 0 || roiSizeY == 0 || bpp == 0 || channels == 0) {
                r.success = false;
                r.message = "GetQHYCCDLiveFrame returned invalid frame meta: "
                            "roi=" + std::to_string(roiSizeX) + "x" + std::to_string(roiSizeY) +
                            " bpp=" + std::to_string(bpp) + " channels=" + std::to_string(channels);
                return r;
            }

            // Live 模式下部分机型/驱动可能返回 8-bit 单通道；这里兼容并转换为 16-bit 单通道，
            // 以复用现有 FITS/PNG 处理链路。
            if (channels != 1 || (bpp != 16 && bpp != 8)) {
                r.success = false;
                r.message = "GetLiveFrame unsupported format: bpp=" + std::to_string(bpp) +
                            " channels=" + std::to_string(channels);
                return r;
            }

            const size_t pixelCount = static_cast<size_t>(roiSizeX) * static_cast<size_t>(roiSizeY);
            const size_t bytesNeeded = pixelCount * (bpp == 16 ? sizeof(uint16_t) : sizeof(uint8_t));
            if (bytesNeeded > buffer->size()) {
                r.success = false;
                r.message = "GetLiveFrame buffer too small: need " + std::to_string(bytesNeeded) +
                            " bytes, have " + std::to_string(buffer->size());
                return r;
            }

            SdkFrameData frame;
            frame.width    = static_cast<int>(roiSizeX);
            frame.height   = static_cast<int>(roiSizeY);
            frame.bpp      = bpp;
            frame.channels = channels;

            // 零拷贝路径：仅在 buffer 不会被下一帧覆盖时启用（exclusive=true）
            // 约束：当前仅支持单通道（channels==1）的 8/16bit 原始数据直通到主线程写 FITS
            if (exclusive && channels == 1 && (bpp == 16 || bpp == 8)) {
                frame.rawBuffer = buffer;
                frame.rawBytes  = bytesNeeded;
            } else {
                // 回退拷贝路径：保证在缓冲池被占满/格式不支持时仍可稳定出图
                {
                    const long long nowMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                    long long expected = g_lastLiveCopyWarnMs.load(std::memory_order_relaxed);
                    if (nowMs - expected >= 2000) {
                        if (g_lastLiveCopyWarnMs.compare_exchange_strong(expected, nowMs, std::memory_order_relaxed)) {
                            Logger::Log(
                                std::string("QHYCCD GetLiveFrame | WARNING: fallback to COPY path (zero-copy disabled). ") +
                                    "reason=" + (exclusive ? "format_or_other" : "buffer_pool_busy") +
                                    " roi=" + std::to_string(roiSizeX) + "x" + std::to_string(roiSizeY) +
                                    " bpp=" + std::to_string(bpp) +
                                    " channels=" + std::to_string(channels) +
                                    " memLength=" + std::to_string(length) +
                                    " bytesNeeded=" + std::to_string(bytesNeeded),
                                LogLevel::WARNING, DeviceType::CAMERA);
                        }
                    }
                }
                if (channels != 1 || (bpp != 16 && bpp != 8)) {
                    r.success = false;
                    r.message = "GetLiveFrame unsupported format: bpp=" + std::to_string(bpp) +
                                " channels=" + std::to_string(channels);
                    return r;
                }
                frame.pixels.resize(pixelCount);
                frame.bpp      = 16;
                frame.channels = 1;
                if (bpp == 16) {
                    const uint16_t* src = reinterpret_cast<const uint16_t*>(buffer->data());
                    std::memcpy(frame.pixels.data(), src, pixelCount * sizeof(uint16_t));
                } else {
                    const uint8_t* src = reinterpret_cast<const uint8_t*>(buffer->data());
                    // 8->16 展开：255 映射到 65535
                    for (size_t i = 0; i < pixelCount; ++i) {
                        frame.pixels[i] = static_cast<uint16_t>(src[i]) * 257;
                    }
                }
            }

            r.success = true;
            r.message = "GetQHYCCDLiveFrame success";
            r.payload = frame;
            return r;
        }

        // 20-扩展. 获取 Live 帧（Fast）：复用缓冲区且不拷贝 pixels（仅返回 meta）
        // 适用场景：只想确认“持续出帧”，不做任何后处理/前端显示。
        if (name == "GetLiveFrameFast") {
            if (!handle) {
                r.success = false;
                r.message = "GetLiveFrameFast requires a valid device handle";
                return r;
            }

            uint32_t length = GetQHYCCDMemLength(handle);
            if (length == 0) {
                r.success = false;
                r.message = "GetQHYCCDMemLength returned 0";
                return r;
            }

            // Fast 版本不返回像素，不需要零拷贝防覆盖保证；但仍复用缓冲池降低分配开销
            auto buffer = acquireLiveBuffer(handle, length, nullptr);

            unsigned int roiSizeX = 0;
            unsigned int roiSizeY = 0;
            unsigned int bpp      = 0;
            unsigned int channels = 0;

            unsigned int ret = GetQHYCCDLiveFrame(handle,
                                                  &roiSizeX,
                                                  &roiSizeY,
                                                  &bpp,
                                                  &channels,
                                                  buffer->data());
            if (ret != QHYCCD_SUCCESS) {
                r.success = false;
                r.message = "GetQHYCCDLiveFrame failed, error code: " + std::to_string(ret);
                return r;
            }

            if (roiSizeX == 0 || roiSizeY == 0 || bpp == 0 || channels == 0) {
                r.success = false;
                r.message = "GetQHYCCDLiveFrame returned invalid frame meta: "
                            "roi=" + std::to_string(roiSizeX) + "x" + std::to_string(roiSizeY) +
                            " bpp=" + std::to_string(bpp) + " channels=" + std::to_string(channels);
                return r;
            }

            SdkFrameData frame;
            frame.width    = static_cast<int>(roiSizeX);
            frame.height   = static_cast<int>(roiSizeY);
            frame.bpp      = bpp;
            frame.channels = channels;
            // pixels intentionally empty (no copy)

            r.success = true;
            r.message = "GetQHYCCDLiveFrame success (fast, no pixels copy)";
            r.payload = frame;
            return r;
        }

        // 20-扩展. 停止 Live 模式（StopQHYCCDLive）
        if (name == "StopLive") {
            if (!handle) {
                r.success = false;
                r.message = "StopLive requires a valid device handle";
                return r;
            }
            unsigned int ret = StopQHYCCDLive(handle);
            return makeResultFromRet("StopQHYCCDLive", ret);
        }

        // 20-Burst. 开启/关闭 Burst 子模式（EnableQHYCCDBurstMode）
        if (name == "EnableBurstMode") {
            if (!handle) {
                r.success = false;
                r.message = "EnableBurstMode requires a valid device handle";
                return r;
            }
            bool enable = false;
            if (!extractParam<bool>(cmd.payload, enable, r, "enable")) {
                return r;
            }
            unsigned int ret = EnableQHYCCDBurstMode(handle, enable);
            return makeResultFromRet(std::string("EnableQHYCCDBurstMode ") + (enable ? "true" : "false"), ret);
        }

        // 20-Burst. 设置 start/end（SetQHYCCDBurstModeStartEnd）
        if (name == "SetBurstStartEnd") {
            if (!handle) {
                r.success = false;
                r.message = "SetBurstStartEnd requires a valid device handle";
                return r;
            }
            std::pair<int,int> se;
            if (!extractParam<std::pair<int,int>>(cmd.payload, se, r, "startEnd")) {
                return r;
            }
            const int start = std::max(0, se.first);
            const int end   = std::max(0, se.second);
            unsigned int ret = SetQHYCCDBurstModeStartEnd(handle,
                                                          static_cast<unsigned short>(start),
                                                          static_cast<unsigned short>(end));
            return makeResultFromRet("SetQHYCCDBurstModeStartEnd", ret);
        }

        // 20-Burst. Reset frame counter
        if (name == "ResetFrameCounter") {
            if (!handle) {
                r.success = false;
                r.message = "ResetFrameCounter requires a valid device handle";
                return r;
            }
            unsigned int ret = ResetQHYCCDFrameCounter(handle);
            return makeResultFromRet("ResetQHYCCDFrameCounter", ret);
        }

        // 20-Burst. 进入 IDLE（SetQHYCCDBurstIDLE）
        if (name == "SetBurstIDLE") {
            if (!handle) {
                r.success = false;
                r.message = "SetBurstIDLE requires a valid device handle";
                return r;
            }
            unsigned int ret = SetQHYCCDBurstIDLE(handle);
            return makeResultFromRet("SetQHYCCDBurstIDLE", ret);
        }

        // 20-Burst. 释放 IDLE（ReleaseQHYCCDBurstIDLE）
        if (name == "ReleaseBurstIDLE") {
            if (!handle) {
                r.success = false;
                r.message = "ReleaseBurstIDLE requires a valid device handle";
                return r;
            }
            unsigned int ret = ReleaseQHYCCDBurstIDLE(handle);
            return makeResultFromRet("ReleaseQHYCCDBurstIDLE", ret);
        }

        // 20-Burst. 设置补包数量（SetQHYCCDBurstModePatchNumber）
        if (name == "SetBurstPatchNumber") {
            if (!handle) {
                r.success = false;
                r.message = "SetBurstPatchNumber requires a valid device handle";
                return r;
            }
            uint32_t v = 0;
            if (!extractParam<uint32_t>(cmd.payload, v, r, "patchNumber")) {
                return r;
            }
            unsigned int ret = SetQHYCCDBurstModePatchNumber(handle, v);
            return makeResultFromRet("SetQHYCCDBurstModePatchNumber", ret);
        }

        // 21. 检测是否支持单帧模式（CAM_SINGLEFRAMEMODE）
        if (name == "CheckSingleFrameModeAvailable") {
            if (!handle) {
                r.success = false;
                r.message = "CheckSingleFrameModeAvailable requires a valid device handle";
                return r;
            }
            unsigned int ret = IsQHYCCDControlAvailable(handle, CAM_SINGLEFRAMEMODE);
            bool available = (ret != QHYCCD_ERROR);
            r.success = true;
            r.payload = available;
            r.message = available
                        ? "CAM_SINGLEFRAMEMODE is available"
                        : "CAM_SINGLEFRAMEMODE is NOT available";
            return r;
        }

        // 22. 判断彩色 / 黑白相机（CAM_IS_COLOR）
        if (name == "IsColorCamera") {
            if (!handle) {
                r.success = false;
                r.message = "IsColorCamera requires a valid device handle";
                return r;
            }
            unsigned int ret = IsQHYCCDControlAvailable(handle, CAM_IS_COLOR);
            bool isColor = (ret == QHYCCD_SUCCESS);
            r.success = true;
            r.payload = isColor;
            r.message = isColor
                        ? "Color camera detected by CAM_IS_COLOR"
                        : ("Monochrome camera or CAM_IS_COLOR not supported, ret=" + std::to_string(ret));
            return r;
        }

        // 22-扩展. 获取彩色相机 CFA（CAM_IS_COLOR -> 临时 DebayerOn -> CAM_COLOR -> 恢复 DebayerOff）
        if (name == "GetCameraCfa") {
            if (!handle) {
                r.success = false;
                r.message = "GetCameraCfa requires a valid device handle";
                return r;
            }

            unsigned int colorRet = IsQHYCCDControlAvailable(handle, CAM_IS_COLOR);
            if (colorRet != QHYCCD_SUCCESS) {
                r.success = true;
                r.payload = std::string();
                r.message = "Camera is monochrome or CAM_IS_COLOR not supported";
                return r;
            }

            unsigned int debayerRet = SetQHYCCDDebayerOnOff(handle, true);
            if (debayerRet != QHYCCD_SUCCESS) {
                r.success = false;
                r.errorCode = SdkErrorCode::OperationFailed;
                r.message = "SetQHYCCDDebayerOnOff(true) failed, error code: " + std::to_string(debayerRet);
                return r;
            }

            unsigned int bayerRet = IsQHYCCDControlAvailable(handle, CAM_COLOR);
            const std::string cfa = qhyBayerIdToCfaString(bayerRet);
            const unsigned int restoreRet = SetQHYCCDDebayerOnOff(handle, false);
            if (restoreRet != QHYCCD_SUCCESS) {
                Logger::Log("QHYCCD GetCameraCfa | failed to restore DebayerOff, error code: " +
                                std::to_string(restoreRet),
                            LogLevel::WARNING, DeviceType::CAMERA);
            }
            if (cfa.empty()) {
                r.success = false;
                r.errorCode = SdkErrorCode::OperationFailed;
                r.message = "Unknown Bayer matrix code from CAM_COLOR: " + std::to_string(bayerRet);
                return r;
            }

            r.success = true;
            r.payload = cfa;
            r.message = "CFA detected: " + cfa + " (Bayer code: " + std::to_string(bayerRet) + ")";
            return r;
        }

        // 23. 获取当前温度（CONTROL_CURTEMP）
        if (name == "GetCurrentTemperature") {
            if (!handle) {
                r.success = false;
                r.message = "GetCurrentTemperature requires a valid device handle";
                return r;
            }
            double temp = GetQHYCCDParam(handle, CONTROL_CURTEMP);
            r.success = true;
            r.payload = temp;
            r.message = "Current sensor temperature = " + std::to_string(temp) + " C";
            return r;
        }

        // 24. 设置制冷目标温度（CONTROL_COOLER）
        if (name == "SetCoolerTargetTemperature") {
            if (!handle) {
                r.success = false;
                r.message = "SetCoolerTargetTemperature requires a valid device handle";
                return r;
            }
            double target = 0.0;
            if (!extractParam<double>(cmd.payload, target, r, "targetTemperature")) {
                return r;
            }
            unsigned int ret = SetQHYCCDParam(handle, CONTROL_COOLER, target);
            return makeResultFromRet("SetQHYCCDParam CONTROL_COOLER", ret);
        }

        // 24-扩展. 获取制冷目标温度的范围及当前值（CONTROL_COOLER）
        if (name == "GetCoolerTargetTemperature") {
            if (!handle) {
                r.success = false;
                r.message = "GetCoolerTargetTemperature requires a valid device handle";
                return r;
            }
            double minV = 0.0, maxV = 0.0, stepV = 0.0;
            uint32_t ret = GetQHYCCDParamMinMaxStep(handle, CONTROL_COOLER, &minV, &maxV, &stepV);
            double cur = GetQHYCCDParam(handle, CONTROL_COOLER);
            if (ret != QHYCCD_SUCCESS || cur == QHYCCD_ERROR) {
                r.success = false;
                r.message = "GetQHYCCDParamMinMaxStep/CONTROL_COOLER failed";
            } else {
                SdkControlParamInfo info;
                info.minValue = minV;
                info.maxValue = maxV;
                info.step     = stepV;
                info.current  = cur;
                r.success = true;
                r.payload = info;
                r.message = "Cooler target temperature param info acquired";
            }
            return r;
        }

        // 25. 获取当前制冷功率（CONTROL_CURPWM）
        if (name == "GetCoolerPower") {
            if (!handle) {
                r.success = false;
                r.message = "GetCoolerPower requires a valid device handle";
                return r;
            }
            double power = GetQHYCCDParam(handle, CONTROL_CURPWM);
            r.success = true;
            r.payload = power;
            r.message = "Current cooler power (PWM) = " + std::to_string(power);
            return r;
        }

        // 26. 取消当前曝光与读出
        if (name == "CancelExposure") {
            if (!handle) {
                r.success = false;
                r.message = "CancelExposure requires a valid device handle";
                return r;
            }
            unsigned int ret = CancelQHYCCDExposingAndReadout(handle);
            return makeResultFromRet("CancelQHYCCDExposingAndReadout", ret);
        }

        // 27. 检测滤镜轮（CFW）是否连接
        if (name == "IsCFWPlugged") {
            if (!handle) {
                r.success = false;
                r.message = "IsCFWPlugged requires a valid device handle";
                return r;
            }
            unsigned int ret = IsQHYCCDCFWPlugged(handle);
            bool plugged = (ret == QHYCCD_SUCCESS);
            r.success = true;
            r.payload = plugged;
            r.message = plugged ? "CFW is plugged" : "CFW is not plugged";
            return r;
        }

        // 28. 获取滤镜轮槽位数量
        if (name == "GetCFWSlotsNum") {
            if (!handle) {
                r.success = false;
                r.message = "GetCFWSlotsNum requires a valid device handle";
                return r;
            }
            double slotsNum = GetQHYCCDParam(handle, CONTROL_CFWSLOTSNUM);
            if (slotsNum == QHYCCD_ERROR) {
                r.success = false;
                r.message = "GetQHYCCDParam CONTROL_CFWSLOTSNUM failed";
            } else {
                r.success = true;
                r.payload = static_cast<int>(slotsNum);
                r.message = "CFW slots number = " + std::to_string(static_cast<int>(slotsNum));
            }
            return r;
        }

        // 29. 获取当前滤镜轮位置
        if (name == "GetCFWPosition") {
            if (!handle) {
                r.success = false;
                r.message = "GetCFWPosition requires a valid device handle";
                return r;
            }
            double pos = GetQHYCCDParam(handle, CONTROL_CFWPORT);
            if (pos == QHYCCD_ERROR) {
                r.success = false;
                r.message = "GetQHYCCDParam CONTROL_CFWPORT failed";
            } else {
                // SDK 返回的位置是 ASCII 字符码（'0' = 48），需要转换为数字索引
                int position = static_cast<int>(pos) - 48;
                r.success = true;
                r.payload = position;
                r.message = "Current CFW position = " + std::to_string(position);
            }
            return r;
        }

        // 30. 设置滤镜轮位置
        if (name == "SetCFWPosition") {
            if (!handle) {
                r.success = false;
                r.message = "SetCFWPosition requires a valid device handle";
                return r;
            }
            int position = 0;
            if (!extractParam<int>(cmd.payload, position, r, "position")) {
                return r;
            }
            // SDK 需要 ASCII 字符码（'0' = 48），将数字索引转换为字符码
            int asciiPos = position + 48;
            unsigned int ret = SetQHYCCDParam(handle, CONTROL_CFWPORT, static_cast<double>(asciiPos));
            if (ret == QHYCCD_SUCCESS) {
                r.success = true;
                r.message = "SetQHYCCDParam CONTROL_CFWPORT success, position set to " + std::to_string(position);
            } else {
                r.success = false;
                r.message = "SetQHYCCDParam CONTROL_CFWPORT failed, error code: " + std::to_string(ret);
            }
            return r;
        }

        // 31. 发送自定义命令到滤镜轮
        if (name == "SendOrderToCFW") {
            if (!handle) {
                r.success = false;
                r.message = "SendOrderToCFW requires a valid device handle";
                return r;
            }
            std::string order;
            if (!extractParam<std::string>(cmd.payload, order, r, "order")) {
                return r;
            }
            // 创建可修改的字符串缓冲区
            std::vector<char> orderBuf(order.begin(), order.end());
            orderBuf.push_back('\0');
            unsigned int ret = SendOrder2QHYCCDCFW(handle, orderBuf.data(), static_cast<uint32_t>(order.length()));
            if (ret == QHYCCD_SUCCESS) {
                r.success = true;
                r.message = "SendOrder2QHYCCDCFW success";
            } else {
                r.success = false;
                r.message = "SendOrder2QHYCCDCFW failed, error code: " + std::to_string(ret);
            }
            return r;
        }

        // 32. 获取滤镜轮状态
        if (name == "GetCFWStatus") {
            if (!handle) {
                r.success = false;
                r.message = "GetCFWStatus requires a valid device handle";
                return r;
            }
            char status[64] = {0};
            unsigned int ret = GetQHYCCDCFWStatus(handle, status);
            if (ret == QHYCCD_SUCCESS) {
                r.success = true;
                r.payload = std::string(status);
                r.message = "CFW status: " + std::string(status);
            } else {
                r.success = false;
                r.message = "GetQHYCCDCFWStatus failed, error code: " + std::to_string(ret);
            }
            return r;
        }
    }

    // 默认：不支持的命令
    r.success = false;
    r.errorCode = SdkErrorCode::NotImplemented;
    r.message = "Unsupported QHY command: " + cmd.name;
    return r;
}

/**
 * @brief 扫描可用设备（实现ISdkDriver的可选接口）
 * @param outDevices 输出参数，扫描到的设备列表
 * @return 执行结果
 * 
 * 扫描所有连接的QHYCCD相机，返回设备信息列表。
 * 设备信息包含cameraId、描述等，可用于后续打开设备。
 */
SdkResult QhyCameraDriver::scanDevices(std::vector<SdkDeviceInfo>& outDevices)
{
    SdkResult r;
    outDevices.clear();

    // 1. 确保SDK资源已初始化
    if (!m_resourceInited) {
        unsigned int ret = InitQHYCCDResource();
        if (ret != QHYCCD_SUCCESS) {
            r.success = false;
            r.errorCode = SdkErrorCode::OperationFailed;
            r.message = "InitQHYCCDResource failed, error code: " + std::to_string(ret);
            return r;
        }
        m_resourceInited = true;
    }

    // 2. 扫描相机数量
    int camCount = ScanQHYCCD();
    if (camCount < 0) {
        r.success = false;
        r.errorCode = SdkErrorCode::OperationFailed;
        r.message = "ScanQHYCCD failed, returned: " + std::to_string(camCount);
        return r;
    }

    if (camCount == 0) {
        r.success = true;
        r.errorCode = SdkErrorCode::Success;
        r.message = "No QHYCCD cameras found";
        return r;
    }

    // 3. 获取每个相机的ID并构建设备信息
    for (int idx = 0; idx < camCount; ++idx) {
        char camId[32] = {0};
        unsigned int ret = GetQHYCCDId(idx, camId);
        if (ret == QHYCCD_SUCCESS) {
            SdkDeviceInfo info;
            info.deviceId = std::string(camId);
            info.driverName = SDK_DRIVER_NAME_QHYCCD;
            info.description = "QHYCCD Camera " + std::string(camId);
            info.state = SdkDeviceState::Closed;  // 扫描时设备尚未打开
            info.handle = nullptr;  // 扫描时没有句柄
            
            outDevices.push_back(info);
        }
    }

    r.success = true;
    r.errorCode = SdkErrorCode::Success;
    r.message = "Scanned " + std::to_string(outDevices.size()) + " QHYCCD camera(s)";
    return r;
}

/**
 * @brief 获取驱动能力描述
 * @return 驱动能力描述
 * 
 * 返回QHYCCD驱动支持的功能和能力信息。
 */
SdkDeviceCapabilities QhyCameraDriver::capabilities() const
{
    SdkDeviceCapabilities caps;
    
    // 设置设备类型为相机
    caps.deviceType = SdkDeviceType::Camera;
    
    // 获取所有支持的命令名称
    auto cmdList = commandList();
    caps.supportedCommands.reserve(cmdList.size());
    for (const auto& cmd : cmdList) {
        caps.supportedCommands.push_back(cmd.name);
    }
    
    // QHYCCD支持多设备（可以同时打开多个相机）
    caps.supportsMultiDevice = true;
    
    // 可以添加其他能力信息，如参数范围等
    // caps.parameters["maxCameras"] = 10;  // 示例
    
    return caps;
}
