#pragma once
// 纯设备（相机）分配决策——无副作用，不依赖 INDI/SDK/MainWindow/句柄。
// 目的：把历史上散落在 ConnectDriver（单连接）与 continueConnectAllDeviceOnce
// （全连接）两处、逐字重复的 MainCamera/Guider 自动分配规则收敛到一处，
// 便于离线穷举测试（见 src/tests/alloc_test.cpp）与从结构上消除
// 2026-07-15 记录的“双 QHY 静默卡死”类 bug（被请求角色的候选被静默丢弃）。
#include <QString>
#include <QVector>
#include <QPair>

namespace devalloc {

enum class Role { MainCamera, Guider };

// 一台候选相机的最小决策属性。index 为不透明句柄下标（INDI 列表或 SDK 池）。
struct Candidate {
    int       index = -1;
    QString   name;
    bool      is5III = false;      // 名称含 "5III"（导星相机族）
    long long pixelCount = -1;     // maxX*maxY；未知记 -1（排序时视为最低优先级）
    QString   savedRole;           // 历史回绑：该设备名匹配到的角色（"MainCamera"/"Guider"/""）
};

// 本次分配请求：哪些角色需要求解，以及是否处于可自动兜底的 QHY 双角色场景。
struct Request {
    bool requestMain   = true;
    bool requestGuider = true;
    bool isQhyDualRole = false;
};

struct Plan {
    QVector<QPair<Role,int>> bind;        // 角色 -> 候选 index（自动绑定）
    QVector<int>             toAllocate;  // 需用户手动分配的候选 index（去重、保序）

    bool binds(Role r) const { for (const auto& p : bind) if (p.first == r) return true; return false; }
    int  boundIndex(Role r) const { for (const auto& p : bind) if (p.first == r) return p.second; return -1; }
};

// 决策优先级：
//   1) 历史回绑（savedRole）——最高，不被兜底覆盖；
//   2) QHY 双角色兜底（§3 规则：5III->Guider / 非5III->Main；同系列按分辨率高低）；
//   3) 不变量：被请求但仍未自动绑定的角色，其余候选必进 toAllocate，不得静默丢弃。
//      （单角色请求下若该角色已绑定，则不上报无关相机——避免误弹分配窗。）
Plan planCameraAllocation(QVector<Candidate> pool, Request req);

} // namespace devalloc
