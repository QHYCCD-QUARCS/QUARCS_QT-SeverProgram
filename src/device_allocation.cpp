#include "device_allocation.h"
#include <algorithm>

namespace devalloc {

Plan planCameraAllocation(QVector<Candidate> pool, Request req)
{
    Plan plan;
    QVector<int> used;                 // 已绑定的候选 index
    auto isUsed = [&](int idx) { return used.contains(idx); };
    bool needMain   = req.requestMain;
    bool needGuider = req.requestGuider;

    // 1) 历史回绑优先（按 savedRole）。历史命中不被后续兜底覆盖。
    for (const auto& c : pool) {
        if (needMain && c.savedRole == QLatin1String("MainCamera")) {
            plan.bind.push_back({Role::MainCamera, c.index}); used.push_back(c.index); needMain = false;
        } else if (needGuider && c.savedRole == QLatin1String("Guider")) {
            plan.bind.push_back({Role::Guider, c.index}); used.push_back(c.index); needGuider = false;
        }
    }

    // 2) QHY 双角色兜底（仅在 QHY 场景、尚有请求角色未满足时）。
    if (req.isQhyDualRole && (needMain || needGuider)) {
        QVector<Candidate> rem;
        for (const auto& c : pool) if (!isUsed(c.index)) rem.push_back(c);

        if (!rem.isEmpty()) {
            // 排序：5III 优先；同类按分辨率高->低；再按 index 稳定。
            std::sort(rem.begin(), rem.end(), [](const Candidate& a, const Candidate& b) {
                if (a.is5III != b.is5III) return a.is5III && !b.is5III;
                if (a.pixelCount != b.pixelCount) return a.pixelCount > b.pixelCount;
                return a.index < b.index;
            });

            int mainIdx = -1, guiderIdx = -1;
            if (needMain && needGuider && rem.size() >= 2) {
                QVector<Candidate> five, non;
                for (const auto& c : rem) (c.is5III ? five : non).push_back(c);
                if (!five.isEmpty() && !non.isEmpty()) {
                    guiderIdx = five.front().index;      // 5III -> Guider
                    mainIdx   = non.front().index;       // 非5III -> Main
                } else {
                    mainIdx   = rem[0].index;            // 同系列：高分辨率 -> Main
                    guiderIdx = rem[1].index;            //          低分辨率 -> Guider
                }
            } else if (needMain) {
                int pick = -1;
                for (const auto& c : rem) if (!c.is5III) { pick = c.index; break; }   // 优先非5III
                if (pick < 0) pick = rem.front().index;                               // 否则分辨率最高
                mainIdx = pick;
            } else if (needGuider) {
                int pick = -1;
                for (const auto& c : rem) if (c.is5III) { pick = c.index; break; }    // 优先5III
                if (pick < 0) pick = rem.back().index;                                // 否则分辨率最低
                guiderIdx = pick;
            }

            if (mainIdx >= 0)   { plan.bind.push_back({Role::MainCamera, mainIdx}); used.push_back(mainIdx); needMain = false; }
            if (guiderIdx >= 0) { plan.bind.push_back({Role::Guider, guiderIdx}); used.push_back(guiderIdx); needGuider = false; }
        }
    }

    // 3) 不变量：仍有请求角色未满足时，把未绑定候选上报待分配（去重、保序）。
    //    这一步替代旧代码里“单角色请求直接 continue、不上报”的静默丢弃路径，
    //    从结构上消除双 QHY 静默卡死：宁可弹分配窗让用户选，也不无声挂起。
    if (needMain || needGuider) {
        for (const auto& c : pool) {
            if (isUsed(c.index)) continue;
            if (!plan.toAllocate.contains(c.index)) plan.toAllocate.push_back(c.index);
        }
    }

    return plan;
}

} // namespace devalloc
