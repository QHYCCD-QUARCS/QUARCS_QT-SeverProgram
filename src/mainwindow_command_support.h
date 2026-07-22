#ifndef MAINWINDOW_COMMAND_SUPPORT_H
#define MAINWINDOW_COMMAND_SUPPORT_H

#include "mainwindow.h"
#include "quarcs_build_version.h"
#include "sdks/SdkDriverRegistry.h"

#include <sys/mman.h>

extern INDI::BaseDevice *dpMount;
extern INDI::BaseDevice *dpGuider;
extern INDI::BaseDevice *dpPoleScope;
extern INDI::BaseDevice *dpMainCamera;
extern INDI::BaseDevice *dpFocuser;
extern INDI::BaseDevice *dpCFW;

extern SdkDeviceHandle sdkMountHandle;
extern SdkDeviceHandle sdkGuiderHandle;
extern SdkDeviceHandle sdkPoleScopeHandle;
extern SdkDeviceHandle sdkMainCameraHandle;
extern SdkDeviceHandle sdkFocuserHandle;
extern SdkDeviceHandle sdkCFWHandle;
extern SdkDeviceHandle sdkCAAHandle;
extern QString sdkFocuserPort;
extern QVector<SdkDeviceHandle> g_sdkQhyCamHandles;
extern QVector<QString> g_sdkQhyCamIds;
extern int g_sdkMainCameraPoolIndex;
extern int g_sdkGuiderPoolIndex;
extern int g_sdkPoleCameraPoolIndex;

extern DriversList drivers_list;
extern std::vector<DevGroup> dev_groups;
extern std::vector<Device> devices;
extern DriversListNew drivers_list_new;
extern SystemDevice systemdevice;
extern SystemDeviceList systemdevicelist;
extern QUrl websocketUrl;

extern int LoopCaptureNum;
extern int LoopCaptureBurstFrames;

inline bool isPoleMasterName(const QString &name)
{
    return name.contains("POLEMASTER", Qt::CaseInsensitive);
}

inline int sdkUiIndexFromPoolIndex(int poolIndex)
{
    return -(poolIndex + 1);
}

inline constexpr int SDK_FOCUSER_UI_INDEX = -10001;
inline constexpr int SDK_CAA_UI_INDEX = -10002;
inline constexpr int kIndiFocuserRelMoveChunkMax = 10000;

inline int sdkPoolIndexFromUiIndex(int uiIndex)
{
    return -uiIndex - 1;
}

inline bool sdkPoolIndexValid(int poolIndex)
{
    return poolIndex >= 0 &&
           poolIndex < g_sdkQhyCamHandles.size() &&
           poolIndex < g_sdkQhyCamIds.size() &&
           !g_sdkQhyCamIds[poolIndex].isEmpty();
}

template <typename Func>
inline void postGuiderCore(GuiderCore *core, Func &&func)
{
    if (!core)
        return;
    QMetaObject::invokeMethod(core,
                              [core, fn = std::forward<Func>(func)]() mutable {
                                  fn(core);
                              },
                              Qt::QueuedConnection);
}

inline const char *polarRoleName(MainWindow::PolarAlignmentCameraRole role)
{
    switch (role)
    {
    case MainWindow::PolarAlignmentCameraRole::Guider:
        return "Guider";
    case MainWindow::PolarAlignmentCameraRole::PoleCamera:
        return "PoleCamera";
    case MainWindow::PolarAlignmentCameraRole::MainCamera:
    default:
        return "MainCamera";
    }
}

inline int toSdkCfwPos0(int pos1Based)
{
    return std::max(0, pos1Based - 1);
}

inline int toUiCfwPos1(int pos0Based)
{
    return pos0Based + 1;
}

inline QString sdkCfwStorageKey(const QString &cameraId)
{
    if (!cameraId.isEmpty())
        return "SDK_CFW_" + cameraId;
    return "SDK_CFW_MainCamera";
}

inline QString sdkCaaDisplayName(const QString &cameraId)
{
    if (!cameraId.isEmpty())
        return "CAA (on camera) - " + cameraId;
    return "CAA (on camera)";
}

