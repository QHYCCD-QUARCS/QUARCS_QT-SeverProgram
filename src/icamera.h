#pragma once
// 相机行为接口（语义层）：屏蔽 INDI 异步回调 vs SDK 工作线程轮询的机制差异。
// 目标：把散落 ~560 处的 if(isSDKConnect){SDK}else{INDI} 收敛为“每角色持一个 ICamera，
// 建对象时判一次传输”。设计见 doc/2026-07-16_相机传输抽象设计_INDI_SDK统一.md。
// 迁移分期：P0 骨架(本提交) / P1 同步行为 / P2 曝光取帧 / P3 生命周期切换 / P4 清扫。
#include <functional>
#include <cstddef>
#include <cstdint>

namespace camtrans {

struct ChipInfo {
    int    maxX = 0, maxY = 0, bitDepth = 0;
    double pixelSize = 0.0, pixelX = 0.0, pixelY = 0.0;
};

struct FrameData {
    // P2 填充：像素缓冲 + 元数据。此处为占位契约。
    const uint8_t* data = nullptr;
    std::size_t    bytes = 0;
    int            width = 0, height = 0, bitDepth = 0;
    std::uint64_t  seq = 0;
};

// 每角色（MainCamera/Guider/PoleCamera）持有一个实现：IndiCamera 或 SdkCamera。
// 所有相机行为通过本接口调用，分派只发生在“建哪个实现”那一次。
class ICamera {
public:
    virtual ~ICamera() = default;

    // 生命周期（P3 落地）
    virtual bool isConnected() const = 0;
    virtual void disconnect() = 0;

    // 只读/配置——同步行为（P1 迁移）
    virtual ChipInfo chipInfo() = 0;
    virtual bool     isColor() = 0;
    virtual void     setRoiFull() = 0;
    virtual void     setRoi(int x, int y, int w, int h) = 0;
    virtual void     setBin(int bx, int by) = 0;
    virtual void     setGain(int v) = 0;
    virtual int      gain() = 0;
    virtual void     setOffset(int v) = 0;
    virtual int      offset() = 0;
    virtual void     setUsbTraffic(int v) = 0;
    virtual void     setTargetTemperature(double celsius) = 0;
    virtual double   temperature() = 0;

    // 曝光/取帧（P2 迁移；取帧统一走 onFrame，屏蔽回调/轮询差异）
    virtual void startExposure(double seconds) = 0;
    virtual void abort() = 0;
    virtual void beginLive() = 0;
    virtual void stopLive() = 0;

    // 帧就绪统一出口：INDI 实现把 blob handler 接到这里；SDK 实现把定时器轮询结果接到这里。
    std::function<void(const FrameData&)> onFrame;
};

} // namespace camtrans
