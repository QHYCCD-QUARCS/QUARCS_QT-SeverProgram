// 纯分配决策的独立离线测试（方案 A：自带 main + 断言，不引入 gtest）。
// 编译：见文件末尾注释；无需 INDI/SDK/硬件/GUI。
#include "../device_allocation.h"
#include <cstdio>

using namespace devalloc;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  [FAIL] %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static Candidate cam(int idx, const char* name, bool is5III, long long px, const char* saved = "") {
    Candidate c; c.index = idx; c.name = QString::fromUtf8(name);
    c.is5III = is5III; c.pixelCount = px; c.savedRole = QString::fromUtf8(saved); return c;
}

int main()
{
    // ── §7-1 双相机异系列全连接：QHY600 + QHY5III ────────────────────────────
    {
        QVector<Candidate> pool{ cam(0,"QHY600",false,60000000), cam(1,"QHY5III",true,2000000) };
        Plan p = planCameraAllocation(pool, Request{true,true,true});
        CHECK(p.boundIndex(Role::MainCamera)==0, "S1 Main<-QHY600(非5III)");
        CHECK(p.boundIndex(Role::Guider)==1,     "S1 Guider<-QHY5III");
        CHECK(p.toAllocate.isEmpty(),            "S1 无待分配");
    }
    // ── §7-2 双相机同系列（都非5III，分辨率不同）：高->Main 低->Guider ────────
    {
        QVector<Candidate> pool{ cam(0,"QHY268",false,20000000), cam(1,"QHY600",false,60000000) };
        Plan p = planCameraAllocation(pool, Request{true,true,true});
        CHECK(p.boundIndex(Role::MainCamera)==1, "S2 Main<-高分辨率QHY600");
        CHECK(p.boundIndex(Role::Guider)==0,     "S2 Guider<-低分辨率QHY268");
        CHECK(p.toAllocate.isEmpty(),            "S2 无待分配");
    }
    // ── §7-2b 双相机同系列（都5III，分辨率不同） ─────────────────────────────
    {
        QVector<Candidate> pool{ cam(0,"QHY5III-A",true,3000000), cam(1,"QHY5III-B",true,2000000) };
        Plan p = planCameraAllocation(pool, Request{true,true,true});
        CHECK(p.boundIndex(Role::MainCamera)==0, "S2b Main<-高分辨率5III");
        CHECK(p.boundIndex(Role::Guider)==1,     "S2b Guider<-低分辨率5III");
    }
    // ── §7-3 单连接 MainCamera（池中 QHY600+5III）：优先非5III ────────────────
    {
        QVector<Candidate> pool{ cam(0,"QHY600",false,60000000), cam(1,"QHY5III",true,2000000) };
        Plan p = planCameraAllocation(pool, Request{true,false,true});
        CHECK(p.boundIndex(Role::MainCamera)==0, "S3 单Main<-QHY600");
        CHECK(!p.binds(Role::Guider),            "S3 不顺带绑Guider");
        CHECK(p.toAllocate.isEmpty(),            "S3 Main已绑,不上报无关相机");
    }
    // ── §7-4 单连接 Guider（池中 QHY600+5III）：优先5III ──────────────────────
    {
        QVector<Candidate> pool{ cam(0,"QHY600",false,60000000), cam(1,"QHY5III",true,2000000) };
        Plan p = planCameraAllocation(pool, Request{false,true,true});
        CHECK(p.boundIndex(Role::Guider)==1,     "S4 单Guider<-QHY5III");
        CHECK(!p.binds(Role::MainCamera),        "S4 不顺带绑Main");
    }
    // ── 历史回绑优先于 5III 启发式 ───────────────────────────────────────────
    {
        // idx0=QHY600(非5III) 历史绑为 Guider；应尊重历史，Main 只能拿到剩下的 5III
        QVector<Candidate> pool{ cam(0,"QHY600",false,60000000,"Guider"), cam(1,"QHY5III",true,2000000) };
        Plan p = planCameraAllocation(pool, Request{true,true,true});
        CHECK(p.boundIndex(Role::Guider)==0,     "H 历史: Guider<-QHY600");
        CHECK(p.boundIndex(Role::MainCamera)==1, "H 兜底只剩5III: Main<-QHY5III");
    }
    // ── #2 不变量：请求角色无法自动绑定时，绝不静默丢弃 ──────────────────────
    {
        // 非 QHY 双角色场景（isQhyDualRole=false）：无自动兜底，2 台相机都应上报待分配
        QVector<Candidate> pool{ cam(0,"CamA",false,10000000), cam(1,"CamB",false,10000000) };
        Plan p = planCameraAllocation(pool, Request{true,false,false});
        CHECK(!p.binds(Role::MainCamera),        "#2 非QHY: 未自动绑Main");
        CHECK(!p.toAllocate.isEmpty(),           "#2 非QHY: 候选进待分配(不静默卡死)");
    }
    {
        // 模拟 clear 后 4 台 QHY(2真+2DEMO)、单角色请求但历史/规则均未命中该角色时，
        // 任一被请求角色未绑定 => 必有待分配候选（前端会弹分配窗而非转圈）。
        QVector<Candidate> pool{
            cam(0,"QHY5III-DEMO",true,2000000), cam(1,"QHY5III",true,2000000),
            cam(2,"QHY5III-DEMO2",true,2000000), cam(3,"QHY5III-real",true,2000000) };
        Plan p = planCameraAllocation(pool, Request{false,true,true}); // 单 Guider
        // Guider 能从 5III 里选到；断言不留“既没绑又没上报”的窟窿：
        bool guiderSatisfied = p.binds(Role::Guider);
        bool noSilentDrop = guiderSatisfied || !p.toAllocate.isEmpty();
        CHECK(noSilentDrop, "#2 4xQHY单Guider: 要么绑定要么上报,无静默丢弃");
    }

    std::printf("\n==== alloc_test: %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