inline bool sdkGetCaaRotator(SdkDeviceHandle handle, SdkControlParamInfo &info, std::string *errMsg = nullptr)
{
    SdkCommand cmd;
    cmd.type = SdkCommandType::Custom;
    cmd.name = "GetCAARotator";
    cmd.payload = std::any();

    SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
    if (!res.success || !res.payload.has_value())
    {
        if (errMsg)
            *errMsg = res.message;
        return false;
    }

    try
    {
        info = std::any_cast<SdkControlParamInfo>(res.payload);
        return true;
    }
    catch (const std::bad_any_cast &)
    {
        if (errMsg)
            *errMsg = "GetCAARotator bad_any_cast";
        return false;
    }
}

inline bool sdkSetCaaRotator(SdkDeviceHandle handle, double angle, std::string *errMsg = nullptr)
{
    SdkCommand cmd;
    cmd.type = SdkCommandType::Custom;
    cmd.name = "SetCAARotator";
    cmd.payload = angle;

    SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
    if (!res.success)
    {
        if (errMsg)
            *errMsg = res.message;
        return false;
    }

    return true;
}

inline bool sdkGetCfwSlotsNum(SdkDeviceHandle handle, int &slotsNum, std::string *errMsg = nullptr)
{
    SdkCommand cmd;
    cmd.type = SdkCommandType::Custom;
    cmd.name = "GetCFWSlotsNum";
    cmd.payload = std::any();

    SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
    if (!res.success || !res.payload.has_value())
    {
        if (errMsg)
            *errMsg = res.message;
        return false;
    }

    try
    {
        slotsNum = std::any_cast<int>(res.payload);
        return true;
    }
    catch (const std::bad_any_cast &)
    {
        if (errMsg)
            *errMsg = "GetCFWSlotsNum bad_any_cast";
        return false;
    }
}

inline bool sdkGetCfwPosition0(SdkDeviceHandle handle, int &pos0, std::string *errMsg = nullptr)
{
    SdkCommand cmd;
    cmd.type = SdkCommandType::Custom;
    cmd.name = "GetCFWPosition";
    cmd.payload = std::any();

    SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
    if (!res.success || !res.payload.has_value())
    {
        if (errMsg)
            *errMsg = res.message;
        return false;
    }

    try
    {
        pos0 = std::any_cast<int>(res.payload);
        return true;
    }
    catch (const std::bad_any_cast &)
    {
        if (errMsg)
            *errMsg = "GetCFWPosition bad_any_cast";
        return false;
    }
}

inline bool sdkSetCfwPosition0AndWait(SdkDeviceHandle handle, int targetPos0, int timeoutMs, std::string *errMsg = nullptr)
{
    {
        SdkCommand cmd;
        cmd.type = SdkCommandType::Custom;
        cmd.name = "SetCFWPosition";
        cmd.payload = targetPos0;

        SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
        if (!res.success)
        {
            if (errMsg)
                *errMsg = res.message;
            return false;
        }
    }

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs)
    {
        int cur0 = -1;
        if (sdkGetCfwPosition0(handle, cur0, errMsg) && cur0 == targetPos0)
            return true;
        QThread::msleep(200);
    }

    if (errMsg)
        *errMsg = "SetCFWPosition timeout";
    return false;
}

inline bool indiSetCfwPosition1AndWait(MyClient *client,
                                       INDI::BaseDevice *dp,
                                       int targetPos1,
                                       int timeoutMs,
                                       std::string *errMsg = nullptr)
{
    if (!client || !dp)
    {
        if (errMsg)
            *errMsg = "device_null";
        return false;
    }
    if (!dp->isConnected())
    {
        if (errMsg)
            *errMsg = "device_disconnected";
        return false;
    }

    const uint32_t ret = client->setCFWPosition(dp, targetPos1);
    if (ret != QHYCCD_SUCCESS)
    {
        if (errMsg)
            *errMsg = "indi_error";
        return false;
    }

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs)
    {
        if (!dp->isConnected())
        {
            if (errMsg)
                *errMsg = "device_disconnected";
            return false;
        }
        int pos = -1, min = 0, max = 0;
        client->getCFWPosition(dp, pos, min, max);
        if (pos == targetPos1)
            return true;
        QThread::msleep(200);
    }

    if (errMsg)
        *errMsg = "timeout";
    return false;
}

#endif
