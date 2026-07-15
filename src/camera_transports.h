#pragma once
// ICamera 的两种传输实现：IndiCamera（包裹 MyClient+INDI::BaseDevice）
// 与 SdkCamera（包裹 per-role SdkDeviceHandle）。见 icamera.h 与设计文档。
// P0：仅 chipInfo()/abort() 实做打通，其余为占位（P1/P2 逐个迁移填充）。
#include "icamera.h"
#include "sdks/SdkCommon.h"   // SdkDeviceHandle / SdkCommand / SdkResult / SdkChipInfo

class MyClient;
namespace INDI { class BaseDevice; }

namespace camtrans {

// —— INDI 实现 ——
class IndiCamera : public ICamera {
public:
    IndiCamera(MyClient* client, INDI::BaseDevice* dp) : client_(client), dp_(dp) {}

    bool     isConnected() const override { return client_ != nullptr && dp_ != nullptr; }
    void     disconnect() override;
    ChipInfo chipInfo() override;
    bool     isColor() override;
    void     setRoiFull() override;
    void     setRoi(int x, int y, int w, int h) override;
    void     setBin(int bx, int by) override;
    void     setGain(int v) override;
    int      gain() override;
    void     setOffset(int v) override;
    int      offset() override;
    void     setUsbTraffic(int v) override;
    void     setTargetTemperature(double celsius) override;
    double   temperature() override;
    void     startExposure(double seconds) override;
    void     abort() override;
    void     beginLive() override;
    void     stopLive() override;

    INDI::BaseDevice* device() const { return dp_; }

private:
    MyClient*         client_ = nullptr;
    INDI::BaseDevice* dp_     = nullptr;
};

// —— SDK 实现 ——
class SdkCamera : public ICamera {
public:
    explicit SdkCamera(SdkDeviceHandle handle) : handle_(handle) {}

    bool     isConnected() const override { return handle_ != nullptr; }
    void     disconnect() override;
    ChipInfo chipInfo() override;
    bool     isColor() override;
    void     setRoiFull() override;
    void     setRoi(int x, int y, int w, int h) override;
    void     setBin(int bx, int by) override;
    void     setGain(int v) override;
    int      gain() override;
    void     setOffset(int v) override;
    int      offset() override;
    void     setUsbTraffic(int v) override;
    void     setTargetTemperature(double celsius) override;
    double   temperature() override;
    void     startExposure(double seconds) override;
    void     abort() override;
    void     beginLive() override;
    void     stopLive() override;

    SdkDeviceHandle handle() const { return handle_; }

private:
    SdkDeviceHandle handle_ = nullptr;
};

} // namespace camtrans
