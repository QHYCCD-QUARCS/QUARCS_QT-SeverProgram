#include "camera_transports.h"
#include "myclient.h"
#include "sdks/SdkManager.h"

#include <any>

namespace camtrans {

// 小工具：构造一个 Custom SDK 命令。
static SdkCommand makeSdkCmd(const char* name, std::any payload = std::any()) {
    SdkCommand cmd;
    cmd.type    = SdkCommandType::Custom;
    cmd.name    = name;
    cmd.payload = std::move(payload);
    return cmd;
}

// ======================= IndiCamera =======================
// P0：chipInfo()/abort() 实做；其余占位，待 P1/P2 逐个迁移。

ChipInfo IndiCamera::chipInfo() {
    ChipInfo ci;
    if (client_ && dp_) {
        int maxX = 0, maxY = 0, bitDepth = 0;
        double pixelsize = 0.0, pixelsizX = 0.0, pixelsizY = 0.0;
        if (client_->getCCDBasicInfo(dp_, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth) == 0) {
            ci.maxX = maxX; ci.maxY = maxY; ci.bitDepth = bitDepth;
            ci.pixelSize = pixelsize; ci.pixelX = pixelsizX; ci.pixelY = pixelsizY;
        }
    }
    return ci;
}

void IndiCamera::abort() {
    if (client_ && dp_)
        client_->setCCDAbortExposure(dp_);
}

void   IndiCamera::disconnect()                 { /* TODO(P3) */ }
bool   IndiCamera::isColor()                    { return false; /* TODO(P1) getCCDCFA */ }
void   IndiCamera::setRoiFull()                 { /* TODO(P1) */ }
void   IndiCamera::setRoi(int, int, int, int)   { /* TODO(P1) setCCDFrameInfo */ }
void   IndiCamera::setBin(int, int)             { /* TODO(P1) setCCDBinnign */ }
void   IndiCamera::setGain(int)                 { /* TODO(P1) setCCDGain */ }
int    IndiCamera::gain()                       { return 0; /* TODO(P1) getCCDGain */ }
void   IndiCamera::setOffset(int)               { /* TODO(P1) setCCDOffset */ }
int    IndiCamera::offset()                     { return 0; /* TODO(P1) getCCDOffset */ }
void   IndiCamera::setUsbTraffic(int)           { /* TODO(P1) setCCDUsbTraffic */ }
void   IndiCamera::setTargetTemperature(double) { /* TODO(P1) setTemperature */ }
double IndiCamera::temperature()                { return 0.0; /* TODO(P1) getTemperature */ }
void   IndiCamera::startExposure(double)        { /* TODO(P2) takeExposure + blob->onFrame */ }
void   IndiCamera::beginLive()                  { /* TODO(P2) */ }
void   IndiCamera::stopLive()                   { /* TODO(P2) */ }

// ======================= SdkCamera =======================

ChipInfo SdkCamera::chipInfo() {
    ChipInfo ci;
    if (!handle_) return ci;
    SdkResult r = SdkManager::instance().callByHandle(handle_, makeSdkCmd("GetChipInfo"));
    if (r.success && r.payload.has_value()) {
        try {
            SdkChipInfo s = std::any_cast<SdkChipInfo>(r.payload);
            ci.maxX = static_cast<int>(s.maxImageSizeX);
            ci.maxY = static_cast<int>(s.maxImageSizeY);
            ci.bitDepth  = static_cast<int>(s.bpp);
            ci.pixelSize = s.pixelWidthUM;
            ci.pixelX    = s.pixelWidthUM;
            ci.pixelY    = s.pixelHeightUM;
        } catch (const std::bad_any_cast&) { /* 解析失败：返回默认 */ }
    }
    return ci;
}

void SdkCamera::abort() {
    if (handle_)
        SdkManager::instance().callByHandle(handle_, makeSdkCmd("CancelExposure"));
}

void   SdkCamera::disconnect()                 { /* TODO(P3) 关句柄/停 executor/定时器 */ }
bool   SdkCamera::isColor()                    { return false; /* TODO(P1) IsColorCamera */ }
void   SdkCamera::setRoiFull()                 { /* TODO(P1) SetResolution(full) */ }
void   SdkCamera::setRoi(int, int, int, int)   { /* TODO(P1) SetResolution */ }
void   SdkCamera::setBin(int, int)             { /* TODO(P1) SetBinMode */ }
void   SdkCamera::setGain(int)                 { /* TODO(P1) SetGain */ }
int    SdkCamera::gain()                       { return 0; /* TODO(P1) GetGain */ }
void   SdkCamera::setOffset(int)               { /* TODO(P1) SetOffset */ }
int    SdkCamera::offset()                     { return 0; /* TODO(P1) GetOffset */ }
void   SdkCamera::setUsbTraffic(int)           { /* TODO(P1) SetUsbTraffic */ }
void   SdkCamera::setTargetTemperature(double) { /* TODO(P1) SetCoolerTargetTemperature */ }
double SdkCamera::temperature()                { return 0.0; /* TODO(P1) GetTemperature */ }
void   SdkCamera::startExposure(double)        { /* TODO(P2) StartSingleExposure + poll->onFrame */ }
void   SdkCamera::beginLive()                  { /* TODO(P2) BeginLive */ }
void   SdkCamera::stopLive()                   { /* TODO(P2) StopLive */ }

} // namespace camtrans
