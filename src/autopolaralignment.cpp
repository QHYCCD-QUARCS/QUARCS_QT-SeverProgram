#include "autopolaralignment.h"
#include <QThread>
#include <QEventLoop>
#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <cmath>
#include <algorithm>
#include <ctime>

#include <cmath>
#include <algorithm>
#include <limits>
#include <string>

namespace {
constexpr double kDeg2Rad = M_PI / 180.0;
constexpr double kRad2Deg = 180.0 / M_PI;
constexpr double kEps = 1e-12;

struct Vec3 {
    double x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(double X, double Y, double Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(double s) const { return {x*s, y*s, z*s}; }
};

inline double dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
inline double norm(const Vec3& v) { return std::sqrt(std::max(0.0, dot(v,v))); }
inline Vec3 normalize(const Vec3& v) {
    double n = norm(v);
    if (n < kEps) return {0,0,0};
    return v * (1.0 / n);
}
inline double clamp(double x, double a, double b){ return std::max(a, std::min(b, x)); }
inline double wrapTo2Pi(double a){
    double t = std::fmod(a, 2.0*M_PI);
    if (t < 0) t += 2.0*M_PI;
    return t;
}

// RA[deg], DEC[deg] -> 单位向量
inline Vec3 radecDeg_to_vec(const double ra_deg, const double dec_deg){
    double a = ra_deg * kDeg2Rad;
    double d = dec_deg * kDeg2Rad;
    double cd = std::cos(d);
    return { cd*std::cos(a), cd*std::sin(a), std::sin(d) };
}

// 单位向量 -> RA[deg], DEC[deg]
inline void vec_to_radecDeg(const Vec3& v, double& ra_deg, double& dec_deg){
    Vec3 u = normalize(v);
    double dec = std::asin(clamp(u.z, -1.0, 1.0));
    double ra = std::atan2(u.y, u.x);
    ra = wrapTo2Pi(ra);
    ra_deg  = ra  * kRad2Deg;
    dec_deg = dec * kRad2Deg;
}

// 在 p 点（单位向量）构造一个稳定的切平面正交基 {e1, e2}，避免在极区退化
struct TangentBasis {
    Vec3 e1; // 第一切向轴
    Vec3 e2; // 第二切向轴（右手系，e2 = p × e1 方向）
};
inline TangentBasis make_tangent_basis(const Vec3& p){
    // 选一个不与 p 平行的参考向量
    Vec3 ref = (std::fabs(p.z) < 0.9) ? Vec3{0,0,1} : Vec3{0,1,0};
    Vec3 e1 = normalize( ref - p * dot(ref, p) ); // 去掉法向分量后归一化
    Vec3 e2 = normalize( cross(p, e1) );
    return {e1, e2};
}

// 球面对数映射：q ∈ S^2 -> 切平面 T_p S^2 的 2D 坐标（以 {e1,e2} 为基）
inline std::pair<double,double> log_map_2d(const Vec3& p, const TangentBasis& B, const Vec3& q){
    double cos_th = clamp(dot(p, q), -1.0, 1.0);
    double th = std::acos(cos_th);                // 球面角距
    if (th < 1e-10) return {0.0, 0.0};
    double sin_th = std::sin(th);
    // 3D 切向量（T_pS^2）
    Vec3 w3 = (q - p * cos_th) * (th / sin_th);   // w3 ⟂ p
    // 用 {e1,e2} 展开为 2D
    double u = dot(w3, B.e1);
    double v = dot(w3, B.e2);
    return {u, v};
}

// 球面指数映射：切平面 2D 向量 -> S^2
inline Vec3 exp_map_2d(const Vec3& p, const TangentBasis& B, const double u, const double v){
    Vec3 w3 = B.e1 * u + B.e2 * v;
    double th = norm(w3);
    if (th < 1e-12) return p;
    Vec3 wdir = w3 * (1.0 / th);
    return normalize( p * std::cos(th) + wdir * std::sin(th) );
}

// 球上两点夹角（弧度）
inline double ang(const Vec3& a, const Vec3& b){
    return std::acos(clamp(dot(normalize(a), normalize(b)), -1.0, 1.0));
}

// 最小旋转把 a 旋到 b，并作用到任意向量 v 上（Rodrigues 公式）
inline Vec3 rotate_minimal_map_a_to_b(const Vec3& v, const Vec3& a, const Vec3& b){
    Vec3 aa = normalize(a), bb = normalize(b);
    double c = clamp(dot(aa, bb), -1.0, 1.0);
    if (c > 1.0 - 1e-12) {
        // a 与 b 基本同向：恒等变换
        return v;
    }
    if (c < -1.0 + 1e-12) {
        // 180° 旋转：轴任取一条与 a ⟂ 的向量
        Vec3 ref = (std::fabs(aa.z) < 0.9) ? Vec3{0,0,1} : Vec3{0,1,0};
        Vec3 k = normalize( cross(aa, ref) ); // 旋转轴
        // Rodriques: R(v) = v cosπ + (k×v) sinπ + k(k·v)(1-cosπ) = -v + 2k(k·v)
        return k * (2.0 * dot(k, v)) - v;
    }
    Vec3 k = normalize( cross(aa, bb) );     // 旋转轴
    double s = std::sqrt(std::max(0.0, 1.0 - c*c)); // = |sin φ|
    // Rodrigues: R v = v cφ + (k×v) sφ + k(k·v)(1-cφ)
    return v * c + cross(k, v) * s + k * ( dot(k, v) * (1.0 - c) );
}

// 用三点在球面上拟合"小圆"的球心（假极点）与半径（弧度）
// 噪声下采用三对差分的法向量近似（无需第三方库）；返回 residual_rms 表征残差
inline bool fit_spherical_circle_3pt(const Vec3& q1, const Vec3& q2, const Vec3& q3,
                                     Vec3& c_hat, double& rho, double& residual_rms)
{
    // 噪声理想情形：c ⟂ (q1-q2), c ⟂ (q1-q3)  =>  c ∥ (q1-q2)×(q1-q3)
    Vec3 n = cross(q1 - q2, q1 - q3);
    double nrm = norm(n);
    if (nrm < 1e-10) return false; // 退化：三点几乎共线或过近

    c_hat = normalize(n);

    // 半径由 θ_i = arccos(c·qi) 的平均近似
    double t1 = ang(c_hat, q1), t2 = ang(c_hat, q2), t3 = ang(c_hat, q3);
    rho = (t1 + t2 + t3) / 3.0;

    // 残差（RMS）
    double m = rho;
    residual_rms = std::sqrt( ( (t1-m)*(t1-m) + (t2-m)*(t2-m) + (t3-m)*(t3-m) ) / 3.0 );

    return true;
}
} // anonymous namespace


// 把当前点 currentRA/DEC 通过"把假极点 c 旋到真极点 p 的最小旋转"映到目标点
static void computeTargetByAxisRotation(double currentRA_deg, double currentDEC_deg,
    double realPolarRA_deg, double realPolarDEC_deg,
    double fakePolarRA_deg, double fakePolarDEC_deg,
    double& targetRA_deg, double& targetDEC_deg)
{
Vec3 s = radecDeg_to_vec(currentRA_deg, currentDEC_deg);
Vec3 p = radecDeg_to_vec(realPolarRA_deg,  realPolarDEC_deg);
Vec3 c = radecDeg_to_vec(fakePolarRA_deg,  fakePolarDEC_deg);

// 目标 = 把 s 用"把 c 旋到 p 的最小旋转 R"作用
Vec3 t = rotate_minimal_map_a_to_b(s, c, p);

vec_to_radecDeg(t, targetRA_deg, targetDEC_deg);
// RA 规范到 0..360，DEC 已在 [-90,90]
if (targetRA_deg < 0) targetRA_deg = std::fmod(targetRA_deg + 360.0, 360.0);
}

// === 单次拍摄偏差与指引（核心）：以当前点 S 为切点，给出 EN 分量与总角距 ===
struct SingleShotGuide {
    double targetRA_deg;
    double targetDEC_deg;
    double east_arcmin;    // +向东 / -向西
    double north_arcmin;   // +向北 / -向南
    double distance_arcmin;
    double bearing_deg_from_north; // 指引箭头方位角（从北向东测）
};

inline SingleShotGuide compute_single_shot_guide(double currentRA_deg, double currentDEC_deg,
    double realRA_deg, double realDEC_deg,
    double fakeRA_deg, double fakeDEC_deg)
{
constexpr double kRad2Deg = 180.0 / M_PI;

// 1) 目标点 T
double targetRA_deg = 0.0, targetDEC_deg = 0.0;
computeTargetByAxisRotation(currentRA_deg, currentDEC_deg,
realRA_deg, realDEC_deg,
fakeRA_deg, fakeDEC_deg,
targetRA_deg, targetDEC_deg);

// 2) 以当前点 S 为切点建立 EN 基底
Vec3 S = radecDeg_to_vec(currentRA_deg, currentDEC_deg);
// "北"方向：指向更高赤纬（沿经线）
Vec3 Zc = {0,0,1};                     // 天球北极方向
Vec3 north_dir = normalize( cross( cross(S, Zc), S ) ); // 去除法向分量
// "东"方向：正东 = 北 × 视线
Vec3 east_dir  = normalize( cross(north_dir, S) );
TangentBasis B = { east_dir, north_dir }; // e1=东, e2=北

// 3) 把 T 投到以 S 为切点的切平面，得到 2D 小向量（单位：弧度）
Vec3 T = radecDeg_to_vec(targetRA_deg, targetDEC_deg);
auto [u_east, v_north] = log_map_2d(S, B, T);

// 4) 量化
double dist_rad  = std::sqrt(u_east*u_east + v_north*v_north);
double dist_arcm = dist_rad * kRad2Deg * 60.0;
double bearing   = std::atan2(u_east, v_north) * kRad2Deg; // 0°=北，顺时针向东正
if (bearing < 0) bearing += 360.0;

SingleShotGuide g;
g.targetRA_deg  = targetRA_deg;
g.targetDEC_deg = targetDEC_deg;
g.east_arcmin   =  u_east * kRad2Deg * 60.0;
g.north_arcmin  =  v_north * kRad2Deg * 60.0;
g.distance_arcmin = dist_arcm;
g.bearing_deg_from_north = bearing;
return g;
}

// 仅依据 当前点S 与 固定目标T 计算 EN 位移
inline SingleShotGuide delta_to_fixed_target(double currentRA_deg, double currentDEC_deg,
    double targetRA_deg, double targetDEC_deg)
{
constexpr double kRad2Deg = 180.0 / M_PI;

Vec3 S = radecDeg_to_vec(currentRA_deg, currentDEC_deg);
Vec3 T = radecDeg_to_vec(targetRA_deg, targetDEC_deg);

// 以 S 为切点构造"北/东"基底
Vec3 Zc = {0,0,1};
Vec3 north = normalize( cross( cross(S, Zc), S ) ); // 朝更高赤纬
Vec3 east  = normalize( cross(north, S) );          // 北 × 视线
TangentBasis B { east, north };                     // e1=东, e2=北

auto [u_east, v_north] = log_map_2d(S, B, T);

double dist_rad  = std::sqrt(u_east*u_east + v_north*v_north);
double dist_arcm = dist_rad * kRad2Deg * 60.0;
double bearing   = std::atan2(u_east, v_north) * kRad2Deg;
if (bearing < 0) bearing += 360.0;

return SingleShotGuide{
targetRA_deg, targetDEC_deg,
u_east * kRad2Deg * 60.0,
v_north * kRad2Deg * 60.0,
dist_arcm, bearing
};
}


PolarAlignment::PolarAlignment(MyClient* indiServer, INDI::BaseDevice* dpMount, INDI::BaseDevice* dpMainCamera, QObject *parent)
    : QObject(parent)
    , indiServer(indiServer)
    , dpMount(dpMount)
    , dpMainCamera(dpMainCamera)
    , currentState(PolarAlignmentState::IDLE)
    , obstacleFromState(PolarAlignmentState::IDLE)
    , initialRA(0.0)
    , initialDEC(0.0)
    , ra1ObstacleAvoided(false)
    , ra2ObstacleAvoided(false)
    , justCompletedObstacleAvoidance(false)
    , currentMeasurementIndex(0)
    , isRunningFlag(false)
    , isPausedFlag(false)
    , userAdjustmentConfirmed(false)
    , progressPercentage(0)
    , currentRAPosition(0.0)
    , currentDECPosition(0.0)
    , currentStatusMessage("空闲")
    , currentSolveResult()
    , isCaptureEnd(false)
    , isSolveEnd(false)
    , lastCapturedImage("")
    , captureFailureCount(0)
    , solveFailureCount(0)
    , targetRA(0.0)
    , targetDEC(0.0)
    , isTargetPositionCached(false)
    , realPolarRA(0.0)
    , realPolarDEC(0.0)
    , isRealPolarCached(false)
{
    // 初始化定时器为单次触发模式
    stateTimer.setSingleShot(true);
    captureAndAnalysisTimer.setSingleShot(true);
    movementTimer.setSingleShot(true);
    
    // 连接定时器信号到对应的槽函数
    connect(&stateTimer, &QTimer::timeout, this, &PolarAlignment::onStateTimerTimeout);
    connect(&captureAndAnalysisTimer, &QTimer::timeout, this, &PolarAlignment::onCaptureAndAnalysisTimerTimeout);
    // connect(&movementTimer, &QTimer::timeout, this, &PolarAlignment::onMovementTimerTimeout);
    
    // 初始化result结构体，避免NaN问题
    result.isSuccessful = false;
    result.raDeviation = 0.0;
    result.decDeviation = 0.0;
    result.totalDeviation = 0.0;
    result.errorMessage = "";
    
    Logger::Log("PolarAlignment: 极轴校准系统初始化完成", LogLevel::INFO, DeviceType::MAIN);
    testimage = 0;
}

PolarAlignment::~PolarAlignment()
{
    stopPolarAlignment();
    Logger::Log("PolarAlignment: 极轴校准系统已销毁", LogLevel::INFO, DeviceType::MAIN);
}

bool PolarAlignment::startPolarAlignment()
{
    QMutexLocker locker(&stateMutex);
    if (isRunningFlag) {
        Logger::Log("PolarAlignment: 校准流程已在运行中", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    if (!indiServer || !dpMount || !dpMainCamera) {
        Logger::Log("PolarAlignment: 设备不可用，无法启动校准", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 验证地理位置配置
    if (std::abs(config.latitude) > 90.0 || std::abs(config.longitude) > 180.0) {
        Logger::Log("PolarAlignment: 地理位置配置无效 - 纬度: " + std::to_string(config.latitude) + 
                    "°, 经度: " + std::to_string(config.longitude) + "°", LogLevel::ERROR, DeviceType::MAIN);
        result.isSuccessful = false;
        result.errorMessage = "地理位置配置无效，请检查纬度和经度设置";
        return false;
    }
    
    Logger::Log("PolarAlignment: 地理位置配置验证通过 - 纬度: " + std::to_string(config.latitude) + 
                "°, 经度: " + std::to_string(config.longitude) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 初始化校准状态和参数
    currentState = PolarAlignmentState::INITIALIZING;
    currentMeasurementIndex = 0;
    currentAdjustmentAttempt = 0;
    currentRAAngle = config.raRotationAngle;
    currentDECAngle = config.decRotationAngle;
    measurements.clear();
    result = PolarAlignmentResult();
    isRunningFlag = true;
    isPausedFlag = false;
    userAdjustmentConfirmed = false;
    progressPercentage = 0;
    isCaptureEnd = false;
    isSolveEnd = false;
    lastCapturedImage = "";
    captureFailureCount = 0;
    solveFailureCount = 0;
    firstCaptureAvoidanceCount = 0;
    secondCaptureAvoidanceCount = 0;
    thirdCaptureAvoidanceCount = 0;
    decMovedToAvoidObstacle = false;
    decMovedAtStart = false;
    secondCaptureAvoided = false;
    captureAttemptCount = 0;
    
    // 重置目标位置缓存，每次校准都重新计算
    isTargetPositionCached = false;
    targetRA = 0.0;
    targetDEC = 0.0;
    
    // 重置假极轴位置缓存，每次校准都重新计算
    isFakePolarCached = false;
    cachedFakePolarRA = 0.0;
    cachedFakePolarDEC = 0.0;
    
    Logger::Log("PolarAlignment: 校准开始，目标点已归零", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("PolarAlignment: 开始自动极轴校准流程", LogLevel::INFO, DeviceType::MAIN);
    emit stateChanged(currentState, "开始自动极轴校准...",0);
    // emit statusUpdated("开始自动极轴校准...");
    // emit progressUpdated(0);
    stateTimer.start(100);
    return true;
}

void PolarAlignment::stopPolarAlignment()
{
    QMutexLocker locker(&stateMutex);
    if (!isRunningFlag) return;
    
    // 停止所有定时器
    stateTimer.stop();
    captureAndAnalysisTimer.stop();
    movementTimer.stop();
    isRunningFlag = false;
    isPausedFlag = false;
    currentState = PolarAlignmentState::IDLE;

    indiServer->setTelescopeAbortMotion(dpMount);
    
    Logger::Log("PolarAlignment: 极轴校准已停止", LogLevel::INFO, DeviceType::MAIN);
    emit stateChanged(currentState, "极轴校准已停止",-1);
    // emit statusUpdated("极轴校准已停止");
    // emit progressUpdated(0);
}

void PolarAlignment::pausePolarAlignment()
{
    QMutexLocker locker(&stateMutex);
    if (!isRunningFlag || isPausedFlag) return;
    
    isPausedFlag = true;
    stateTimer.stop();
    captureAndAnalysisTimer.stop();
    movementTimer.stop();
    
    Logger::Log("PolarAlignment: 极轴校准已暂停", LogLevel::INFO, DeviceType::MAIN);
    emit stateChanged(currentState, "极轴校准已暂停",progressPercentage);
    // emit statusUpdated("极轴校准已暂停");
    // emit progressUpdated(0);
}

void PolarAlignment::resumePolarAlignment()
{
    QMutexLocker locker(&stateMutex);
    if (!isRunningFlag || !isPausedFlag) return;
    
    isPausedFlag = false;
    stateTimer.start(100);
    
    Logger::Log("PolarAlignment: 极轴校准已恢复", LogLevel::INFO, DeviceType::MAIN);
    emit stateChanged(currentState, "极轴校准已恢复",progressPercentage);
    // emit statusUpdated("极轴校准已恢复");
    // emit progressUpdated(0);
}

void PolarAlignment::userConfirmAdjustment()
{
    QMutexLocker locker(&stateMutex);
    if (currentState == PolarAlignmentState::GUIDING_ADJUSTMENT) {
        Logger::Log("PolarAlignment: 用户确认调整完成", LogLevel::INFO, DeviceType::MAIN);
        userAdjustmentConfirmed = true;
        if (isRunningFlag && !isPausedFlag) {
            stateTimer.start(100);
        }
    }
}

void PolarAlignment::userCancelAlignment()
{
    QMutexLocker locker(&stateMutex);
    if (currentState == PolarAlignmentState::GUIDING_ADJUSTMENT) {
        Logger::Log("PolarAlignment: 用户取消校准", LogLevel::INFO, DeviceType::MAIN);
        result.isSuccessful = false;
        result.errorMessage = "用户取消校准";
        stopPolarAlignment();
        emit resultReady(result);
    }
}

PolarAlignmentState PolarAlignment::getCurrentState() const
{
    QMutexLocker locker(&stateMutex);
    return currentState;
}

QString PolarAlignment::getCurrentStatusMessage() const
{
    QMutexLocker locker(&stateMutex);
    return currentStatusMessage;
}

int PolarAlignment::getProgressPercentage() const
{
    QMutexLocker locker(&stateMutex);
    return progressPercentage;
}

bool PolarAlignment::isRunning() const
{
    QMutexLocker locker(&stateMutex);
    return isRunningFlag && !isPausedFlag;
}

bool PolarAlignment::isCompleted() const
{
    QMutexLocker locker(&stateMutex);
    return currentState == PolarAlignmentState::COMPLETED;
}

bool PolarAlignment::isFailed() const
{
    QMutexLocker locker(&stateMutex);
    return currentState == PolarAlignmentState::FAILED || currentState == PolarAlignmentState::USER_INTERVENTION;
}

PolarAlignmentResult PolarAlignment::getResult() const
{
    QMutexLocker locker(&resultMutex);
    return result;
}


void PolarAlignment::setConfig(const PolarAlignmentConfig& config)
{
    this->config = config;
    Logger::Log("PolarAlignment: 校准配置已更新", LogLevel::INFO, DeviceType::MAIN);
}


void PolarAlignment::onStateTimerTimeout()
{
    if (isPausedFlag) return;
    
    
    processCurrentState();
}

void PolarAlignment::onCaptureAndAnalysisTimerTimeout()
{
    Logger::Log("PolarAlignment: 拍摄和分析超时", LogLevel::WARNING, DeviceType::MAIN);
    handleAnalysisFailure(1);
}

void PolarAlignment::onMovementTimerTimeout()
{
    Logger::Log("PolarAlignment: 望远镜移动超时", LogLevel::WARNING, DeviceType::MAIN);
    setState(PolarAlignmentState::FAILED);
}



void PolarAlignment::setState(PolarAlignmentState newState)
{
    QMutexLocker locker(&stateMutex);
    if (currentState == newState) return;
    
    currentState = newState;
    
    // 根据新状态设置对应的状态消息
    switch (newState) {
        case PolarAlignmentState::IDLE:
            currentStatusMessage = "空闲";
            break;
        case PolarAlignmentState::INITIALIZING:
            currentStatusMessage = "初始化中...";
            break;
        case PolarAlignmentState::CHECKING_POLAR_POINT:
            currentStatusMessage = "检查极点位置...";
            break;
        case PolarAlignmentState::MOVING_DEC_AWAY:
            currentStatusMessage = "移动DEC轴脱离极点...";
            break;
        case PolarAlignmentState::WAITING_DEC_MOVE_END:
            currentStatusMessage = "等待DEC轴移动...";
            break;
        case PolarAlignmentState::FIRST_CAPTURE:
            currentStatusMessage = "第一次拍摄...";
            break;
        case PolarAlignmentState::FIRST_CAPTURE_LONG_EXPOSURE:
            currentStatusMessage = "第一次拍摄长曝光重试...";
            break;
        case PolarAlignmentState::FIRST_CAPTURE_DEC_AVOIDANCE:
            currentStatusMessage = "第一次拍摄DEC轴避障...";
            break;
        case PolarAlignmentState::WAITING_FIRST_DEC_AVOIDANCE:
            currentStatusMessage = "等待第一次拍摄DEC轴避障...";
            break;
        case PolarAlignmentState::SECOND_CAPTURE:
            currentStatusMessage = "第二次拍摄...";
            break;
        case PolarAlignmentState::SECOND_CAPTURE_LONG_EXPOSURE:
            currentStatusMessage = "第二次拍摄长曝光重试...";
            break;
        case PolarAlignmentState::SECOND_CAPTURE_RA_AVOIDANCE:
            currentStatusMessage = "第二次拍摄RA轴避障...";
            break;
        case PolarAlignmentState::WAITING_SECOND_RA_AVOIDANCE:
            currentStatusMessage = "等待第二次拍摄RA轴避障...";
            break;
        case PolarAlignmentState::THIRD_CAPTURE:
            currentStatusMessage = "第三次拍摄...";
            break;
        case PolarAlignmentState::THIRD_CAPTURE_LONG_EXPOSURE:
            currentStatusMessage = "第三次拍摄长曝光重试...";
            break;
        case PolarAlignmentState::THIRD_CAPTURE_RA_AVOIDANCE:
            currentStatusMessage = "第三次拍摄RA轴避障...";
            break;
        case PolarAlignmentState::WAITING_THIRD_RA_AVOIDANCE:
            currentStatusMessage = "等待第三次拍摄RA轴避障...";
            break;
        case PolarAlignmentState::MOVING_RA_FIRST:
            currentStatusMessage = "第一次RA轴移动...";
            break;
        case PolarAlignmentState::WAITING_RA_MOVE_END:
            currentStatusMessage = "等待RA第一次轴移动...";
            break;
        case PolarAlignmentState::MOVING_RA_SECOND:
            currentStatusMessage = "第二次RA轴移动...";
            break;
        case PolarAlignmentState::WAITING_RA_MOVE_END_2:
            currentStatusMessage = "等待RA第二次轴移动...";
            break;
        case PolarAlignmentState::CALCULATING_DEVIATION:
            currentStatusMessage = "计算偏差...";
            break;
        case PolarAlignmentState::GUIDING_ADJUSTMENT:
            currentStatusMessage = "指导调整...";
            break;
        case PolarAlignmentState::FINAL_VERIFICATION:
            currentStatusMessage = "最终验证...";
            break;
        case PolarAlignmentState::ADJUSTING_FOR_OBSTACLE:
            currentStatusMessage = "调整避开遮挡物...";
            break;
        case PolarAlignmentState::COMPLETED:
            currentStatusMessage = "校准完成";
            break;
        case PolarAlignmentState::FAILED:
            currentStatusMessage = "校准失败";
            break;
        case PolarAlignmentState::USER_INTERVENTION:
            currentStatusMessage = "需要用户干预";
            break;
    }
    
    Logger::Log("PolarAlignment: 状态已更改为 " + currentStatusMessage.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    // emit statusUpdated(currentStatusMessage);    // 0: 开始 1: 结束 -1: 暂停
    
    // 根据状态更新进度百分比
    int newProgress = 0;
    switch (newState) {
        // 初始化阶段 (0-10%)
        case PolarAlignmentState::INITIALIZING:
            newProgress = 2;
            break;
        case PolarAlignmentState::CHECKING_POLAR_POINT:
            newProgress = 5;
            break;
        case PolarAlignmentState::MOVING_DEC_AWAY:
            newProgress = 7;
            break;
        case PolarAlignmentState::WAITING_DEC_MOVE_END:
            newProgress = 8;
            break;
        
        // 第一次拍摄阶段 (10-30%) - 节点: 25%
        case PolarAlignmentState::FIRST_CAPTURE:
            newProgress = 15;
            break;
        case PolarAlignmentState::FIRST_CAPTURE_LONG_EXPOSURE:
            newProgress = 20;
            break;
        case PolarAlignmentState::FIRST_CAPTURE_DEC_AVOIDANCE:
            newProgress = 22;
            break;
        case PolarAlignmentState::WAITING_FIRST_DEC_AVOIDANCE:
            newProgress = 24;
            break;
        case PolarAlignmentState::MOVING_RA_FIRST:
            newProgress = 26;
            break;
        case PolarAlignmentState::WAITING_RA_MOVE_END:
            newProgress = 28;
            break;
        
        // 第二次拍摄阶段 (30-55%) - 节点: 50%
        case PolarAlignmentState::SECOND_CAPTURE:
            newProgress = 35;
            break;
        case PolarAlignmentState::SECOND_CAPTURE_LONG_EXPOSURE:
            newProgress = 40;
            break;
        case PolarAlignmentState::SECOND_CAPTURE_RA_AVOIDANCE:
            newProgress = 42;
            break;
        case PolarAlignmentState::WAITING_SECOND_RA_AVOIDANCE:
            newProgress = 44;
            break;
        case PolarAlignmentState::MOVING_RA_SECOND:
            newProgress = 46;
            break;
        case PolarAlignmentState::WAITING_RA_MOVE_END_2:
            newProgress = 48;
            break;
        
        // 第三次拍摄阶段 (55-80%) - 节点: 75%
        case PolarAlignmentState::THIRD_CAPTURE:
            newProgress = 60;
            break;
        case PolarAlignmentState::THIRD_CAPTURE_LONG_EXPOSURE:
            newProgress = 65;
            break;
        case PolarAlignmentState::THIRD_CAPTURE_RA_AVOIDANCE:
            newProgress = 67;
            break;
        case PolarAlignmentState::WAITING_THIRD_RA_AVOIDANCE:
            newProgress = 69;
            break;
        
        // 计算和调整阶段 (80-100%)
        case PolarAlignmentState::CALCULATING_DEVIATION:
            newProgress = 82;
            break;
        case PolarAlignmentState::GUIDING_ADJUSTMENT:
            newProgress = 88;
            break;
        case PolarAlignmentState::FINAL_VERIFICATION:
            newProgress = 95;
            break;
        case PolarAlignmentState::ADJUSTING_FOR_OBSTACLE:
            newProgress = 92;
            break;
        case PolarAlignmentState::COMPLETED:
            newProgress = 100;
            break;
        case PolarAlignmentState::FAILED:
        case PolarAlignmentState::USER_INTERVENTION:
            newProgress = 0;
            break;
        default:
            newProgress = progressPercentage;
            break;
    }
    
    if (newProgress != progressPercentage) {
        progressPercentage = newProgress;
        // emit progressUpdated(progressPercentage);
        emit stateChanged(currentState, currentStatusMessage, progressPercentage);
    }
    
    // 如果正在运行且未暂停，启动状态定时器
    if (isRunningFlag && !isPausedFlag) {
        stateTimer.start(100);
    }
}

void PolarAlignment::processCurrentState()
{
    QString Stat;
    switch (currentState) {
        case PolarAlignmentState::INITIALIZING:
            setState(PolarAlignmentState::CHECKING_POLAR_POINT);
            break;
        case PolarAlignmentState::CHECKING_POLAR_POINT:
            {
                PolarPointCheckResult checkResult = checkPolarPoint();
                if (checkResult.success) {
                    if (checkResult.isNearPole) {
                        // 如果需要移动DEC轴，则移动到DEC轴脱离极点状态
                        setState(PolarAlignmentState::MOVING_DEC_AWAY);
                    } else {
                        // 如果不需要移动DEC轴，直接开始第一次拍摄
                        setState(PolarAlignmentState::FIRST_CAPTURE);
                    }
                } else {
                    // 极点验证失败，直接设置为失败状态
                    Logger::Log("PolarAlignment: 极点验证失败，停止校准", LogLevel::ERROR, DeviceType::MAIN);
                    setState(PolarAlignmentState::FAILED);
                }
            }
            break;
        case PolarAlignmentState::MOVING_DEC_AWAY:
            if (moveDecAxisAway()) {
                setState(PolarAlignmentState::WAITING_DEC_MOVE_END);
            } else {
                // 移动失败时，直接进入第一次拍摄，不设置FAILED状态
                Logger::Log("PolarAlignment: DEC轴移动失败，直接进入第一次拍摄", LogLevel::WARNING, DeviceType::MAIN);
                setState(PolarAlignmentState::FIRST_CAPTURE);
            }
            break;

        case PolarAlignmentState::WAITING_DEC_MOVE_END:
            indiServer->getTelescopeStatus(dpMount, Stat);
            if (Stat == "Idle") {
                // DEC移动完成，记录初始位置并开始第一次拍摄
                indiServer->getTelescopeRADECJNOW(dpMount, initialRA, initialDEC);
                initialRA = Tools::HourToDegree(initialRA);
                Logger::Log("PolarAlignment: DEC轴移动完成，记录初始位置 - RA: " + std::to_string(initialRA) + "°, DEC: " + std::to_string(initialDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
                setState(PolarAlignmentState::FIRST_CAPTURE);
            }else{
                Logger::Log("PolarAlignment: 等待DEC轴移动完成,当前状态: " + Stat.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                if (isRunningFlag && !isPausedFlag) {
                    stateTimer.start(1000); // 1秒后再次检查
                }
            }
            break;

        case PolarAlignmentState::FIRST_CAPTURE:
            if (captureAndAnalyze(1)) {
                setState(PolarAlignmentState::MOVING_RA_FIRST);
                if (measurements.size() >= 1) {
                    saveAndEmitAdjustmentGuideData(measurements[0].RA_Degree, measurements[0].DEC_Degree,
                            measurements[0].RA_0, measurements[0].DEC_0, 
                            measurements[0].RA_1, measurements[0].DEC_1, 
                            measurements[0].RA_2, measurements[0].DEC_2, 
                            measurements[0].RA_3, measurements[0].DEC_3,
                            -1, -1, 0.0, 0.0, 
                            "", "",
                            0.0, 0.0, 0.0, 0.0);
                }
            } else {
                // 检查是否是拍摄失败导致的退出
                if (captureFailureCount >= 2) {
                    // 拍摄失败，直接退出
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 解析失败，必须进行长曝光重试
                    Logger::Log("PolarAlignment: 第一次拍摄失败，必须尝试长曝光重试", LogLevel::WARNING, DeviceType::MAIN);
                    setState(PolarAlignmentState::FIRST_CAPTURE_LONG_EXPOSURE);
                }
            }
            break;
            
        case PolarAlignmentState::FIRST_CAPTURE_LONG_EXPOSURE:
            // 第一次拍摄长曝光重试
            if (captureAndAnalyze(2)) {
                // 长曝光重试成功，继续正常流程
                setState(PolarAlignmentState::MOVING_RA_FIRST);
                if (measurements.size() >= 1) {
                    saveAndEmitAdjustmentGuideData(measurements[0].RA_Degree, measurements[0].DEC_Degree,
                            measurements[0].RA_0, measurements[0].DEC_0, 
                            measurements[0].RA_1, measurements[0].DEC_1, 
                            measurements[0].RA_2, measurements[0].DEC_2,
                            measurements[0].RA_3, measurements[0].DEC_3,
                            -1, -1, 0.0, 0.0, 
                            "", "",
                            0.0, 0.0, 0.0, 0.0);
                }
            } else {
                // 长曝光重试也失败，检查避障次数
                if (firstCaptureAvoidanceCount >= 1) {
                    // 已经进行过避障，仍然失败，校准失败
                    Logger::Log("PolarAlignment: 第一次拍摄长曝光重试也失败，且已进行过避障，校准失败", LogLevel::ERROR, DeviceType::MAIN);
                    setState(PolarAlignmentState::FAILED);
                } else if (decMovedAtStart) {
                    // 如果开始时移动了DEC轴脱离极点，进行DEC轴避障（移动到相反位置）
                    Logger::Log("PolarAlignment: 第一次拍摄长曝光重试也失败，进行DEC轴避障（移动到相反位置）", LogLevel::WARNING, DeviceType::MAIN);
                    firstCaptureAvoidanceCount++;
                    setState(PolarAlignmentState::FIRST_CAPTURE_DEC_AVOIDANCE);
                } else {
                    // 如果开始时不在极点附近，认为已经手动避障成功，直接报错
                    Logger::Log("PolarAlignment: 第一次拍摄长曝光重试也失败，且不在极点附近，校准失败", LogLevel::ERROR, DeviceType::MAIN);
                    setState(PolarAlignmentState::FAILED);
                }
            }
            break;
            
        case PolarAlignmentState::FIRST_CAPTURE_DEC_AVOIDANCE:
            // 第一次拍摄DEC轴避障
            if (moveDecAxisForObstacleAvoidance()) {
                setState(PolarAlignmentState::WAITING_FIRST_DEC_AVOIDANCE);
            } else {
                Logger::Log("PolarAlignment: 第一次拍摄DEC轴避障移动失败，请检查环境和焦距设置", LogLevel::ERROR, DeviceType::MAIN);
                setState(PolarAlignmentState::FAILED);
            }
            break;
            
        case PolarAlignmentState::WAITING_FIRST_DEC_AVOIDANCE:
            indiServer->getTelescopeStatus(dpMount, Stat);
            if (Stat == "Idle") {
                // DEC轴避障移动完成，重新开始第一次拍摄
                Logger::Log("PolarAlignment: 第一次拍摄DEC轴避障移动完成，重新开始第一次拍摄", LogLevel::INFO, DeviceType::MAIN);
                justCompletedObstacleAvoidance = true; // 标记刚刚完成避障
                setState(PolarAlignmentState::FIRST_CAPTURE);
            } else {
                Logger::Log("PolarAlignment: 等待第一次拍摄DEC轴避障移动完成,当前状态: " + Stat.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                if (isRunningFlag && !isPausedFlag) {
                    stateTimer.start(1000); // 1秒后再次检查
                }
            }
            break;
            
            
            
        case PolarAlignmentState::MOVING_RA_FIRST:
            if (moveRAAxis()) {
                setState(PolarAlignmentState::WAITING_RA_MOVE_END);
            } else {
                // 移动失败时，直接进入第二次拍摄，不设置FAILED状态
                Logger::Log("PolarAlignment: 第一次RA轴移动失败，直接进入第二次拍摄", LogLevel::WARNING, DeviceType::MAIN);
                setState(PolarAlignmentState::SECOND_CAPTURE);
            }
            break;
        case PolarAlignmentState::WAITING_RA_MOVE_END:
            indiServer->getTelescopeStatus(dpMount, Stat);
            if (Stat == "Idle") {
                setState(PolarAlignmentState::SECOND_CAPTURE);
            }else{
                Logger::Log("PolarAlignment: 等待第一次RA轴移动完成,当前状态: " + Stat.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                if (isRunningFlag && !isPausedFlag) {
                    stateTimer.start(1000); // 1秒后再次检查
                }
            }
            break;

        case PolarAlignmentState::SECOND_CAPTURE:
            if (captureAndAnalyze(1)) {
                setState(PolarAlignmentState::MOVING_RA_SECOND);
                if (measurements.size() >= 2) {
                    saveAndEmitAdjustmentGuideData(measurements[1].RA_Degree, measurements[1].DEC_Degree,
                            measurements[1].RA_0, measurements[1].DEC_0, 
                            measurements[1].RA_1, measurements[1].DEC_1, 
                            measurements[1].RA_2, measurements[1].DEC_2, 
                            measurements[1].RA_3, measurements[1].DEC_3,
                            -1, -1, 0.0, 0.0, "", "",
                            0.0, 0.0, 0.0, 0.0);
                }
            } else {
                // 检查是否是拍摄失败导致的退出
                if (captureFailureCount >= 2) {
                    // 拍摄失败，直接退出
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 解析失败，必须进行长曝光重试
                    Logger::Log("PolarAlignment: 第二次拍摄失败，必须尝试长曝光重试", LogLevel::WARNING, DeviceType::MAIN);
                    setState(PolarAlignmentState::SECOND_CAPTURE_LONG_EXPOSURE);
                }
            }
            break;
            
        case PolarAlignmentState::SECOND_CAPTURE_LONG_EXPOSURE:
            // 第二次拍摄长曝光重试
            if (captureAndAnalyze(2)) {
                // 长曝光重试成功，继续正常流程
                setState(PolarAlignmentState::MOVING_RA_SECOND);
                if (measurements.size() >= 2) {
                    saveAndEmitAdjustmentGuideData(measurements[1].RA_Degree, measurements[1].DEC_Degree,
                            measurements[1].RA_0, measurements[1].DEC_0, 
                            measurements[1].RA_1, measurements[1].DEC_1, 
                            measurements[1].RA_2, measurements[1].DEC_2, 
                            measurements[1].RA_3, measurements[1].DEC_3,
                            -1, -1, 0.0, 0.0, "", "",
                            0.0, 0.0, 0.0, 0.0);
                }
            } else {
                // 长曝光重试也失败，检查避障次数
                if (secondCaptureAvoidanceCount >= 1) {
                    // 已经进行过避障，仍然失败，校准失败
                    Logger::Log("PolarAlignment: 第二次拍摄长曝光重试也失败，且已进行过避障，校准失败", LogLevel::ERROR, DeviceType::MAIN);
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 进行RA轴避障
                    Logger::Log("PolarAlignment: 第二次拍摄长曝光重试也失败，进行RA轴避障", LogLevel::WARNING, DeviceType::MAIN);
                    secondCaptureAvoidanceCount++;
                    secondCaptureAvoided = true; // 标记进行了第二次拍摄避障
                    setState(PolarAlignmentState::SECOND_CAPTURE_RA_AVOIDANCE);
                }
            }
            break;
            
        case PolarAlignmentState::SECOND_CAPTURE_RA_AVOIDANCE:
            // 第二次拍摄RA轴避障
            if (moveToAvoidObstacleRA1()) {
                setState(PolarAlignmentState::WAITING_SECOND_RA_AVOIDANCE);
            } else {
                Logger::Log("PolarAlignment: 第二次拍摄RA轴避障移动失败，请检查环境和焦距设置", LogLevel::ERROR, DeviceType::MAIN);
                setState(PolarAlignmentState::FAILED);
            }
            break;
            
        case PolarAlignmentState::WAITING_SECOND_RA_AVOIDANCE:
            indiServer->getTelescopeStatus(dpMount, Stat);
            if (Stat == "Idle") {
                // RA轴避障移动完成，重新开始第二次拍摄
                Logger::Log("PolarAlignment: 第二次拍摄RA轴避障移动完成，重新开始第二次拍摄", LogLevel::INFO, DeviceType::MAIN);
                justCompletedObstacleAvoidance = true; // 标记刚刚完成避障
                setState(PolarAlignmentState::SECOND_CAPTURE);
            } else {
                Logger::Log("PolarAlignment: 等待第二次拍摄RA轴避障移动完成,当前状态: " + Stat.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                if (isRunningFlag && !isPausedFlag) {
                    stateTimer.start(1000); // 1秒后再次检查
                }
            }
            break;
        case PolarAlignmentState::MOVING_RA_SECOND:
            if (moveRAAxis()) {
                setState(PolarAlignmentState::WAITING_RA_MOVE_END_2);
            } else {
                // 移动失败时，直接进入第三次拍摄，不设置FAILED状态
                Logger::Log("PolarAlignment: 第二次RA轴移动失败，直接进入第三次拍摄", LogLevel::WARNING, DeviceType::MAIN);
                setState(PolarAlignmentState::THIRD_CAPTURE);
            }
            break;

        case PolarAlignmentState::WAITING_RA_MOVE_END_2:
            indiServer->getTelescopeStatus(dpMount, Stat);
            if (Stat == "Idle") {
                setState(PolarAlignmentState::THIRD_CAPTURE);
            }else{
                Logger::Log("PolarAlignment: 等待第二次RA轴移动完成,当前状态: " + Stat.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                if (isRunningFlag && !isPausedFlag) {
                    stateTimer.start(1000); // 1秒后再次检查
                }
            }
            break;
            
        case PolarAlignmentState::THIRD_CAPTURE:
            if (captureAndAnalyze(1)) {
                setState(PolarAlignmentState::CALCULATING_DEVIATION);
                if (measurements.size() >= 3) {
                    saveAndEmitAdjustmentGuideData(measurements[2].RA_Degree, measurements[2].DEC_Degree,
                            measurements[2].RA_0, measurements[2].DEC_0, 
                            measurements[2].RA_1, measurements[2].DEC_1, 
                            measurements[2].RA_2, measurements[2].DEC_2, 
                            measurements[2].RA_3, measurements[2].DEC_3,
                            -1, -1, 0.0, 0.0, "", "",
                            0.0, 0.0, 0.0, 0.0);
                }
            } else {
                // 检查是否是拍摄失败导致的退出
                if (captureFailureCount >= 2) {
                    // 拍摄失败，直接退出
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 解析失败，必须进行长曝光重试
                    Logger::Log("PolarAlignment: 第三次拍摄失败，必须尝试长曝光重试", LogLevel::WARNING, DeviceType::MAIN);
                    setState(PolarAlignmentState::THIRD_CAPTURE_LONG_EXPOSURE);
                }
            }
            break;
            
        case PolarAlignmentState::THIRD_CAPTURE_LONG_EXPOSURE:
            // 第三次拍摄长曝光重试
            if (captureAndAnalyze(2)) {
                // 长曝光重试成功，继续正常流程
                setState(PolarAlignmentState::CALCULATING_DEVIATION);
                if (measurements.size() >= 3) {
                    saveAndEmitAdjustmentGuideData(measurements[2].RA_Degree, measurements[2].DEC_Degree,
                            measurements[2].RA_0, measurements[2].DEC_0, 
                            measurements[2].RA_1, measurements[2].DEC_1, 
                            measurements[2].RA_2, measurements[2].DEC_2, 
                            measurements[2].RA_3, measurements[2].DEC_3,
                            -1, -1, 0.0, 0.0, "", "",
                            0.0, 0.0, 0.0, 0.0);
                }
            } else {
                // 长曝光重试也失败，检查避障次数
                if (thirdCaptureAvoidanceCount >= 1) {
                    // 已经进行过避障，仍然失败，校准失败
                    Logger::Log("PolarAlignment: 第三次拍摄长曝光重试也失败，且已进行过避障，校准失败", LogLevel::ERROR, DeviceType::MAIN);
                    setState(PolarAlignmentState::FAILED);
                } else {
                    // 进行RA轴避障
                    Logger::Log("PolarAlignment: 第三次拍摄长曝光重试也失败，进行RA轴避障", LogLevel::WARNING, DeviceType::MAIN);
                    thirdCaptureAvoidanceCount++;
                    setState(PolarAlignmentState::THIRD_CAPTURE_RA_AVOIDANCE);
                }
            }
            break;
            
        case PolarAlignmentState::THIRD_CAPTURE_RA_AVOIDANCE:
            // 第三次拍摄RA轴避障
            if (moveToAvoidObstacleRA2()) {
                setState(PolarAlignmentState::WAITING_THIRD_RA_AVOIDANCE);
            } else {
                Logger::Log("PolarAlignment: 第三次拍摄RA轴避障移动失败，请检查环境和焦距设置", LogLevel::ERROR, DeviceType::MAIN);
                setState(PolarAlignmentState::FAILED);
            }
            break;
            
        case PolarAlignmentState::WAITING_THIRD_RA_AVOIDANCE:
            indiServer->getTelescopeStatus(dpMount, Stat);
            if (Stat == "Idle") {
                // RA轴避障移动完成，重新开始第三次拍摄
                Logger::Log("PolarAlignment: 第三次拍摄RA轴避障移动完成，重新开始第三次拍摄", LogLevel::INFO, DeviceType::MAIN);
                justCompletedObstacleAvoidance = true; // 标记刚刚完成避障
                setState(PolarAlignmentState::THIRD_CAPTURE);
            } else {
                Logger::Log("PolarAlignment: 等待第三次拍摄RA轴避障移动完成,当前状态: " + Stat.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                if (isRunningFlag && !isPausedFlag) {
                    stateTimer.start(1000); // 1秒后再次检查
                }
            }
            break;
        case PolarAlignmentState::CALCULATING_DEVIATION:
            if (calculateDeviation()) {
                setState(PolarAlignmentState::GUIDING_ADJUSTMENT);
            } else {
                // 计算偏差失败，校准失败
                Logger::Log("PolarAlignment: 计算偏差失败，校准失败", LogLevel::ERROR, DeviceType::MAIN);
                setState(PolarAlignmentState::FAILED);
            }
            break;
        case PolarAlignmentState::GUIDING_ADJUSTMENT:
        {
            if (!performGuidanceAdjustmentStep()) {
                // 失败时 performGuidanceAdjustmentStep 内部已有日志与节拍控制
            }
        }
        break;
            
        case PolarAlignmentState::FINAL_VERIFICATION:
            if (performFinalVerification()) {
                setState(PolarAlignmentState::COMPLETED);
            } else {
                setState(PolarAlignmentState::GUIDING_ADJUSTMENT);
            }
            break;
            
            
            
        case PolarAlignmentState::ADJUSTING_FOR_OBSTACLE:
            if (adjustForObstacle()) {
                setState(PolarAlignmentState::GUIDING_ADJUSTMENT);
            } else {
                // 调整避开遮挡物失败，校准失败
                Logger::Log("PolarAlignment: 调整避开遮挡物失败，校准失败", LogLevel::ERROR, DeviceType::MAIN);
                setState(PolarAlignmentState::FAILED);
            }
            break;
        case PolarAlignmentState::COMPLETED:
            isRunningFlag = false;
            break;
        case PolarAlignmentState::FAILED:
        case PolarAlignmentState::USER_INTERVENTION:
            isRunningFlag = false;
            break;
        default:
            setState(PolarAlignmentState::FAILED);
            break;
    }
}

PolarAlignment::PolarPointCheckResult PolarAlignment::checkPolarPoint()
{
    Logger::Log("PolarAlignment: 检查极点位置", LogLevel::INFO, DeviceType::MAIN);

    double currentRA, currentDEC;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA, currentDEC);
    if (currentRA == 0.0 && currentDEC == 0.0) {
        Logger::Log("PolarAlignment: 当前位置为0,0，无法检查极点位置", LogLevel::ERROR, DeviceType::MAIN);
        return {false, false, "当前位置为0,0，无法检查极点位置"};
    }
    bool isNearPole;
    if (88<=currentDEC && currentDEC<=90.0 || -90.0<=currentDEC && currentDEC<=-88.0) {
        isNearPole = true;
    } else {
        isNearPole = false;
    }

    // 检查是否在极点（DEC=90°±5°或-90°±5°）
    bool isAtPole = (currentDEC >= 85.0 && currentDEC <= 90.0) || (currentDEC >= -90.0 && currentDEC <= -85.0);
    
    if (isAtPole) {
        // 在极点，需要移动DEC轴脱离极点
        Logger::Log("PolarAlignment: 当前位置在极点，需要移动DEC轴脱离极点", LogLevel::INFO, DeviceType::MAIN);
        decMovedAtStart = true; // 记录移动了DEC轴
        return {true, true, ""}; // 需要拍摄，也需要移动DEC轴
    } else {
        // 不在极点，不移动DEC轴，直接拍摄
        Logger::Log("PolarAlignment: 当前位置不在极点，不移动DEC轴，直接拍摄", LogLevel::INFO, DeviceType::MAIN);
        decMovedAtStart = false; // 记录没有移动DEC轴
        return {true, false, ""}; // 需要拍摄，但不需要移动DEC轴
    }
}

bool PolarAlignment::moveDecAxisAway()
{
    Logger::Log("PolarAlignment: 移动DEC轴脱离极点", LogLevel::INFO, DeviceType::MAIN);
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取当前望远镜位置
    double currentRA_Hours, currentDEC_Degree;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA_Hours, currentDEC_Degree);
    double currentRA = Tools::HourToDegree(currentRA_Hours);
    double currentDEC = currentDEC_Degree;
    
    Logger::Log("PolarAlignment: 当前位置 - RA: " + std::to_string(currentRA) + "°, DEC: " + std::to_string(currentDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 从极点移动，确保不超出有效范围
    double targetDEC;
    if (isNorthernHemisphere()) {
        // 北半球：从90°向下移动
        targetDEC = currentDEC_Degree - currentDECAngle;
        if (targetDEC < 0) targetDEC = 0; // 确保不超出范围
    } else {
        // 南半球：从-90°向上移动
        targetDEC = currentDEC_Degree + currentDECAngle;
        if (targetDEC > 0) targetDEC = 0; // 确保不超出范围
    }
    
    Logger::Log("PolarAlignment: 从极点移动到DEC: " + std::to_string(targetDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 直接移动到目标位置
    bool success = moveTelescopeToAbsolutePosition(currentRA, targetDEC);
    return success;
}

bool PolarAlignment::moveDecAxisForObstacleAvoidance()
{
    Logger::Log("PolarAlignment: DEC轴避障移动（移动到相反位置）", LogLevel::INFO, DeviceType::MAIN);
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取当前望远镜位置
    double currentRA_Hours, currentDEC_Degree;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA_Hours, currentDEC_Degree);
    double currentRA = Tools::HourToDegree(currentRA_Hours);
    double currentDEC = currentDEC_Degree;
    
    Logger::Log("PolarAlignment: 当前位置 - RA: " + std::to_string(currentRA) + "°, DEC: " + std::to_string(currentDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // DEC轴避障移动：移动到相反位置（以极点为对称中心）
    double targetDEC;
    if (isNorthernHemisphere()) {
        // 北半球：从当前DEC移动到相反位置（以90°为对称中心）
        // 如果当前DEC是85°，则移动到95°（但DEC不能超过90°，所以移动到85°）
        // 实际上是以90°为对称中心，移动到对称位置
        targetDEC = 90.0 - (currentDEC - 90.0); // 以90°为对称中心
        if (targetDEC > 90.0) targetDEC = 90.0; // 确保不超出范围
        if (targetDEC < 0) targetDEC = 0; // 确保不超出范围
    } else {
        // 南半球：从当前DEC移动到相反位置（以-90°为对称中心）
        targetDEC = -90.0 - (currentDEC - (-90.0)); // 以-90°为对称中心
        if (targetDEC < -90.0) targetDEC = -90.0; // 确保不超出范围
        if (targetDEC > 0) targetDEC = 0; // 确保不超出范围
    }
    
    Logger::Log("PolarAlignment: DEC轴避障移动到相反位置DEC: " + std::to_string(targetDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 直接移动到目标位置
    bool success = moveTelescopeToAbsolutePosition(currentRA, targetDEC);
    return success;
}

bool PolarAlignment::captureAndAnalyze(int attempt)
{
    Logger::Log("PolarAlignment: 拍摄和分析，尝试次数 " + std::to_string(attempt), LogLevel::INFO, DeviceType::MAIN);
    if (!dpMainCamera) {
        Logger::Log("PolarAlignment: 相机设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 根据尝试次数确定曝光时间
    int exposureTime;
    if (attempt == 1) {
        exposureTime = config.defaultExposureTime;
    } else if (attempt == 2) {
        exposureTime = config.recoveryExposureTime;
    } else {
        if (isFirstCaptureFailure()) {
            Logger::Log("PolarAlignment: 初次拍摄连续失败，停止校准", LogLevel::ERROR, DeviceType::MAIN);
            result.isSuccessful = false;
            result.errorMessage = "初次拍摄解析失败，请检查拍摄条件";
            setState(PolarAlignmentState::FAILED);
            return false;
        } else {
            return handlePostMovementFailure();
        }
    }
    
    // 拍摄图像
    if (!captureImage(exposureTime)) {
        Logger::Log("PolarAlignment: 图像拍摄失败", LogLevel::WARNING, DeviceType::MAIN);
        captureFailureCount++;
        
        // 检查是否达到两次拍摄失败的限制
        if (captureFailureCount >= 2) {
            Logger::Log("PolarAlignment: 连续两次拍摄失败，退出校准", LogLevel::ERROR, DeviceType::MAIN);
            result.isSuccessful = false;
            result.errorMessage = "连续两次拍摄失败，请检查相机连接和拍摄条件";
            setState(PolarAlignmentState::FAILED);
            return false;
        }
        
        return false;
    }
    
    // 等待拍摄完成
    if (!waitForCaptureComplete()) {
        Logger::Log("PolarAlignment: 拍摄超时", LogLevel::WARNING, DeviceType::MAIN);
        captureFailureCount++;
        
        // 检查是否达到两次拍摄失败的限制
        if (captureFailureCount >= 2) {
            Logger::Log("PolarAlignment: 连续两次拍摄超时，退出校准", LogLevel::ERROR, DeviceType::MAIN);
            result.isSuccessful = false;
            result.errorMessage = "连续两次拍摄超时，请检查相机连接";
            setState(PolarAlignmentState::FAILED);
            return false;
        }
        
        return false;
    }
    
    // 拍摄成功，重置拍摄失败计数
    captureFailureCount = 0;
    
    // 解析图像
    if (!solveImage(lastCapturedImage)) {
        Logger::Log("PolarAlignment: 图像解析开始命令执行失败", LogLevel::WARNING, DeviceType::MAIN);
        return false; // 直接返回失败，不进行重试
    }
    
    // 等待解析完成
    if (!waitForSolveComplete()) {
        Logger::Log("PolarAlignment: 解析超时", LogLevel::WARNING, DeviceType::MAIN);
        return false; // 直接返回失败，不进行重试
    }
    
    // 解析成功，但不立即重置解析失败计数，让重试逻辑处理
    // solveFailureCount = 0;
    
    // 获取解析结果
    SloveResults analysisResult = Tools::ReadSolveResult(lastCapturedImage, config.cameraWidth, config.cameraHeight);
    if (isAnalysisSuccessful(analysisResult)) {
        // 保存当前解析结果到成员变量
        currentSolveResult = analysisResult;
        
        // 检查是否需要添加测量结果
        // 如果当前测量索引为0，说明这是第一次拍摄，需要添加
        // 如果当前测量索引为1，说明第一个点已经在checkPolarPoint中添加，这里添加第二个点
        // 如果当前测量索引为2，说明添加第三个点
        if (currentMeasurementIndex < 3) {
            measurements.append(analysisResult);
            currentMeasurementIndex++;
            // 成功完成一个测量点，重置失败次数
            solveFailureCount = 0;
            firstCaptureAvoidanceCount = 0; // 重置第一次拍摄避障计数
            secondCaptureAvoidanceCount = 0; // 重置第二次拍摄避障计数
            thirdCaptureAvoidanceCount = 0; // 重置第三次拍摄避障计数
            decMovedToAvoidObstacle = false; // 重置DEC移动标志
            secondCaptureAvoided = false; // 重置第二次拍摄避障标志
            captureAttemptCount = 0; // 重置拍摄尝试计数
            Logger::Log("PolarAlignment: 拍摄和分析成功，测量次数 " + std::to_string(currentMeasurementIndex), LogLevel::INFO, DeviceType::MAIN);
            
            QString adjustmentRa = "";
            QString adjustmentDec = "";
            
            // 校准点数据将在emit adjustmentGuideData时保存
            
            // emit adjustmentGuideData(analysisResult.RA_Degree, analysisResult.DEC_Degree,
            //             analysisResult.RA_1, analysisResult.RA_0, 
            //             analysisResult.DEC_2, analysisResult.DEC_1, 
            //             -1, -1, 0.0, 0.0, 
            //             adjustmentRa, adjustmentDec);
        } else {
            Logger::Log("PolarAlignment: 已达到最大测量次数，跳过添加", LogLevel::WARNING, DeviceType::MAIN);
        }
        currentRAPosition = analysisResult.RA_Degree;
        currentDECPosition = analysisResult.DEC_Degree;
        return true;
    } else {
        Logger::Log("PolarAlignment: 解析结果无效", LogLevel::WARNING, DeviceType::MAIN);
        return false; // 直接返回失败，不进行重试
    }
}

bool PolarAlignment::moveRAAxis()
{
    double moveAngle = currentRAAngle;
    
    // 根据当前状态和避障标志决定移动方向
    if (currentState == PolarAlignmentState::MOVING_RA_SECOND) {
        // 第三次移动：根据是否进行了第二次拍摄避障决定方向
        if (secondCaptureAvoided) {
            // 避障后第三次移动：RA - currentRAAngle
            moveAngle = -currentRAAngle;
            Logger::Log("PolarAlignment: 避障后第三次移动RA轴 " + std::to_string(moveAngle) + " 度", LogLevel::INFO, DeviceType::MAIN);
        } else {
            // 未避障第三次移动：RA + currentRAAngle
            moveAngle = currentRAAngle;
            Logger::Log("PolarAlignment: 未避障第三次移动RA轴 " + std::to_string(moveAngle) + " 度", LogLevel::INFO, DeviceType::MAIN);
        }
    } else {
        // 第一次移动：正常移动 RA + currentRAAngle
        moveAngle = currentRAAngle;
        Logger::Log("PolarAlignment: 第一次移动RA轴 " + std::to_string(moveAngle) + " 度", LogLevel::INFO, DeviceType::MAIN);
    }
    
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 移动RA轴到下一个位置
    // moveTelescope函数内部已经包含了等待移动完成的逻辑，不需要再次调用waitForMovementComplete
    bool success = moveTelescope(moveAngle, 0.0);
    return success;
}

bool PolarAlignment::moveToAvoidObstacle()
{
    Logger::Log("PolarAlignment: 第一次拍摄失败，移动到反向位置", LogLevel::INFO, DeviceType::MAIN);
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取当前望远镜位置
    double currentRA_Hours, currentDEC_Degree;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA_Hours, currentDEC_Degree);
    double currentRA = Tools::HourToDegree(currentRA_Hours);
    double currentDEC = currentDEC_Degree;
    
    Logger::Log("PolarAlignment: 当前位置 - RA: " + std::to_string(currentRA) + "°, DEC: " + std::to_string(currentDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 第一次拍摄失败：移动到反向位置（RA+180°）
    double targetRA = currentRA + 180.0;
    if (targetRA >= 360.0) targetRA -= 360.0; // 确保RA在0-360度范围内
    double targetDEC = currentDEC; // DEC保持不变
    
    Logger::Log("PolarAlignment: 移动到反向位置，目标RA: " + std::to_string(targetRA) + "°, DEC: " + std::to_string(targetDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 移动到目标位置
    return moveTelescopeToAbsolutePosition(targetRA, targetDEC);
}

bool PolarAlignment::calculateDeviation()
{
    Logger::Log("PolarAlignment: 计算极轴偏差（球面法·精简版）", LogLevel::INFO, DeviceType::MAIN);

    if (measurements.size() < 3) {
        Logger::Log("PolarAlignment: 测量数据不足，需要3个点", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    // ===== 1) 用严格几何法（三点球面小圆）计算偏差与假极点 =====
    GeometricAlignmentResult geom;
    if (!calculatePolarDeviationGeometric(measurements[0], measurements[1], measurements[2], geom) || !geom.isValid) {
        Logger::Log("PolarAlignment: 几何法计算失败", LogLevel::ERROR, DeviceType::MAIN);
        return false; // 只用这一种方法，其它全删
    }

    // 将结果填入传统结构（注意：这是切平面分量，仅用于展示/指导，不参与 RA/DEC 加减）
    result.raDeviation  = geom.azimuthDeviation;   // du [deg]，真极切平面基底下
    result.decDeviation = geom.altitudeDeviation;  // dv [deg]
    result.totalDeviation = calculateTotalDeviation(result.raDeviation, result.decDeviation);
    result.measurements   = measurements;
    result.isSuccessful   = true;

    Logger::Log("PolarAlignment: 几何法校准完成 - 置信度: " + std::to_string(geom.confidence),
                LogLevel::INFO, DeviceType::MAIN);

    // 缓存假极点
    cachedFakePolarRA  = geom.fakePolarRA;
    cachedFakePolarDEC = geom.fakePolarDEC;
    isFakePolarCached  = true;

    Logger::Log("PolarAlignment: 假极轴位置已缓存 - RA: " + std::to_string(cachedFakePolarRA) +
                "°, DEC: " + std::to_string(cachedFakePolarDEC) + "°", LogLevel::INFO, DeviceType::MAIN);

    // ===== 2) 取得当前位置（第三次测量点），计算真极点 =====
    const SloveResults cur = measurements.last();
    const double currentRA  = cur.RA_Degree;
    const double currentDEC = cur.DEC_Degree;

    double realRA = 0.0, realDEC = 90.0;
    if (!isRealPolarCached) {
        if (!calculatePrecisePolarPosition(realRA, realDEC)) {
            Logger::Log("PolarAlignment: 计算真极轴位置失败", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
        this->realPolarRA  = realRA;
        this->realPolarDEC = realDEC;
        isRealPolarCached  = true;
        Logger::Log("PolarAlignment: 真极轴位置已缓存 - RA: " + std::to_string(realPolarRA) +
                    "°, DEC: " + std::to_string(realPolarDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    } else {
        realRA  = this->realPolarRA;
        realDEC = this->realPolarDEC;
        Logger::Log("PolarAlignment: 使用已缓存的真极轴位置 - RA: " + std::to_string(realRA) +
                    "°, DEC: " + std::to_string(realDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    }

    // ===== 3) 通过最小旋转 R(c→p) 计算目标点 =====
    // 仅当目标点未初始化时计算；你也可以每次都重算以覆盖旧值
    if (targetRA == 0.0 && targetDEC == 0.0) {
        computeTargetByAxisRotation(
            currentRA, currentDEC,
            realRA, realDEC,
            cachedFakePolarRA, cachedFakePolarDEC,
            targetRA, targetDEC
        );

        Logger::Log(
            "PolarAlignment: 目标点(严格球面法) - 真极: (" + std::to_string(realRA) + ", " + std::to_string(realDEC) +
            "), 假极: (" + std::to_string(cachedFakePolarRA) + ", " + std::to_string(cachedFakePolarDEC) +
            "), 当前位置: (" + std::to_string(currentRA) + ", " + std::to_string(currentDEC) +
            "), 目标位置: (" + std::to_string(targetRA) + ", " + std::to_string(targetDEC) + ")",
            LogLevel::INFO, DeviceType::MAIN
        );
    }

    // ===== 4) 生成指导语并发出信号 =====
    QString adjustmentRa, adjustmentDec;
    QString adjustmentGuide = generateAdjustmentGuide(adjustmentRa, adjustmentDec);
    Logger::Log("PolarAlignment: 调整指导: " + adjustmentGuide.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // 期望极点用于显示（如需要）
    double expectedRA = realRA, expectedDEC = realDEC;
    if (!isRealPolarCached) { // 正常不会进入，因为上面已缓存
        calculateExpectedPolarPosition(expectedRA, expectedDEC);
        this->realPolarRA  = expectedRA;
        this->realPolarDEC = expectedDEC;
        isRealPolarCached  = true;
    }

    // 解析数据将在emit adjustmentGuideData时保存

    // 发出信号：当前位置、目标位置、偏差值、假极轴、真极轴
    saveAndEmitAdjustmentGuideData(
        currentRAPosition, currentDECPosition,
        currentSolveResult.RA_0, currentSolveResult.DEC_0,
        currentSolveResult.RA_1, currentSolveResult.DEC_1,
        currentSolveResult.RA_2, currentSolveResult.DEC_2,
        currentSolveResult.RA_3, currentSolveResult.DEC_3,
        targetRA, targetDEC,
        result.raDeviation, result.decDeviation,
        adjustmentRa, adjustmentDec,
        cachedFakePolarRA, cachedFakePolarDEC,
        realRA, realDEC
    );
    isTargetPositionCached = true;

    return true;
}


bool PolarAlignment::guideUserAdjustment()
{
    Logger::Log("PolarAlignment: 指导用户调整（此函数已不再使用）", LogLevel::INFO, DeviceType::MAIN);
    
    // 这个函数现在不再被使用，因为极轴校准已经改为自动化流程
    // 保留函数是为了向后兼容，但总是返回false
    return false;
}

bool PolarAlignment::performFinalVerification()
{
    Logger::Log("PolarAlignment: 执行最终验证", LogLevel::INFO, DeviceType::MAIN);
    
    // 检查是否有有效的偏差计算结果
    if (!result.isSuccessful || measurements.size() < 3) {
        Logger::Log("PolarAlignment: 没有有效的偏差计算结果，无法进行最终验证", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取当前位置（最后一次测量点的位置）
    SloveResults currentPosition = measurements.last();
    Logger::Log("PolarAlignment: 当前位置 - RA: " + std::to_string(currentPosition.RA_Degree) + 
                "°, DEC: " + std::to_string(currentPosition.DEC_Degree) + "°", LogLevel::INFO, DeviceType::MAIN);
    

    // 进行最终确认验证拍摄，增加重试机制
    int finalVerificationAttempts = 0;
    const int maxFinalVerificationAttempts = 3;
    
    while (finalVerificationAttempts < maxFinalVerificationAttempts) {
        finalVerificationAttempts++;
        Logger::Log("PolarAlignment: 最终验证尝试 " + std::to_string(finalVerificationAttempts) + "/" + std::to_string(maxFinalVerificationAttempts), LogLevel::INFO, DeviceType::MAIN);
        
        if (captureAndAnalyze(1)) {
            SloveResults verificationResult = currentSolveResult;
            double verificationRA = verificationResult.RA_Degree;
            double verificationDEC = verificationResult.DEC_Degree;
            
            Logger::Log("PolarAlignment: 最终验证拍摄成功 - RA: " + std::to_string(verificationRA) + 
                        "°, DEC: " + std::to_string(verificationDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
            
            // 计算当前位置与目标位置的误差
            double raError = std::abs(verificationRA - targetRA);
            double decError = std::abs(verificationDEC - targetDEC);
            double totalError = std::sqrt(raError * raError + decError * decError);
            
            Logger::Log("PolarAlignment: 最终验证误差 - RA误差: " + std::to_string(raError) + 
                        "°, DEC误差: " + std::to_string(decError) + 
                        "°, 总误差: " + std::to_string(totalError) + "°", LogLevel::INFO, DeviceType::MAIN);
            
            // 根据配置的精度要求进行判断
            double precisionThreshold = config.finalVerificationThreshold;
            if (totalError < precisionThreshold) {
                Logger::Log("PolarAlignment: 最终确认验证成功，校准精度: " + std::to_string(totalError) + 
                            "°, 要求精度: " + std::to_string(precisionThreshold) + "°", LogLevel::INFO, DeviceType::MAIN);
                
                // 设置最终结果
                result.isSuccessful = true;
                result.raDeviation = raError;
                result.decDeviation = decError;
                result.totalDeviation = totalError;
                result.measurements = measurements;
                
                return true;
            } else {
                Logger::Log("PolarAlignment: 最终确认验证失败，当前精度: " + std::to_string(totalError) + 
                            "°, 要求精度: " + std::to_string(precisionThreshold) + "°, 继续调整", LogLevel::WARNING, DeviceType::MAIN);
                return false;
            }
        } else {
            Logger::Log("PolarAlignment: 最终确认验证拍摄失败，尝试次数 " + std::to_string(finalVerificationAttempts), LogLevel::WARNING, DeviceType::MAIN);
            if (finalVerificationAttempts >= maxFinalVerificationAttempts) {
                Logger::Log("PolarAlignment: 最终确认验证拍摄失败次数过多", LogLevel::ERROR, DeviceType::MAIN);
                return false;
            }
            continue; // 继续下一次尝试
        }
    }
    
    Logger::Log("PolarAlignment: 所有最终验证尝试都失败", LogLevel::ERROR, DeviceType::MAIN);
    return false;
}

bool PolarAlignment::performGuidanceAdjustmentStep()
{
    static int adjustmentAttempts = 0;
    adjustmentAttempts++;

    Logger::Log("PolarAlignment: 开始第 " + std::to_string(adjustmentAttempts) + " 次调整尝试",
                LogLevel::INFO, DeviceType::MAIN);

    if (!captureAndAnalyze(1)) {
        Logger::Log("PolarAlignment: 验证拍摄失败", LogLevel::WARNING, DeviceType::MAIN);
        if (isRunningFlag && !isPausedFlag) stateTimer.start(2000);
        return false;
    }

    if (!isTargetPositionCached) {
        Logger::Log("PolarAlignment: 目标未锁定，请先完成三点校准", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    // 当前实际解算点（只读）
    const double currentRA  = currentSolveResult.RA_Degree;
    const double currentDEC = currentSolveResult.DEC_Degree;

    Logger::Log("PolarAlignment: 单次验证 - 当前: (" + std::to_string(currentRA) + ", " +
                std::to_string(currentDEC) + "), 目标(固定): (" +
                std::to_string(targetRA) + ", " + std::to_string(targetDEC) + ")",
                LogLevel::INFO, DeviceType::MAIN);

    // —— 用固定目标计算东/北分量与球面距离（不再用 RA/DEC 线性差）——
    SingleShotGuide guide = delta_to_fixed_target(currentRA, currentDEC, targetRA, targetDEC);

    // 这些"像 az/alt 偏差"的输出仅为兼容旧接口（单位：度），实义是 EN 分量
    double east_deg  = guide.east_arcmin  / 60.0;
    double north_deg = guide.north_arcmin / 60.0;
    double total_deg = guide.distance_arcmin / 60.0;

    Logger::Log(
        "PolarAlignment: 偏差 - 东 " + std::to_string(guide.east_arcmin) + "′, 北 " +
        std::to_string(guide.north_arcmin) + "′, 距离 " + std::to_string(guide.distance_arcmin) +
        "′, 方位(自北顺时针) " + std::to_string(guide.bearing_deg_from_north) + "°",
        LogLevel::INFO, DeviceType::MAIN
    );

    // 生成指导文案（建议改为基于 EN 分量）
    QString adjustmentRa, adjustmentDec;
    QString adjustmentGuide = generateAdjustmentGuide(adjustmentRa, adjustmentDec);
    Logger::Log("PolarAlignment: 调整指导: " + adjustmentGuide.toStdString(),
                LogLevel::INFO, DeviceType::MAIN);

    // 发信号给 UI：把 EN 分量通过原参数传出（或新增字段更清晰）
    saveAndEmitAdjustmentGuideData(
        currentRAPosition, currentDECPosition,
        currentSolveResult.RA_0, currentSolveResult.DEC_0,
        currentSolveResult.RA_1, currentSolveResult.DEC_1,
        currentSolveResult.RA_2, currentSolveResult.DEC_2,
        currentSolveResult.RA_3, currentSolveResult.DEC_3,
        targetRA, targetDEC,         // 固定目标
        east_deg, north_deg,         // 兼容旧"az/alt"槽位
        adjustmentRa, adjustmentDec,
        cachedFakePolarRA, cachedFakePolarDEC,
        realPolarRA, realPolarDEC
    );

    // 达标判断用球面距离
    double precisionThreshold = config.finalVerificationThreshold; // 仍然"度"
    if (total_deg < precisionThreshold) {
        Logger::Log("PolarAlignment: 精度达标: " + std::to_string(total_deg) + "° < " +
                    std::to_string(precisionThreshold) + "°", LogLevel::INFO, DeviceType::MAIN);
        result.raDeviation  = east_deg;
        result.decDeviation = north_deg;
        result.totalDeviation = total_deg;
        adjustmentAttempts = 0;
        setState(PolarAlignmentState::FINAL_VERIFICATION);
    } else {
        Logger::Log("PolarAlignment: 精度未达标，继续调整",
                    LogLevel::WARNING, DeviceType::MAIN);
        if (isRunningFlag && !isPausedFlag) stateTimer.start(3000);
    }

    return true;
}

bool PolarAlignment::captureImage(int exposureTime)
{
    Logger::Log("PolarAlignment: 拍摄图像，曝光时间 " + std::to_string(exposureTime) + "ms", LogLevel::INFO, DeviceType::MAIN);
    if (!dpMainCamera) {
        Logger::Log("PolarAlignment: 相机设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 检查INDI客户端是否有效
    if (!indiServer) {
        Logger::Log("PolarAlignment: INDI客户端不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    uint32_t ret = indiServer->resetCCDFrameInfo(dpMainCamera);
    if (ret != QHYCCD_SUCCESS)
    {
        Logger::Log("INDI_Capture | indi resetCCDFrameInfo | failed", LogLevel::WARNING, DeviceType::CAMERA);
    }
    
    // 通过INDI接口拍摄图像
    Logger::Log("PolarAlignment: 开始调用INDI拍摄接口", LogLevel::INFO, DeviceType::MAIN);
    ret = indiServer->takeExposure(dpMainCamera, exposureTime / 1000.0);
    if (ret == QHYCCD_SUCCESS) {
        isCaptureEnd = false;
        lastCapturedImage = "/dev/shm/ccd_simulator.fits";

        Logger::Log("PolarAlignment: 拍摄命令发送成功，等待回调", LogLevel::INFO, DeviceType::MAIN);
        return true;
    } else {
        Logger::Log("PolarAlignment: 拍摄失败，错误代码: " + std::to_string(ret), LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
}



int PolarAlignment::selectOptimalSolveMode()
{
    // 模式说明：0=全局解析；1=加视场；2=加视场+RA/DEC窗口
    // 输入指标：
    //   - devAbs = sqrt(raDeviation^2 + decDeviation^2)（切平面偏差幅值，度）
    //   - distanceFromHistory = 最近一次解析点与当前位置的球面角距（度）
    // 目标：
    //   - 偏差大时优先 0；中等时 1；接近时 2；带轻微滞回避免频繁切换

    bool hasValidHistory = isAnalysisSuccessful(currentSolveResult);
    if (!hasValidHistory) {
        Logger::Log("PolarAlignment: 无历史解析数据，选择模式1（视场）", LogLevel::INFO, DeviceType::MAIN);
        return 1;
    }

    // 计算 devAbs（若无有效 result 则置大）
    double devAbs = (result.isSuccessful && !std::isnan(result.raDeviation) && !std::isnan(result.decDeviation))
                    ? std::hypot(result.raDeviation, result.decDeviation) : 1e9;

    // 计算与历史的角距
    double currentRA, currentDEC;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA, currentDEC);
    currentRA = Tools::HourToDegree(currentRA);
    double distanceFromHistory = calculateSphericalDistance(currentRA, currentDEC,
                                                            currentSolveResult.RA_Degree, currentSolveResult.DEC_Degree);

    // 阈值（可通过 config 调整）
    double th2 = std::min(config.solveMode2MaxOffsetDeg, 5.0);   // 模式2窗口硬上限 5°
    double th1 = std::max(config.solveMode1MaxOffsetDeg, th2 + 1.0); // 模式1阈值 ≥ 模式2阈值

    // 基于两个指标的决策：任一指标大则降级
    // - 若 devAbs 很小且 distance 小 → 模式2
    // - 若二者处于中间 → 模式1
    // - 若 devAbs 或 distance 很大 → 模式0
    int chosen = 0;
    if (distanceFromHistory <= th2 && devAbs <= config.largeDeviationThresholdDeg) {
        chosen = 2;
    } else if (distanceFromHistory <= th1 && devAbs <= (config.largeDeviationThresholdDeg * 2.0)) {
        chosen = 1;
    } else {
        chosen = 0;
    }

    Logger::Log(
        "PolarAlignment: 解析模式选择 - devAbs=" + std::to_string(devAbs) +
        "°, distHist=" + std::to_string(distanceFromHistory) +
        "°, th2=" + std::to_string(th2) +
        "°, th1=" + std::to_string(th1) +
        ", chosen=" + std::to_string(chosen),
        LogLevel::INFO, DeviceType::MAIN
    );

    return chosen;
}

double PolarAlignment::calculateSphericalDistance(double ra1, double dec1, double ra2, double dec2)
{
    // 使用球面三角法计算两点间的角距离（度）
    double ra1_rad = ra1 * M_PI / 180.0;
    double dec1_rad = dec1 * M_PI / 180.0;
    double ra2_rad = ra2 * M_PI / 180.0;
    double dec2_rad = dec2 * M_PI / 180.0;
    
    // 球面余弦定理
    double cos_distance = sin(dec1_rad) * sin(dec2_rad) + 
                         cos(dec1_rad) * cos(dec2_rad) * cos(ra1_rad - ra2_rad);
    
    // 防止数值误差导致的域外值
    cos_distance = std::max(-1.0, std::min(1.0, cos_distance));
    
    double distance_rad = acos(cos_distance);
    double distance_deg = distance_rad * 180.0 / M_PI;
    
    return distance_deg;
}

bool PolarAlignment::solveImage(const QString& imageFile)
{
    Logger::Log("PolarAlignment: 解析图像 " + imageFile.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    
    int focalLength = config.focalLength; // 焦距（毫米）
    double cameraWidth = config.cameraWidth; // 相机传感器宽度（毫米）
    double cameraHeight = config.cameraHeight; // 相机传感器高度（毫米）
    
    // 智能选择解析模式
    int solveMode = selectOptimalSolveMode();
    double lastRA = 0.0, lastDEC = 0.0;
    
    // 如果选择模式2，使用历史位置数据
    if (solveMode == 2) {
        lastRA = currentSolveResult.RA_Degree;
        lastDEC = currentSolveResult.DEC_Degree;
        Logger::Log("PolarAlignment: 使用模式2解析，参考位置 - RA: " + std::to_string(lastRA) + "°, DEC: " + std::to_string(lastDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    }
    
    // 调用图像解析功能
    bool ret = Tools::PlateSolve(imageFile, focalLength, cameraWidth, cameraHeight, false, solveMode, lastRA, lastDEC);
    if(!ret)
    {
        Logger::Log("PolarAlignment: 图像解析命令执行失败", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    
    Logger::Log("PolarAlignment: 图像开始解析", LogLevel::INFO, DeviceType::MAIN);
    return true;
}

bool PolarAlignment::moveTelescope(double ra, double dec)
{
    Logger::Log("PolarAlignment: 移动望远镜 RA=" + std::to_string(ra) + " DEC=" + std::to_string(dec),
                LogLevel::INFO, DeviceType::MAIN);

    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    // --- 获取当前 JNOW 位置（RA:hour → deg） ---
    double currentRA_Hours = 0.0, currentDEC_Degree = 0.0;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA_Hours, currentDEC_Degree);
    double currentRA  = Tools::HourToDegree(currentRA_Hours); // deg
    double currentDEC = currentDEC_Degree;                    // deg

    // --- 计算新位置（相对 → 绝对），并做规范化 ---
    auto norm360 = [](double d){ d = fmod(d, 360.0); if (d < 0) d += 360.0; return d; };
    auto clampDec = [](double d){ return std::max(-90.0, std::min(90.0, d)); };

    double newRA  = norm360(currentRA + ra);       // 环回到 [0,360)
    double newDEC = clampDec(currentDEC + dec);    // 限幅到 [-90,+90]

    Logger::Log("PolarAlignment: 当前位置 RA=" + std::to_string(currentRA) + " DEC=" + std::to_string(currentDEC) +
                ", 目标位置 RA=" + std::to_string(newRA) + " DEC=" + std::to_string(newDEC),
                LogLevel::INFO, DeviceType::MAIN);

    if (dpMount != nullptr)
    {
        // --- 若已在移动，先等它停（最多 30s） ---
        QString currentStat;
        indiServer->getTelescopeStatus(dpMount, currentStat);
        if (currentStat == "Moving") {
            Logger::Log("PolarAlignment: 赤道仪正在移动中，等待完成后再发送新命令", LogLevel::WARNING, DeviceType::MAIN);
            int waitTime = 0;
            while (currentStat == "Moving" && waitTime < 30000) { // 最多等待30秒
                QThread::msleep(500);
                waitTime += 500;
                indiServer->getTelescopeStatus(dpMount, currentStat);
            }
            if (waitTime >= 30000) {
                Logger::Log("PolarAlignment: 等待赤道仪移动完成超时(30s)", LogLevel::ERROR, DeviceType::MAIN);
                return false;
            }
        }

        // --- 下发非阻塞 GOTO（JNOW） ---
        INDI::PropertyNumber property = NULL;
        indiServer->slewTelescopeJNowNonBlock(
            dpMount,
            Tools::DegreeToHour(newRA),
            newDEC,
            true
        );

    }

    return true;
}


bool PolarAlignment::moveTelescopeToAbsolutePosition(double ra, double dec)
{
    Logger::Log("PolarAlignment: 移动到绝对位置 RA=" + std::to_string(ra) + " DEC=" + std::to_string(dec), LogLevel::INFO, DeviceType::MAIN);
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取当前望远镜位置
    double currentRA_Hours, currentDEC_Degree;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA_Hours, currentDEC_Degree);
    double currentRA = Tools::HourToDegree(currentRA_Hours);
    double currentDEC = currentDEC_Degree;
    
    Logger::Log("PolarAlignment: 当前位置 RA=" + std::to_string(currentRA) + " DEC=" + std::to_string(currentDEC) + 
                ", 目标位置 RA=" + std::to_string(ra) + " DEC=" + std::to_string(dec), LogLevel::INFO, DeviceType::MAIN);
    
    // 发送移动命令到绝对位置
    if(dpMount!=NULL)
    {
        // 检查赤道仪是否已经在移动
        QString currentStat;
        indiServer->getTelescopeStatus(dpMount, currentStat);
        if(currentStat == "Moving") {
            Logger::Log("PolarAlignment: 赤道仪正在移动中，等待完成后再发送绝对位置移动命令", LogLevel::WARNING, DeviceType::MAIN);
            // 等待当前移动完成
            int waitTime = 0;
            while(currentStat == "Moving" && waitTime < 30000) { // 最多等待30秒
                QThread::msleep(500);
                waitTime += 500;
                indiServer->getTelescopeStatus(dpMount, currentStat);
            }
            if(waitTime >= 30000) {
                Logger::Log("PolarAlignment: 等待赤道仪移动完成超时", LogLevel::ERROR, DeviceType::MAIN);
                return false;
            }
        }
        
        indiServer->slewTelescopeJNowNonBlock(dpMount, Tools::DegreeToHour(ra), dec, true);
    }
    
    return true;
}

void PolarAlignment::setCaptureEnd(bool isEnd)
{
    Logger::Log("PolarAlignment: 设置拍摄完成标志: " + std::to_string(isEnd), LogLevel::INFO, DeviceType::MAIN);
    isCaptureEnd = isEnd;
}


bool PolarAlignment::waitForCaptureComplete()
{
    Logger::Log("PolarAlignment: 等待拍摄完成", LogLevel::INFO, DeviceType::MAIN);
    
    
    captureAndAnalysisTimer.start(config.captureAndAnalysisTimeout);
    QEventLoop loop;
    QTimer checkTimer;
    checkTimer.setInterval(100);
    
    connect(&checkTimer, &QTimer::timeout, [&]() {
        if (isCaptureEnd) { // 5秒后假设完成
            if (true) {
                // if (currentState == PolarAlignmentState::CHECKING_POLAR_POINT) {
                //     lastCapturedImage = QString("/home/quarcs/workspace/QUARCS/testimage1/1.fits");
                // }
                if (currentState == PolarAlignmentState::FIRST_CAPTURE) {
                    lastCapturedImage = QString("/home/quarcs/workspace/QUARCS/testimage1/2.fits");
                }
                else if (currentState == PolarAlignmentState::SECOND_CAPTURE) {
                    lastCapturedImage = QString("/home/quarcs/workspace/QUARCS/testimage1/3.fits");
                }
                // else if (currentState == PolarAlignmentState::THIRD_CAPTURE) {
                //     lastCapturedImage = QString("/home/quarcs/workspace/QUARCS/testimage1/4.fits");
                // }else {
                //     if (testimage > 8) {
                //         testimage = 5;
                //     }
                //     if (testimage < 1) {
                //         testimage = 1;
                //     }
                //     lastCapturedImage = QString("/home/quarcs/workspace/QUARCS/testimage1/%1.fits").arg(testimage);
                //     testimage++;
                // }
            }
            else {
                lastCapturedImage = QString("/dev/shm/ccd_simulator.fits");
            }
            cv::Mat image;
            cv::Mat originalImage16;
            cv::Mat image16;

            int status = Tools::readFits(lastCapturedImage.toLocal8Bit().constData(), image);
            Logger::Log("PolarAlignment: 读取图像: " + lastCapturedImage.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            if (status != 0)
            {
                Logger::Log("Failed to read FITS file: " + lastCapturedImage.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                return status;
            }
            if (image.type() == CV_8UC1 || image.type() == CV_8UC3 || image.type() == CV_16UC1)
            {
                originalImage16 = Tools::convert8UTo16U_BayerSafe(image, false);
                image.release();
            }
            else
            {
                Logger::Log("The current image data type is not supported for processing.", LogLevel::WARNING, DeviceType::MAIN);
                return -1;
            }
            int binning = 1;
            int currentSize = originalImage16.cols;

            Logger::Log("Starting median blur...", LogLevel::INFO, DeviceType::CAMERA);
            cv::medianBlur(originalImage16, originalImage16, 3);
            Logger::Log("Median blur applied successfully.", LogLevel::INFO, DeviceType::CAMERA);

            // 逐步增加binning直到像素大小小于等于548
            while (currentSize > 548 && binning <= 16)
            {
                binning *= 2;
                currentSize = originalImage16.cols / binning;
            }

            // 限制最大binning为16
            if (binning > 16)
            {
                binning = 16;
            }
               
            image16 = Tools::processMatWithBinAvg(originalImage16, binning, binning, false, true);
           
            originalImage16.release();

            Tools::SaveMatToFITS(image16);
            image16.release();

            lastCapturedImage = QString("/dev/shm/MatToFITS.fits");
            loop.quit();
            // 保存第一个调整点的图像
            if (currentState == PolarAlignmentState::FIRST_CAPTURE) {
                // 创建目标目录（如果不存在）
                QString targetDir = "/home/quarcs/images/";
                QDir dir;
                if (!dir.exists(targetDir)) {
                    if (dir.mkpath(targetDir)) {
                        Logger::Log("PolarAlignment: 创建目录成功: " + targetDir.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    } else {
                        Logger::Log("PolarAlignment: 创建目录失败: " + targetDir.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    }
                }
                
                // 生成新的文件名：第一个调整点_时间戳.fits
                QDateTime currentTime = QDateTime::currentDateTime();
                QString timestamp = currentTime.toString("yyyyMMdd_hhmmss");
                QString newFileName = QString("第一个调整点_%1.fits").arg(timestamp);
                QString targetPath = targetDir + newFileName;
                
                // 复制文件
                if (QFile::copy(lastCapturedImage, targetPath)) {
                    Logger::Log("PolarAlignment: 第一个调整点图像已保存: " + targetPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                } else {
                    Logger::Log("PolarAlignment: 第一个调整点图像保存失败: " + targetPath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                }
            }else if (currentState == PolarAlignmentState::SECOND_CAPTURE){
                // 保存第二个调整点的图像
                // 创建目标目录（如果不存在）
                QString targetDir = "/home/quarcs/images/";
                QDir dir;
                if (!dir.exists(targetDir)) {
                    if (dir.mkpath(targetDir)) {
                        Logger::Log("PolarAlignment: 创建目录成功: " + targetDir.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    } else {
                        Logger::Log("PolarAlignment: 创建目录失败: " + targetDir.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    }
                }
                
                // 生成新的文件名：第二个调整点_时间戳.fits
                QDateTime currentTime = QDateTime::currentDateTime();
                QString timestamp = currentTime.toString("yyyyMMdd_hhmmss");
                QString newFileName = QString("第二个调整点_%1.fits").arg(timestamp);
                QString targetPath = targetDir + newFileName;
                
                // 复制文件
                if (QFile::copy(lastCapturedImage, targetPath)) {
                    Logger::Log("PolarAlignment: 第二个调整点图像已保存: " + targetPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                } else {
                    Logger::Log("PolarAlignment: 第二个调整点图像保存失败: " + targetPath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                }
            }else if (currentState == PolarAlignmentState::THIRD_CAPTURE){
                // 保存第三个调整点的图像
                // 创建目标目录（如果不存在）
                QString targetDir = "/home/quarcs/images/";
                QDir dir;
                if (!dir.exists(targetDir)) {
                    if (dir.mkpath(targetDir)) {
                        Logger::Log("PolarAlignment: 创建目录成功: " + targetDir.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    } else {
                        Logger::Log("PolarAlignment: 创建目录失败: " + targetDir.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    }
                }
                // 生成新的文件名：第三个调整点_时间戳.fits
                QDateTime currentTime = QDateTime::currentDateTime();
                QString timestamp = currentTime.toString("yyyyMMdd_hhmmss");
                QString newFileName = QString("第三个调整点_%1.fits").arg(timestamp);
                QString targetPath = targetDir + newFileName;
                
                // 复制文件
                if (QFile::copy(lastCapturedImage, targetPath)) {
                    Logger::Log("PolarAlignment: 第三个调整点图像已保存: " + targetPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                } else {
                    Logger::Log("PolarAlignment: 第三个调整点图像保存失败: " + targetPath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                }
            }
        }
    });
    
    connect(&captureAndAnalysisTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    checkTimer.start();
    loop.exec();
    checkTimer.stop();
    captureAndAnalysisTimer.stop();
    
    return isCaptureEnd;
}

bool PolarAlignment::waitForSolveComplete()
{
    Logger::Log("PolarAlignment: 等待解析完成. 等待中...", LogLevel::INFO, DeviceType::MAIN);
    return Tools::isSolveImageFinish(); 
}

bool PolarAlignment::waitForMovementComplete()
{
    Logger::Log("PolarAlignment: 等待移动完成", LogLevel::INFO, DeviceType::MAIN);
    
    movementTimer.start(config.movementTimeout);
    QEventLoop loop;
    QTimer checkTimer;
    checkTimer.setInterval(500);
    
    connect(&checkTimer, &QTimer::timeout, [&]() {
        if (!dpMount) {
            loop.quit();
            return;
        }
        
        QString status;
        indiServer->getTelescopeStatus(dpMount, status);
        if (status == "Idle") {
            Logger::Log("PolarAlignment: 移动完成", LogLevel::INFO, DeviceType::MAIN);
            loop.quit();
        }
    });
    
    connect(&movementTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    checkTimer.start();
    loop.exec();
    checkTimer.stop();
    movementTimer.stop();
    
    return !movementTimer.isActive();
}

bool PolarAlignment::isAnalysisSuccessful(const SloveResults& result)
{
    // 检查解析结果是否有效
    return (result.RA_Degree != -1 && result.DEC_Degree != -1);
}

void PolarAlignment::handleAnalysisFailure(int attempt)
{
    Logger::Log("PolarAlignment: 分析失败，尝试次数 " + std::to_string(attempt), LogLevel::WARNING, DeviceType::MAIN);
    if (attempt >= config.maxRetryAttempts) {
        handleCriticalFailure();
    } else {
    }
}

void PolarAlignment::handleCriticalFailure()
{
    Logger::Log("PolarAlignment: 严重失败，返回初始位置", LogLevel::ERROR, DeviceType::MAIN);
    result.isSuccessful = false;
    result.errorMessage = "解析成功率太低，请重新调整拍摄位置，注意光线干扰";
    setState(PolarAlignmentState::USER_INTERVENTION);
}


double PolarAlignment::calculateTotalDeviation(double raDev, double decDev)
{
    // 计算总偏差（欧几里得距离）
    return std::sqrt(raDev * raDev + decDev * decDev);
}

double PolarAlignment::normalizeAngle360(double angle)
{
    while (angle < 0) angle += 360.0;
    while (angle >= 360.0) angle -= 360.0;
    return angle;
}

double PolarAlignment::normalizeAngle180(double angle)
{
    while (angle < -180.0) angle += 360.0;
    while (angle > 180.0) angle -= 360.0;
    return angle;
}

// ==================== 地理位置相关函数实现 ====================

bool PolarAlignment::calculateExpectedPolarPosition(double& expectedRA, double& expectedDEC)
{
    // 如果已经缓存了真极轴位置，直接返回缓存值
    if (isRealPolarCached) {
        expectedRA = realPolarRA;
        expectedDEC = realPolarDEC;
        Logger::Log("PolarAlignment: 返回缓存的目标位置 - RA: " + std::to_string(expectedRA) + 
                    "°, DEC: " + std::to_string(expectedDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
    
    Logger::Log("PolarAlignment: 计算期望的极点位置", LogLevel::INFO, DeviceType::MAIN);
    
    // 使用精确的极点计算函数
    return calculatePrecisePolarPosition(expectedRA, expectedDEC);
}

bool PolarAlignment::isNorthernHemisphere() const
{
    // 根据纬度判断半球
    // 正值表示北半球，负值表示南半球
    bool isNorth = config.latitude >= 0.0;
    Logger::Log("PolarAlignment: 地理位置配置 - 纬度: " + std::to_string(config.latitude) + 
                "°, 经度: " + std::to_string(config.longitude) + 
                "°, 判断为: " + (isNorth ? "北半球" : "南半球"), LogLevel::INFO, DeviceType::MAIN);
    return isNorth;
}

bool PolarAlignment::calculatePrecisePolarPosition(double& expectedRA, double& expectedDEC)
{
    Logger::Log("PolarAlignment: 计算精确的极点位置", LogLevel::INFO, DeviceType::MAIN);
    
    // 获取当前系统时间
    QDateTime currentTime = QDateTime::currentDateTime();
    QDateTime utcTime = currentTime.toUTC();
    
    Logger::Log("PolarAlignment: 当前UTC时间: " + utcTime.toString("yyyy-MM-dd hh:mm:ss").toStdString(), LogLevel::INFO, DeviceType::MAIN);
    
    // 计算儒略日（Julian Day）
    int year = utcTime.date().year();
    int month = utcTime.date().month();
    int day = utcTime.date().day();
    int hour = utcTime.time().hour();
    int minute = utcTime.time().minute();
    int second = utcTime.time().second();
    
    // 计算儒略日
    double jd = calculateJulianDay(year, month, day, hour, minute, second);
    
    Logger::Log("PolarAlignment: 儒略日: " + std::to_string(jd), LogLevel::INFO, DeviceType::MAIN);
    
    // 计算格林尼治恒星时（GST）
    double gst = calculateGreenwichSiderealTime(jd);
    
    Logger::Log("PolarAlignment: 格林尼治恒星时: " + std::to_string(gst) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 计算本地恒星时（LST）
    double lst = gst + config.longitude;
    if (lst < 0) lst += 360.0;
    if (lst >= 360.0) lst -= 360.0;
    
    Logger::Log("PolarAlignment: 本地恒星时: " + std::to_string(lst) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 修正：天极的RA坐标是固定的，不随时间变化
    if (isNorthernHemisphere()) {
        expectedRA = 0.0;        // 北天极的RA = 0°（固定）
        expectedDEC = 90.0;      // 北天极DEC = 90°（固定）
    } else {
        expectedRA = 180.0;      // 南天极的RA = 12h = 180°（固定）
        expectedDEC = -90.0;     // 南天极DEC = -90°（固定）
    }
    
    // 应用岁差修正（主要影响RA，对DEC影响很小）
    double t = (jd - 2451545.0) / 36525.0;
    
    // 岁差对天极RA的影响（主要修正）
    double precessionRA = 50.29 * t + 0.000222 * t * t; // 角秒/年
    expectedRA += precessionRA / 3600.0; // 转换为度
    
    // 岁差对天极DEC的影响（很小，但需要修正）
    double precessionDEC = 20.04 * t; // 角秒/年
    expectedDEC += precessionDEC / 3600.0; // 转换为度
    
    // 确保RA在0-360度范围内
    expectedRA = normalizeAngle360(expectedRA);
    
    // 确保DEC在合理范围内
    if (expectedDEC > 90.0) expectedDEC = 90.0;
    if (expectedDEC < -90.0) expectedDEC = -90.0;
    
    return true;
}

// ==================== 天文计算辅助函数 ====================

double PolarAlignment::calculateJulianDay(int year, int month, int day, int hour, int minute, int second)
{
    // 精确的儒略日计算（基于Meeus算法）
    
    // 处理1月和2月
    if (month <= 2) {
        year -= 1;
        month += 12;
    }
    
    // 计算世纪数
    int a = year / 100;
    int b = 2 - a + a / 4;
    
    // 计算儒略日
    double jd = 365.25 * (year + 4716) + 30.6001 * (month + 1) + day + b - 1524.5;
    
    // 添加时间部分（转换为小数天）
    double timeFraction = (hour + minute / 60.0 + second / 3600.0) / 24.0;
    jd += timeFraction;
    
    // 修正：对于格里高利历，需要额外的修正
    if (year > 1582 || (year == 1582 && month > 10) || (year == 1582 && month == 10 && day >= 15)) {
        // 格里高利历，无需额外修正
    } else {
        // 儒略历，需要修正
        jd -= b;
    }
    
    return jd;
}

double PolarAlignment::calculateGreenwichSiderealTime(double jd)
{
    // 简化的格林尼治恒星时计算（基于Meeus算法，适用于现代应用）
    
    // 计算从J2000.0开始的天数
    double t = (jd - 2451545.0) / 36525.0;
    
    // 计算格林尼治恒星时（度）
    // 基于Meeus "Astronomical Algorithms" 第12章
    double gst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 
                 0.000387933 * t * t - t * t * t / 38710000.0;
    
    // 归一化到0-360度范围
    gst = normalizeAngle360(gst);
    
    return gst;
}

QString PolarAlignment::generateAdjustmentGuide(QString &adjustmentRa, QString &adjustmentDec)
{
    Logger::Log("PolarAlignment: 生成调整指导", LogLevel::INFO, DeviceType::MAIN);
    
    if (!result.isSuccessful) {
        return "无法生成调整指导，请重新开始校准";
    }
    
    QString guide = "";
    
    // 可选：获取当前地平坐标，仅用于日志
    double currentAzimuth = 0.0, currentAltitude = 0.0;
    getCurrentHorizontalCoordinates(currentAzimuth, currentAltitude);
    
    // 计算机械调整量：根据偏差大小在两种方法间平滑切换
    double azimuthAdjustment = 0.0;
    double altitudeAdjustment = 0.0;
    double observerLat = 0.0, observerLon = 0.0, observerElev = 0.0;
    if (getObserverLocation(observerLat, observerLon, observerElev)) {
        // 计算偏差幅值
        double devAbs = std::hypot(result.raDeviation, result.decDeviation);
        double wJac = 0.0; // 雅可比权重
        // 分段平滑：小于 small 完全雅可比；大于 large 完全非线性；中间线性插值
        if (devAbs <= config.smallDeviationThresholdDeg) {
            wJac = 1.0;
        } else if (devAbs >= config.largeDeviationThresholdDeg) {
            wJac = 0.0;
        } else {
            double t = (config.largeDeviationThresholdDeg - devAbs) /
                       (config.largeDeviationThresholdDeg - config.smallDeviationThresholdDeg);
            wJac = std::clamp(t, 0.0, 1.0);
        }

        // 方法A：机械轴雅可比（小偏差更准）
        double azJac = result.raDeviation, altJac = result.decDeviation;
        if (!calculatePolarAlignmentAdjustment(result.raDeviation, result.decDeviation,
                                              observerLat, observerLon, azJac, altJac)) {
            azJac = result.raDeviation;
            altJac = result.decDeviation;
        }

        // 方法B：非线性机械法（大偏差更稳）
        // 1) 构造 p（与 calculatePolarAlignmentAdjustment 一致）：优先假极轴，否则理想天极
        auto dot3 = [](const CartesianCoordinates& a, const CartesianCoordinates& b){ return a.x*b.x + a.y*b.y + a.z*b.z; };
        auto norm3 = [&](const CartesianCoordinates& a){ double L = sqrt(dot3(a,a)); return (L>0) ? CartesianCoordinates{a.x/L, a.y/L, a.z/L} : CartesianCoordinates{0,0,0}; };
        auto rodrigues = [&](const CartesianCoordinates& k_unit, double angle, const CartesianCoordinates& v){
            double c = cos(angle), s = sin(angle);
            CartesianCoordinates kxv = crossProduct(k_unit, v);
            double kdotv = dot3(k_unit, v);
            CartesianCoordinates term1 = { v.x * c, v.y * c, v.z * c };
            CartesianCoordinates term2 = { kxv.x * s, kxv.y * s, kxv.z * s };
            CartesianCoordinates term3 = { k_unit.x * kdotv * (1 - c), k_unit.y * kdotv * (1 - c), k_unit.z * kdotv * (1 - c) };
            return CartesianCoordinates{ term1.x + term2.x + term3.x, term1.y + term2.y + term3.y, term1.z + term2.z + term3.z };
        };

        CartesianCoordinates Up   = {0.0, 0.0, 1.0};
        CartesianCoordinates East = {1.0, 0.0, 0.0};

        double latRad = observerLat * M_PI / 180.0;
        CartesianCoordinates p;
        bool usedMeasuredPole = false;
        if (isFakePolarCached) {
            double poleAz=0.0, poleAlt=0.0;
            double fakeRA_hours = cachedFakePolarRA / 15.0;
            if (convertRADECToHorizontal(fakeRA_hours, cachedFakePolarDEC, observerLat, observerLon, poleAz, poleAlt)) {
                double azRad = poleAz * M_PI / 180.0;
                double altRad2 = poleAlt * M_PI / 180.0;
                p.x = sin(azRad) * cos(altRad2);
                p.y = cos(azRad) * cos(altRad2);
                p.z = sin(altRad2);
                usedMeasuredPole = true;
            }
        }
        if (!usedMeasuredPole) {
            double altAbs = std::fabs(latRad);
            double az = (observerLat >= 0.0) ? 0.0 : M_PI;
            p.x = sin(az) * cos(altAbs);
            p.y = cos(az) * cos(altAbs) * ((observerLat >= 0.0) ? 1.0 : -1.0);
            p.z = sin(altAbs);
        }

        // 2) 切平面基 e_alt_like, e_az_like
        double dotUpP = dot3(Up, p);
        CartesianCoordinates t;
        if (std::fabs(std::fabs(dotUpP) - 1.0) < 1e-6) {
            double dotEastP = dot3(East, p);
            t = { East.x - dotEastP * p.x, East.y - dotEastP * p.y, East.z - dotEastP * p.z };
        } else {
            t = { Up.x - dotUpP * p.x, Up.y - dotUpP * p.y, Up.z - dotUpP * p.z };
        }
        CartesianCoordinates e_alt_like = normalizeVector(t);
        CartesianCoordinates e_az_like  = crossProduct(p, e_alt_like);
        e_az_like = normalizeVector(e_az_like);

        // 3) 从切平面有限角旋转得到目标极轴 p_target
        double u = result.raDeviation * M_PI / 180.0;
        double v = result.decDeviation * M_PI / 180.0;
        CartesianCoordinates p1 = rodrigues(e_az_like, u, p);
        CartesianCoordinates p_target = rodrigues(e_alt_like, v, p1);
        p_target = normalizeVector(p_target);

        // 4) 由 ENU 分量直接计算当前与目标的 az/alt
        auto toAzAlt = [](const CartesianCoordinates& q){
            double alt = asin(std::clamp(q.z, -1.0, 1.0));
            double az = atan2(q.x, q.y);
            double azDeg = az * 180.0 / M_PI; if (azDeg < 0) azDeg += 360.0;
            double altDeg = alt * 180.0 / M_PI;
            return std::pair<double,double>(azDeg, altDeg);
        };
        auto cur = toAzAlt(p);
        auto tgt = toAzAlt(p_target);
        auto norm180 = [](double ang){ while (ang > 180.0) ang -= 360.0; while (ang < -180.0) ang += 360.0; return ang; };
        double azNonlin = norm180(tgt.first  - cur.first);
        double altNonlin = norm180(tgt.second - cur.second);

        // 平滑融合
        azimuthAdjustment  = wJac * azJac  + (1.0 - wJac) * azNonlin;
        altitudeAdjustment = wJac * altJac + (1.0 - wJac) * altNonlin;
    } else {
        // 无法获取观测者位置，退化为直接映射
        azimuthAdjustment = result.raDeviation;
        altitudeAdjustment = result.decDeviation;
    }
    
    // 数值健壮性
    if (std::isnan(azimuthAdjustment) || std::isnan(altitudeAdjustment)) {
        Logger::Log("PolarAlignment: 检测到NaN值，重置为0", LogLevel::WARNING, DeviceType::MAIN);
        azimuthAdjustment = 0.0;
        altitudeAdjustment = 0.0;
    }
    
    // 方位角指导
    if (std::abs(azimuthAdjustment) > 0.1) {
        if (azimuthAdjustment > 0) {
            guide += QString("方位角: 向西调整 %1 度; ").arg(std::abs(azimuthAdjustment), 0, 'f', 2);
            adjustmentRa = QString("向西调整 %1 度; ").arg(std::abs(azimuthAdjustment), 0, 'f', 2);
        } else {
            guide += QString("方位角: 向东调整 %1 度; ").arg(std::abs(azimuthAdjustment), 0, 'f', 2);
            adjustmentRa = QString("向东调整 %1 度; ").arg(std::abs(azimuthAdjustment), 0, 'f', 2);
        }
    } else {
        guide += "方位角: 已对齐; ";
        adjustmentRa = "已对齐; ";
    }
    
    // 高度角指导
    if (std::abs(altitudeAdjustment) > 0.1) {
        if (altitudeAdjustment > 0) {
            guide += QString("高度角: 向上调整 %1 度; ").arg(std::abs(altitudeAdjustment), 0, 'f', 2);
            adjustmentDec = QString("向上调整 %1 度; ").arg(std::abs(altitudeAdjustment), 0, 'f', 2);
        } else {
            guide += QString("高度角: 向下调整 %1 度; ").arg(std::abs(altitudeAdjustment), 0, 'f', 2);
            adjustmentDec = QString("向下调整 %1 度; ").arg(std::abs(altitudeAdjustment), 0, 'f', 2);
        }
    } else {
        guide += "高度角: 已对齐; ";
        adjustmentDec = "已对齐; ";
    }
    
    double totalDeviation = sqrt(azimuthAdjustment * azimuthAdjustment + altitudeAdjustment * altitudeAdjustment);
    guide += QString("总偏差: %1 度").arg(totalDeviation, 0, 'f', 2);

    // 记录融合细节，便于观察是否存在突变
    double devAbsLog = std::hypot(result.raDeviation, result.decDeviation);
    double wJacLog;
    if (devAbsLog <= config.smallDeviationThresholdDeg) wJacLog = 1.0;
    else if (devAbsLog >= config.largeDeviationThresholdDeg) wJacLog = 0.0;
    else wJacLog = std::clamp((config.largeDeviationThresholdDeg - devAbsLog) /
                               (config.largeDeviationThresholdDeg - config.smallDeviationThresholdDeg), 0.0, 1.0);

    Logger::Log(
        "PolarAlignment: 融合详情 - 偏差幅值: " + std::to_string(devAbsLog) +
        "°, wJac: " + std::to_string(wJacLog) +
        ", 小阈值: " + std::to_string(config.smallDeviationThresholdDeg) +
        "°, 大阈值: " + std::to_string(config.largeDeviationThresholdDeg) + "°",
        LogLevel::INFO, DeviceType::MAIN
    );
    
    Logger::Log("PolarAlignment: 生成极轴校准调整指导(解耦) - 当前方位角: " + std::to_string(currentAzimuth) +
                "°, 当前高度角: " + std::to_string(currentAltitude) +
                "°, 方位角调整: " + std::to_string(azimuthAdjustment) +
                "°, 高度角调整: " + std::to_string(altitudeAdjustment) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    return guide;
}

bool PolarAlignment::adjustForObstacle()
{
    Logger::Log("PolarAlignment: 调整避开遮挡物，尝试次数 " + std::to_string(currentAdjustmentAttempt + 1), LogLevel::INFO, DeviceType::MAIN);
    
    if (currentAdjustmentAttempt >= config.maxAdjustmentAttempts) {
        Logger::Log("PolarAlignment: 达到最大调整尝试次数", LogLevel::ERROR, DeviceType::MAIN);
        result.isSuccessful = false;
        result.errorMessage = "多次调整后仍无法避开遮挡物，请寻找视野开阔的位置";
        return false;
    }
    
    // 减小移动角度
    currentRAAngle *= config.adjustmentAngleReduction;
    currentDECAngle *= config.adjustmentAngleReduction;
    
    Logger::Log("PolarAlignment: 调整角度 - RA: " + std::to_string(currentRAAngle) + 
                ", DEC: " + std::to_string(currentDECAngle), LogLevel::INFO, DeviceType::MAIN);
    
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 执行调整移动
    // moveTelescope函数内部已经包含了等待移动完成的逻辑
    bool success = moveTelescope(currentRAAngle, currentDECAngle);
    
    if (success) {
        currentAdjustmentAttempt++;
        Logger::Log("PolarAlignment: 调整成功，尝试次数 " + std::to_string(currentAdjustmentAttempt), LogLevel::INFO, DeviceType::MAIN);
        return true;
    } else {
        Logger::Log("PolarAlignment: 调整失败", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
}

bool PolarAlignment::isFirstCaptureFailure()
{
    // 判断是否为第一次拍摄失败
    return (currentState == PolarAlignmentState::FIRST_CAPTURE || 
            currentState == PolarAlignmentState::FIRST_CAPTURE_LONG_EXPOSURE || 
            currentState == PolarAlignmentState::FIRST_CAPTURE_DEC_AVOIDANCE ||
            currentState == PolarAlignmentState::WAITING_FIRST_DEC_AVOIDANCE);
}

bool PolarAlignment::handlePostMovementFailure()
{
    Logger::Log("PolarAlignment: 处理移动后失败", LogLevel::INFO, DeviceType::MAIN);
    setState(PolarAlignmentState::ADJUSTING_FOR_OBSTACLE);
    return false;
}

// ==================== 正确的三点极轴校准算法实现 ====================
//
// 算法原理：
// 1. 当极轴完全对准时，望远镜在RA轴上旋转时，DEC轴应该保持不变
// 2. 如果极轴存在偏差，RA轴旋转会导致DEC轴出现偏移
// 3. 通过测量三个不同位置的星点坐标，可以计算出极轴偏差
// 4. 使用三维几何方法，通过平面法向量确定极点位置
// 5. 比较假极点与真实极点的位置来计算偏差
//
// 算法优势：
// - 基于物理原理，计算准确
// - 使用三维几何方法，避免了统计方法的误差
// - 提供方位角和高度角两个方向的偏差
// - 实时反馈，用户友好

// 坐标转换函数实现
PolarAlignment::CartesianCoordinates PolarAlignment::equatorialToCartesian(double ra, double dec, double radius)
{
    // 转换为弧度
    double raRad = ra * M_PI / 180.0;
    double decRad = dec * M_PI / 180.0;
    
    CartesianCoordinates cart;
    cart.x = radius * cos(decRad) * cos(raRad);
    cart.y = radius * cos(decRad) * sin(raRad);
    cart.z = radius * sin(decRad);
    
    return cart;
}

PolarAlignment::SphericalCoordinates PolarAlignment::cartesianToEquatorial(const CartesianCoordinates& cart)
{
    SphericalCoordinates sph;
    double radius = sqrt(cart.x * cart.x + cart.y * cart.y + cart.z * cart.z);
    
    // 检查半径是否为零，避免除零错误
    if (radius < 1e-10) {
        Logger::Log("PolarAlignment: 笛卡尔坐标半径为0，返回默认值", LogLevel::ERROR, DeviceType::MAIN);
        sph.ra = 0.0;
        sph.dec = 0.0;
        return sph;
    }
    
    sph.dec = asin(cart.z / radius) * 180.0 / M_PI;
    sph.ra = atan2(cart.y, cart.x) * 180.0 / M_PI;
    
    // 确保RA在0-360度范围内
    if (sph.ra < 0) sph.ra += 360.0;
    
    return sph;
}

PolarAlignment::CartesianCoordinates PolarAlignment::crossProduct(const CartesianCoordinates& v1, const CartesianCoordinates& v2)
{
    CartesianCoordinates result;
    result.x = v1.y * v2.z - v1.z * v2.y;
    result.y = v1.z * v2.x - v1.x * v2.z;
    result.z = v1.x * v2.y - v1.y * v2.x;
    return result;
}

double PolarAlignment::vectorLength(const CartesianCoordinates& v)
{
    return sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

PolarAlignment::CartesianCoordinates PolarAlignment::normalizeVector(const CartesianCoordinates& v)
{
    double length = vectorLength(v);
    CartesianCoordinates result;
    result.x = v.x / length;
    result.y = v.y / length;
    result.z = v.z / length;
    return result;
}

double PolarAlignment::calculateAngleDifference(double angle1, double angle2)
{
    double difference = angle2 - angle1;
    while (difference > 180) difference -= 360;
    while (difference < -180) difference += 360;
    return difference;
}

// 正确的三点极轴校准算法
bool PolarAlignment::calculatePolarDeviationCorrect(const SloveResults& pos1, const SloveResults& pos2, const SloveResults& pos3,
                                                   GeometricAlignmentResult& result)
{
    Logger::Log("PolarAlignment: 开始正确的三点极轴校准计算", LogLevel::INFO, DeviceType::MAIN);
    
    /*
     * 三点极轴校准算法原理：
     * 
     * 1. 测量三个点：在不同RA位置拍摄并解析，获得三个星点坐标
     *    - 这些点应该在不同的RA位置，但DEC位置应该相近（如果极轴对准的话）
     * 
     * 2. 计算假极轴：通过三点几何关系计算出假极轴位置
     *    - 假极轴是当前赤道仪极轴指向的位置
     *    - 使用三点确定一个平面，该平面的法向量指向假极轴
     * 
     * 3. 计算真极轴：根据当前时间和地理位置计算真实极轴位置
     *    - 真极轴是地球自转轴在天球上的投影
     *    - 考虑了岁差、章动等天文效应
     * 
     * 4. 计算偏差：假极轴与真极轴的差值就是极轴偏差
     *    - 方位角偏差：假极轴在方位角方向上的偏移
     *    - 高度角偏差：假极轴在高度角方向上的偏移
     * 
     * 5. 计算目标位置：当前位置 + 偏差 = 应该移动到的目标位置
     *    - 将赤道仪移动到目标位置，使假极轴与真极轴重合
     *    - 这样就实现了极轴校准
     */
    
    // 检查测量点是否足够分散
    double raDiff1 = std::abs(pos2.RA_Degree - pos1.RA_Degree);
    double raDiff2 = std::abs(pos3.RA_Degree - pos1.RA_Degree);
    double raDiff3 = std::abs(pos3.RA_Degree - pos2.RA_Degree);
    double decDiff1 = std::abs(pos2.DEC_Degree - pos1.DEC_Degree);
    double decDiff2 = std::abs(pos3.DEC_Degree - pos1.DEC_Degree);
    double decDiff3 = std::abs(pos3.DEC_Degree - pos2.DEC_Degree);
    
    // 如果测量点过于接近，无法进行有效计算
    if (raDiff1 < 0.1 && raDiff2 < 0.1 && raDiff3 < 0.1 && 
        decDiff1 < 0.1 && decDiff2 < 0.1 && decDiff3 < 0.1) {
        Logger::Log("PolarAlignment: 测量点过于接近，无法进行有效计算", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 1. 将三个测量点转换为笛卡尔坐标
    CartesianCoordinates p1 = equatorialToCartesian(pos1.RA_Degree, pos1.DEC_Degree);
    CartesianCoordinates p2 = equatorialToCartesian(pos2.RA_Degree, pos2.DEC_Degree);
    CartesianCoordinates p3 = equatorialToCartesian(pos3.RA_Degree, pos3.DEC_Degree);
    
    Logger::Log("PolarAlignment: 测量点1: (" + std::to_string(p1.x) + ", " + std::to_string(p1.y) + ", " + std::to_string(p1.z) + ")", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("PolarAlignment: 测量点2: (" + std::to_string(p2.x) + ", " + std::to_string(p2.y) + ", " + std::to_string(p2.z) + ")", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("PolarAlignment: 测量点3: (" + std::to_string(p3.x) + ", " + std::to_string(p3.y) + ", " + std::to_string(p3.z) + ")", LogLevel::INFO, DeviceType::MAIN);
    
    // 2. 计算两个向量
    CartesianCoordinates v1 = {p2.x - p1.x, p2.y - p1.y, p2.z - p1.z};
    CartesianCoordinates v2 = {p3.x - p1.x, p3.y - p1.y, p3.z - p1.z};
    
    // 3. 计算法向量（叉积）
    CartesianCoordinates normal = crossProduct(v1, v2);
    
    Logger::Log("PolarAlignment: 法向量: (" + std::to_string(normal.x) + ", " + std::to_string(normal.y) + ", " + std::to_string(normal.z) + ")", LogLevel::INFO, DeviceType::MAIN);
    
    // 检查法向量是否为零向量
    double normalLength = vectorLength(normal);
    if (normalLength < 1e-10) {
        Logger::Log("PolarAlignment: 法向量为零向量，测量点共线或重复", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 4. 归一化法向量
    CartesianCoordinates unitNormal = normalizeVector(normal);
    
    // 5. 计算与单位球面的交点（假极点）
    double r = 1.0;
    CartesianCoordinates fakePolarPoint = {unitNormal.x * r, unitNormal.y * r, unitNormal.z * r};
    
    // 选择正确的交点（z坐标为正的）
    if (fakePolarPoint.z < 0) {
        fakePolarPoint.x = -fakePolarPoint.x;
        fakePolarPoint.y = -fakePolarPoint.y;
        fakePolarPoint.z = -fakePolarPoint.z;
    }
    
    Logger::Log("PolarAlignment: 假极点: (" + std::to_string(fakePolarPoint.x) + ", " + std::to_string(fakePolarPoint.y) + ", " + std::to_string(fakePolarPoint.z) + ")", LogLevel::INFO, DeviceType::MAIN);
    
    // 检查假极点是否为零向量
    double fakePolarLength = sqrt(fakePolarPoint.x * fakePolarPoint.x + fakePolarPoint.y * fakePolarPoint.y + fakePolarPoint.z * fakePolarPoint.z);
    if (fakePolarLength < 1e-10) {
        Logger::Log("PolarAlignment: 假极点为零向量，计算失败", LogLevel::ERROR, DeviceType::MAIN);
        result.isValid = false;
        result.errorMessage = "假极点为零向量";
        return false;
    }
    
    // 6. 将假极点转换为赤道坐标
    SphericalCoordinates fakePolarEquatorial = cartesianToEquatorial(fakePolarPoint);
    
    Logger::Log("PolarAlignment: 假极点赤道坐标: RA=" + std::to_string(fakePolarEquatorial.ra) + "°, DEC=" + std::to_string(fakePolarEquatorial.dec) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 检查DEC值是否在有效范围内
    if (fakePolarEquatorial.dec < -90.0 || fakePolarEquatorial.dec > 90.0) {
        Logger::Log("PolarAlignment: 假极点DEC值超出有效范围 [-90°, 90°]: " + std::to_string(fakePolarEquatorial.dec) + "°", LogLevel::ERROR, DeviceType::MAIN);
        result.isValid = false;
        result.errorMessage = "假极点DEC值超出有效范围";
        return false;
    }
    
    // 7. 真实极点坐标（根据当前时间和地理位置计算）
    double realPolarRA, realPolarDEC;
    
    // 优先使用缓存的真极轴位置，避免频繁变化
    if (isRealPolarCached) {
        realPolarRA = this->realPolarRA;
        realPolarDEC = this->realPolarDEC;
        Logger::Log("PolarAlignment: 使用缓存的真极轴位置进行偏差计算 - RA: " + std::to_string(realPolarRA) + 
                    "°, DEC: " + std::to_string(realPolarDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    } else {
        // 如果缓存不存在，重新计算
        if (!calculatePrecisePolarPosition(realPolarRA, realPolarDEC)) {
            Logger::Log("PolarAlignment: 计算真实极点坐标失败", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
        Logger::Log("PolarAlignment: 重新计算真实极点坐标 - RA: " + std::to_string(realPolarRA) + 
                    "°, DEC: " + std::to_string(realPolarDEC) + "°", LogLevel::WARNING, DeviceType::MAIN);
    }
    
    // 8. 计算偏差：假极轴位置 - 真极轴位置
    result.azimuthDeviation = calculateAngleDifference(realPolarRA, fakePolarEquatorial.ra);
    result.altitudeDeviation = fakePolarEquatorial.dec - realPolarDEC;
    
    // 9. 保存假极轴位置
    result.fakePolarRA = fakePolarEquatorial.ra;
    result.fakePolarDEC = fakePolarEquatorial.dec;
    
    // 检查计算结果的有效性，避免NaN
    if (std::isnan(result.azimuthDeviation) || std::isnan(result.altitudeDeviation)) {
        Logger::Log("PolarAlignment: 偏差计算结果为NaN，计算失败", LogLevel::ERROR, DeviceType::MAIN);
        result.isValid = false;
        result.errorMessage = "偏差计算结果为NaN";
        return false;
    }
    
    // 检查计算结果的范围合理性
    if (std::abs(result.azimuthDeviation) > 180.0 || std::abs(result.altitudeDeviation) > 90.0) {
        Logger::Log("PolarAlignment: 偏差计算结果超出合理范围，计算失败", LogLevel::ERROR, DeviceType::MAIN);
        result.isValid = false;
        result.errorMessage = "偏差计算结果超出合理范围";
        return false;
    }
    
    // 设置结果有效性
    result.isValid = true;
    result.confidence = 1.0; // 简化设置置信度
    
    Logger::Log("PolarAlignment: 方位角偏差: " + std::to_string(result.azimuthDeviation) + "°, 高度角偏差: " + std::to_string(result.altitudeDeviation) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    return true;
}

// ==================== 严格几何法函数实现 ====================

bool PolarAlignment::calculatePolarDeviationGeometric(const SloveResults& pos1,
    const SloveResults& pos2,
    const SloveResults& pos3,
    GeometricAlignmentResult& result)
{
Logger::Log("PolarAlignment: 开始严格几何法三点极轴校准计算（球面法）", LogLevel::INFO, DeviceType::MAIN);

// 初始化
result.azimuthDeviation = 0.0;
result.altitudeDeviation = 0.0;
result.confidence = 0.0;
result.isValid = false;
result.errorMessage = "";

// 1) 真极点 p
double realPolarRA_deg = 0.0, realPolarDEC_deg = 90.0;
if (!calculatePrecisePolarPosition(realPolarRA_deg, realPolarDEC_deg)) {
result.errorMessage = "计算真实极点坐标失败";
Logger::Log("PolarAlignment: " + result.errorMessage.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
return false;
}
Vec3 p = radecDeg_to_vec(realPolarRA_deg, realPolarDEC_deg);
Logger::Log("PolarAlignment: 真实极点坐标 - RA: " + std::to_string(realPolarRA_deg) +
"°, DEC: " + std::to_string(realPolarDEC_deg) + "°", LogLevel::INFO, DeviceType::MAIN);

// 2) 三个测量点的单位向量
Vec3 q1 = radecDeg_to_vec(pos1.RA_Degree, pos1.DEC_Degree);
Vec3 q2 = radecDeg_to_vec(pos2.RA_Degree, pos2.DEC_Degree);
Vec3 q3 = radecDeg_to_vec(pos3.RA_Degree, pos3.DEC_Degree);

// 基本分散度检查（球面角距）
auto ang12 = ang(q1, q2);
auto ang13 = ang(q1, q3);
auto ang23 = ang(q2, q3);
double min_sep_deg = std::min({ang12, ang13, ang23}) * kRad2Deg;

if (min_sep_deg < 0.01) { // < 36arcsec：几乎重合
result.errorMessage = "测量点过近，无法可靠求解";
Logger::Log("PolarAlignment: " + result.errorMessage.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
return false;
}

// 3) 拟合球面小圆的球心（假极点）与半径
Vec3 c;        // 假极点方向
double rho = 0.0, rms = 0.0;
if (!fit_spherical_circle_3pt(q1, q2, q3, c, rho, rms)) {
result.errorMessage = "三点几何退化，无法确定假极点（小圆球心）";
Logger::Log("PolarAlignment: " + result.errorMessage.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
return false;
}

// 选择靠近真极点的解（c 与 -c 等价，选 dot(p,c) 最大的那个）
if (dot(p, c) < 0.0) c = c * (-1.0);

// 4) 计算切平面偏差向量 d = log_p(c)（二维）
TangentBasis B = make_tangent_basis(p);
auto [du, dv] = log_map_2d(p, B, c);

// 5) 把假极点转回 RA/DEC 以便日志/输出
double fakeRA_deg = 0.0, fakeDEC_deg = 0.0;
vec_to_radecDeg(c, fakeRA_deg, fakeDEC_deg);

// 6) 置信度（基于小圆一致性 + 点的分散度）
//    - 小圆一致性：θ_i = ang(c, qi) 应基本相等 => 用 RMS(θ_i) 归一化衡量
double t1 = ang(c, q1), t2 = ang(c, q2), t3 = ang(c, q3);
double mean_theta = (t1 + t2 + t3) / 3.0;
double r_rms = std::sqrt(((t1-mean_theta)*(t1-mean_theta) + (t2-mean_theta)*(t2-mean_theta) + (t3-mean_theta)*(t3-mean_theta))/3.0);

//    - 分散度：最小夹角越大越好；>5° 视为满分
double spread_gain = std::min(1.0, std::max(0.0, (min_sep_deg / 5.0))); // 0~1

//    - 综合置信度（0~1）：相对残差小 => 置信度高
double rel = r_rms / std::max(mean_theta, 1e-6);
double conf = 1.0 / (1.0 + rel*rel);   // 简洁稳定
conf *= spread_gain;

// 7) 输出
result.azimuthDeviation  = du * kRad2Deg;  // 注意：这是相对于真极切平面基 {e1,e2} 的分量
result.altitudeDeviation = dv * kRad2Deg;
result.fakePolarRA  = fakeRA_deg;
result.fakePolarDEC = fakeDEC_deg;
result.confidence   = conf;
result.isValid      = std::isfinite(result.azimuthDeviation) &&
std::isfinite(result.altitudeDeviation);

if (!result.isValid) {
result.errorMessage = "计算结果出现非数（NaN/Inf）";
Logger::Log("PolarAlignment: " + result.errorMessage.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
return false;
}

Logger::Log("PolarAlignment: 假极点(球心) - RA: " + std::to_string(fakeRA_deg) + 
"°, DEC: " + std::to_string(fakeDEC_deg) + "°", LogLevel::INFO, DeviceType::MAIN);
Logger::Log("PolarAlignment: 偏差向量(切平面) - du: " + std::to_string(result.azimuthDeviation) +
"°, dv: " + std::to_string(result.altitudeDeviation) + "°", LogLevel::INFO, DeviceType::MAIN);
Logger::Log("PolarAlignment: 小圆半径(平均): " + std::to_string(mean_theta * kRad2Deg) +
"°, 半径RMS: " + std::to_string(r_rms * kRad2Deg) +
"°, 最小点间夹角: " + std::to_string(min_sep_deg) +
"°, 置信度: " + std::to_string(conf), LogLevel::INFO, DeviceType::MAIN);

return true;
}


PolarAlignment::TangentPlaneCoordinates PolarAlignment::projectToTangentPlane(double ra, double dec, double poleRa, double poleDec)
{
    // 转换为弧度
    double raRad = ra * M_PI / 180.0;
    double decRad = dec * M_PI / 180.0;
    double poleRaRad = poleRa * M_PI / 180.0;
    double poleDecRad = poleDec * M_PI / 180.0;
    
    // 计算球面坐标的笛卡尔表示
    double x = cos(decRad) * cos(raRad);
    double y = cos(decRad) * sin(raRad);
    double z = sin(decRad);
    
    // 极点方向的单位向量
    double poleX = cos(poleDecRad) * cos(poleRaRad);
    double poleY = cos(poleDecRad) * sin(poleRaRad);
    double poleZ = sin(poleDecRad);
    
    // 计算从极点到测量点的向量
    double dx = x - poleX;
    double dy = y - poleY;
    double dz = z - poleZ;
    
    // 计算到极点的距离（球面距离）
    double distance = sqrt(dx*dx + dy*dy + dz*dz);
    
    // 如果距离太小，返回原点
    if (distance < 1e-10) {
        return {0.0, 0.0};
    }
    
    // 归一化方向向量
    dx /= distance;
    dy /= distance;
    dz /= distance;
    
    // 计算切平面坐标（使用球面投影）
    // 这里使用等角投影（stereographic projection）
    double scale = 2.0 / (1.0 + poleX*x + poleY*y + poleZ*z);
    
    TangentPlaneCoordinates result;
    result.u = scale * (x - poleX);
    result.v = scale * (y - poleY);
    
    return result;
}

PolarAlignment::SphericalCoordinates PolarAlignment::projectFromTangentPlane(const TangentPlaneCoordinates& tangentCoords, 
                                                           double poleRa, double poleDec)
{
    // 转换为弧度
    double poleRaRad = poleRa * M_PI / 180.0;
    double poleDecRad = poleDec * M_PI / 180.0;
    
    // 极点方向的单位向量
    double poleX = cos(poleDecRad) * cos(poleRaRad);
    double poleY = cos(poleDecRad) * sin(poleRaRad);
    double poleZ = sin(poleDecRad);
    
    // 从切平面坐标恢复球面坐标（逆等角投影）
    double u = tangentCoords.u;
    double v = tangentCoords.v;
    double r2 = u*u + v*v;
    
    double x = (4.0 * u) / (4.0 + r2) + poleX;
    double y = (4.0 * v) / (4.0 + r2) + poleY;
    double z = (r2 - 4.0) / (4.0 + r2) + poleZ;
    
    // 归一化
    double norm = sqrt(x*x + y*y + z*z);
    x /= norm;
    y /= norm;
    z /= norm;
    
    // 转换为球面坐标
    SphericalCoordinates result;
    result.dec = asin(z) * 180.0 / M_PI;
    result.ra = atan2(y, x) * 180.0 / M_PI;
    
    // 确保RA在0-360度范围内
    if (result.ra < 0) result.ra += 360.0;
    
    return result;
}

bool PolarAlignment::calculateDeviationInTangentPlane(const QVector<TangentPlaneCoordinates>& tangentPoints,
                                                     GeometricAlignmentResult& result)
{
    if (tangentPoints.size() != 3) {
        result.errorMessage = "需要恰好三个测量点";
        Logger::Log("PolarAlignment: " + result.errorMessage.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    const auto& p1 = tangentPoints[0];
    const auto& p2 = tangentPoints[1];
    const auto& p3 = tangentPoints[2];
    
    // 计算三角形的几何中心
    TangentPlaneCoordinates center = calculateGeometricCenter(p1, p2, p3);
    
    // 计算三角形面积，用于评估测量质量
    double area = calculateTriangleArea(p1, p2, p3);
    if (area < 1e-10) {
        result.errorMessage = "测量点共线，无法计算偏差";
        Logger::Log("PolarAlignment: " + result.errorMessage.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 在切平面中，理想的极轴位置应该是原点(0,0)
    // 几何中心到原点的向量就是极轴偏差
    TangentPlaneCoordinates deviation;
    deviation.u = -center.u;  // 负号是因为我们要从当前位置移动到原点
    deviation.v = -center.v;
    
    // 将切平面偏差转换为球面偏差
    // 这里使用简化的线性近似，对于小角度偏差是合理的
    result.azimuthDeviation = deviation.u * 180.0 / M_PI;  // 转换为度
    result.altitudeDeviation = deviation.v * 180.0 / M_PI;
    
    Logger::Log("PolarAlignment: 切平面几何中心: (" + std::to_string(center.u) + ", " + std::to_string(center.v) + ")", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("PolarAlignment: 切平面偏差: (" + std::to_string(deviation.u) + ", " + std::to_string(deviation.v) + ")", LogLevel::INFO, DeviceType::MAIN);
    
    return true;
}

PolarAlignment::TangentPlaneCoordinates PolarAlignment::calculateGeometricCenter(const TangentPlaneCoordinates& p1,
                                                                const TangentPlaneCoordinates& p2,
                                                                const TangentPlaneCoordinates& p3)
{
    TangentPlaneCoordinates center;
    center.u = (p1.u + p2.u + p3.u) / 3.0;
    center.v = (p1.v + p2.v + p3.v) / 3.0;
    return center;
}

double PolarAlignment::calculateTangentPlaneDistance(const TangentPlaneCoordinates& p1,
                                                    const TangentPlaneCoordinates& p2)
{
    double du = p2.u - p1.u;
    double dv = p2.v - p1.v;
    return sqrt(du*du + dv*dv);
}

double PolarAlignment::calculateTriangleArea(const TangentPlaneCoordinates& p1,
                                            const TangentPlaneCoordinates& p2,
                                            const TangentPlaneCoordinates& p3)
{
    // 使用叉积公式计算三角形面积
    double area = 0.5 * std::abs((p2.u - p1.u) * (p3.v - p1.v) - (p3.u - p1.u) * (p2.v - p1.v));
    return area;
}

double PolarAlignment::calculateGeometricConfidence(const QVector<TangentPlaneCoordinates>& tangentPoints,
                                                  const TangentPlaneCoordinates& deviation)
{
    if (tangentPoints.size() != 3) {
        return 0.0;
    }
    
    const auto& p1 = tangentPoints[0];
    const auto& p2 = tangentPoints[1];
    const auto& p3 = tangentPoints[2];
    
    // 计算三角形的面积和周长
    double area = calculateTriangleArea(p1, p2, p3);
    double perimeter = calculateTangentPlaneDistance(p1, p2) + 
                      calculateTangentPlaneDistance(p2, p3) + 
                      calculateTangentPlaneDistance(p3, p1);
    
    // 计算偏差的大小
    double deviationMagnitude = sqrt(deviation.u*deviation.u + deviation.v*deviation.v);
    
    // 基于三角形质量和偏差大小计算置信度
    double areaConfidence = std::min(1.0, area * 1000.0);  // 面积越大，置信度越高
    double deviationConfidence = std::max(0.0, 1.0 - deviationMagnitude * 10.0);  // 偏差越小，置信度越高
    
    double confidence = (areaConfidence + deviationConfidence) / 2.0;
    return std::max(0.0, std::min(1.0, confidence));
}




// ==================== 坐标转换函数实现 ====================

bool PolarAlignment::convertEquatorialToHorizontal(double ra, double dec, double latitude, double longitude,
                                                  double& azimuth, double& altitude)
{
    // 转换为弧度
    double raRad = ra * M_PI / 180.0;
    double decRad = dec * M_PI / 180.0;
    double latRad = latitude * M_PI / 180.0;
    double lonRad = longitude * M_PI / 180.0;
    
    // 计算当前时间（UTC）
    QDateTime utc = QDateTime::currentDateTimeUtc();
    QDate date = utc.date();
    QTime time = utc.time();
    
    // 计算儒略日
    double jd = calculateJulianDay(date.year(), date.month(), date.day(), 
                                  time.hour(), time.minute(), time.second());
    
    // 计算格林尼治恒星时
    double gst = calculateGreenwichSiderealTime(jd);
    
    // 计算地方恒星时
    double lst = gst + longitude;
    lst = normalizeAngle360(lst);
    
    // 转换为弧度
    double lstRad = lst * M_PI / 180.0;
    
    // 计算时角（地方恒星时 - 赤经）
    double haRad = lstRad - raRad;
    
    // 归一化时角到0-2π范围
    while (haRad < 0) haRad += 2.0 * M_PI;
    while (haRad >= 2.0 * M_PI) haRad -= 2.0 * M_PI;
    
    // 计算地平坐标
    double sinAlt = sin(decRad) * sin(latRad) + cos(decRad) * cos(latRad) * cos(haRad);
    
    // 处理数值精度问题
    if (sinAlt > 1.0) sinAlt = 1.0;
    if (sinAlt < -1.0) sinAlt = -1.0;
    
    altitude = asin(sinAlt) * 180.0 / M_PI;
    
    // 计算方位角（使用更稳定的公式）
    double cosAz = (sin(decRad) - sin(altitude * M_PI / 180.0) * sin(latRad)) / 
                   (cos(altitude * M_PI / 180.0) * cos(latRad));
    
    // 处理数值精度问题
    if (cosAz > 1.0) cosAz = 1.0;
    if (cosAz < -1.0) cosAz = -1.0;
    
    // 使用atan2函数计算方位角，避免象限问题
    double sinAz = (cos(decRad) * sin(haRad)) / cos(altitude * M_PI / 180.0);
    double azRad = atan2(sinAz, cosAz);
    
    // 转换为度并确保在0-360度范围内
    azimuth = azRad * 180.0 / M_PI;
    azimuth = normalizeAngle360(azimuth);
    
    return true;
}

bool PolarAlignment::convertHorizontalToEquatorial(double azimuth, double altitude, double latitude, double longitude,
                                                  double& ra, double& dec)
{
    // 转换为弧度
    double azRad = azimuth * M_PI / 180.0;
    double altRad = altitude * M_PI / 180.0;
    double latRad = latitude * M_PI / 180.0;
    double lonRad = longitude * M_PI / 180.0;
    
    // 计算赤纬
    double sinDec = sin(altRad) * sin(latRad) + cos(altRad) * cos(latRad) * cos(azRad);
    
    // 处理数值精度问题
    if (sinDec > 1.0) sinDec = 1.0;
    if (sinDec < -1.0) sinDec = -1.0;
    
    dec = asin(sinDec) * 180.0 / M_PI;
    
    // 计算时角（使用更稳定的公式）
    double cosHa = (sin(altRad) - sin(dec * M_PI / 180.0) * sin(latRad)) / 
                   (cos(dec * M_PI / 180.0) * cos(latRad));
    
    // 处理数值精度问题
    if (cosHa > 1.0) cosHa = 1.0;
    if (cosHa < -1.0) cosHa = -1.0;
    
    // 使用atan2函数计算时角，避免象限问题
    double sinHa = (cos(altRad) * sin(azRad)) / cos(dec * M_PI / 180.0);
    double haRad = atan2(sinHa, cosHa);
    
    // 确保时角在0-2π范围内
    while (haRad < 0) haRad += 2.0 * M_PI;
    while (haRad >= 2.0 * M_PI) haRad -= 2.0 * M_PI;
    
    // 计算当前时间（UTC）
    QDateTime utc = QDateTime::currentDateTimeUtc();
    QDate date = utc.date();
    QTime time = utc.time();
    
    // 计算儒略日
    double jd = calculateJulianDay(date.year(), date.month(), date.day(), 
                                  time.hour(), time.minute(), time.second());
    
    // 计算格林尼治恒星时
    double gst = calculateGreenwichSiderealTime(jd);
    
    // 计算地方恒星时
    double lst = gst + longitude;
    lst = normalizeAngle360(lst);
    
    // 计算赤经
    ra = lst - haRad * 180.0 / M_PI;
    ra = normalizeAngle360(ra);
    
    return true;
}

bool PolarAlignment::getCurrentHorizontalCoordinates(double& azimuth, double& altitude)
{
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 首先尝试直接获取地平坐标
    double az, alt;
    uint32_t result = indiServer->getTelescopetAZALT(dpMount, az, alt);
    
    if (result == QHYCCD_SUCCESS) {
        // 直接获取成功
        azimuth = az;
        altitude = alt;
        Logger::Log("PolarAlignment: 直接获取地平坐标 - 方位角: " + std::to_string(azimuth) + 
                    "°, 高度角: " + std::to_string(altitude) + "°", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
    
    // 直接获取失败，尝试通过赤经赤纬计算
    Logger::Log("PolarAlignment: 直接获取地平坐标失败，尝试通过赤经赤纬计算", LogLevel::INFO, DeviceType::MAIN);
    
    double ra_hours, dec_degrees;
    result = indiServer->getTelescopeRADECJNOW(dpMount, ra_hours, dec_degrees);
    
    if (result != QHYCCD_SUCCESS) {
        Logger::Log("PolarAlignment: 获取赤经赤纬坐标失败", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取观测者位置信息
    double observer_lat, observer_lon, observer_elev;
    if (!getObserverLocation(observer_lat, observer_lon, observer_elev)) {
        Logger::Log("PolarAlignment: 获取观测者位置失败", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 将赤经赤纬转换为地平坐标
    if (!convertRADECToHorizontal(ra_hours, dec_degrees, observer_lat, observer_lon, azimuth, altitude)) {
        Logger::Log("PolarAlignment: 赤经赤纬转地平坐标计算失败", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    Logger::Log("PolarAlignment: 通过计算获得地平坐标 - 方位角: " + std::to_string(azimuth) + 
                "°, 高度角: " + std::to_string(altitude) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    return true;
}

// ==================== 坐标转换辅助函数 ====================

bool PolarAlignment::getObserverLocation(double& latitude, double& longitude, double& elevation)
{
    // 从配置中获取观测者位置
    latitude = config.latitude;
    longitude = config.longitude;
    elevation = 0.0;  // 通常没有海拔数据，设为0
    
    // 检查位置数据是否有效
    if (latitude == 0.0 && longitude == 0.0) {
        Logger::Log("PolarAlignment: 观测者位置未设置，请先设置观测地点", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    Logger::Log("PolarAlignment: 使用配置的观测者位置 - 纬度: " + std::to_string(latitude) + 
                "°, 经度: " + std::to_string(longitude) + "°", 
                LogLevel::INFO, DeviceType::MAIN);
    
    return true;
}

bool PolarAlignment::convertRADECToHorizontal(double ra_hours, double dec_degrees, 
                                            double observer_lat, double observer_lon, 
                                            double& azimuth, double& altitude)
{
    // 将输入转换为弧度
    double ra_rad = ra_hours * M_PI / 12.0;  // 赤经：小时转弧度
    double dec_rad = dec_degrees * M_PI / 180.0;  // 赤纬：度转弧度
    double lat_rad = observer_lat * M_PI / 180.0;  // 纬度：度转弧度
    double lon_rad = observer_lon * M_PI / 180.0;  // 经度：度转弧度
    
    // 计算当前时间（简化处理，使用系统时间）
    time_t now = time(0);
    struct tm* timeinfo = localtime(&now);
    double hour = timeinfo->tm_hour + timeinfo->tm_min / 60.0 + timeinfo->tm_sec / 3600.0;
    
    // 计算本地恒星时（简化计算）
    double lst_hours = hour + lon_rad * 12.0 / M_PI;  // 简化计算
    double lst_rad = lst_hours * M_PI / 12.0;
    
    // 计算时角
    double ha_rad = lst_rad - ra_rad;
    
    // 计算地平坐标
    double sin_alt = sin(dec_rad) * sin(lat_rad) + cos(dec_rad) * cos(lat_rad) * cos(ha_rad);
    double cos_alt = sqrt(1.0 - sin_alt * sin_alt);
    
    if (cos_alt == 0) {
        // 在天顶或天底
        altitude = (sin_alt > 0) ? 90.0 : -90.0;
        azimuth = 0.0;  // 未定义
    } else {
        altitude = asin(sin_alt) * 180.0 / M_PI;
        
        double cos_az = (sin(dec_rad) - sin(lat_rad) * sin_alt) / (cos(lat_rad) * cos_alt);
        double sin_az = -cos(dec_rad) * sin(ha_rad) / cos_alt;
        
        azimuth = atan2(sin_az, cos_az) * 180.0 / M_PI;
        if (azimuth < 0) azimuth += 360.0;
    }
    
    Logger::Log("PolarAlignment: 坐标转换 - RA: " + std::to_string(ra_hours) + 
                "h, DEC: " + std::to_string(dec_degrees) + "° -> AZ: " + 
                std::to_string(azimuth) + "°, ALT: " + std::to_string(altitude) + "°", 
                LogLevel::DEBUG, DeviceType::MAIN);
    
    return true;
}

bool PolarAlignment::calculatePolarAlignmentAdjustment(double raDeviation, double decDeviation,
                                                     double observerLat, double observerLon,
                                                     double& azimuthAdjustment, double& altitudeAdjustment)
{
    Logger::Log("PolarAlignment: 计算极轴校准机械调整量(机械轴雅可比)", LogLevel::INFO, DeviceType::MAIN);

    // 1) 构建本地 ENU 基底
    const double deg2rad = M_PI / 180.0;
    const double rad2deg = 180.0 / M_PI;
    const double latRad = observerLat * deg2rad;

    // 基向量（ENU）
    CartesianCoordinates Up   = {0.0, 0.0, 1.0};
    CartesianCoordinates East = {1.0, 0.0, 0.0};

    // 2) 在本地地平系下构造当前"极轴方向" p
    // 优先使用已测得的假极轴(cachedFakePolarRA/DEC)，否则使用理想化的天极位置：
    //   北半球: az=0°,  alt=|lat|
    //   南半球: az=180°, alt=|lat|
    CartesianCoordinates p;
    bool usedMeasuredPole = false;
    if (isFakePolarCached) {
        double poleAz=0.0, poleAlt=0.0;
        // convertRADECToHorizontal 需要 RA(小时)，DEC(度)
        double fakeRA_hours = cachedFakePolarRA / 15.0;
        if (convertRADECToHorizontal(fakeRA_hours, cachedFakePolarDEC, observerLat, observerLon, poleAz, poleAlt)) {
            double azRad = poleAz * deg2rad;
            double altRad = poleAlt * deg2rad;
            p.x = sin(azRad) * cos(altRad);   // East
            p.y = cos(azRad) * cos(altRad);   // North
            p.z = sin(altRad);                // Up
            usedMeasuredPole = true;
        }
    }
    if (!usedMeasuredPole) {
        double altAbs = std::fabs(latRad);
        double az = (observerLat >= 0.0) ? 0.0 : M_PI; // 北半球朝北; 南半球朝南
        p.x = sin(az) * cos(altAbs);
        p.y = cos(az) * cos(altAbs) * ((observerLat >= 0.0) ? 1.0 : -1.0); // 等价于 N = sign(lat)*cos(|lat|)
        p.z = sin(altAbs); // 始终抬升至 |lat|
    }

    auto dot3 = [](const CartesianCoordinates& a, const CartesianCoordinates& b){ return a.x*b.x + a.y*b.y + a.z*b.z; };

    // 3) 在 p 处建立切平面正交基：e_alt_like（沿仰角方向）、e_az_like（沿绕极方向）
    double dotUpP = dot3(Up, p);
    CartesianCoordinates t;
    // 若 Up 与 p 几乎平行，使用 East 作为备用基以避免奇异
    if (std::fabs(std::fabs(dotUpP) - 1.0) < 1e-6) {
        double dotEastP = dot3(East, p);
        t = { East.x - dotEastP * p.x, East.y - dotEastP * p.y, East.z - dotEastP * p.z };
    } else {
        t = { Up.x - dotUpP * p.x, Up.y - dotUpP * p.y, Up.z - dotUpP * p.z };
    }
    CartesianCoordinates e_alt_like = normalizeVector(t);
    CartesianCoordinates e_az_like  = crossProduct(p, e_alt_like); // 已与 p 和 e_alt_like 正交

    // 4) 机械轴的单位小转动在切平面的投影
    //    - 方位螺栓：绕 Up 旋转 -> delta_p_az = Up × p
    //    - 高度螺栓：绕 East 旋转 -> delta_p_alt = East × p
    CartesianCoordinates delta_p_az  = crossProduct(Up, p);
    CartesianCoordinates delta_p_alt = crossProduct(East, p);

    // 5) 组装 2x2 雅可比：J * [d_az_mech, d_alt_mech]^T = [u, v]^T
    //    其中 [u,v] 是切平面期望偏差（单位：弧度）；raDeviation->沿 e_az_like，decDeviation->沿 e_alt_like
    double u = raDeviation * deg2rad;
    double v = decDeviation * deg2rad;

    double J11 = dot3(delta_p_az,  e_az_like);
    double J12 = dot3(delta_p_az,  e_alt_like);
    double J21 = dot3(delta_p_alt, e_az_like);
    double J22 = dot3(delta_p_alt, e_alt_like);

    double det = J11*J22 - J12*J21;
    if (std::fabs(det) < 1e-9 || std::isnan(det)) {
        Logger::Log("PolarAlignment: 雅可比矩阵奇异，退化为直接映射", LogLevel::WARNING, DeviceType::MAIN);
        azimuthAdjustment = raDeviation;
        altitudeAdjustment = decDeviation;
        return true;
    }

    // 6) 反解机械角（弧度）
    double invJ11 =  J22 / det;
    double invJ12 = -J12 / det;
    double invJ21 = -J21 / det;
    double invJ22 =  J11 / det;

    double d_az_mech  = invJ11*u + invJ12*v; // 弧度
    double d_alt_mech = invJ21*u + invJ22*v; // 弧度

    // 7) 输出（度）
    azimuthAdjustment  = d_az_mech  * rad2deg;
    altitudeAdjustment = d_alt_mech * rad2deg;

    if (std::isnan(azimuthAdjustment) || std::isnan(altitudeAdjustment)) {
        Logger::Log("PolarAlignment: 机械角计算为NaN，返回失败", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    Logger::Log("PolarAlignment: 极轴校准调整量(雅可比) - 方位角调整: " + std::to_string(azimuthAdjustment) +
                "°, 高度角调整: " + std::to_string(altitudeAdjustment) + "°", LogLevel::INFO, DeviceType::MAIN);
    return true;
}

// ==================== 调整指导数据管理函数实现 ====================

void PolarAlignment::sendValidAdjustmentGuideData()
{
    Logger::Log("PolarAlignment: 发送校准点和最后解析数据", LogLevel::INFO, DeviceType::MAIN);
    
    if (adjustmentGuideDataHistory.isEmpty()) {
        Logger::Log("PolarAlignment: 调整指导数据容器为空", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    
    // 发送三个校准点时的数据（offsetRa和offsetDec都为0的数据）
    int calibrationPointCount = 0;
    for (const AdjustmentGuideData& data : adjustmentGuideDataHistory) {
        if (data.offsetRa == 0.0 && data.offsetDec == 0.0 && data.targetRa == -1.0) {
            calibrationPointCount++;
            Logger::Log("PolarAlignment: 发送校准点" + std::to_string(calibrationPointCount) + 
                       " - RA: " + std::to_string(data.ra) + "°, DEC: " + std::to_string(data.dec) + "°", 
                       LogLevel::INFO, DeviceType::MAIN);
            
            emit adjustmentGuideData(data.ra, data.dec, data.ra0, data.dec0, data.ra1, data.dec1, data.ra2, data.dec2, data.ra3, data.dec3, data.targetRa, data.targetDec,
                                   data.offsetRa, data.offsetDec, data.adjustmentRa, data.adjustmentDec,
                                   data.fakePolarRA, data.fakePolarDEC, data.realPolarRA, data.realPolarDEC);
            
            // 只发送前三个校准点
            if (calibrationPointCount >= 3) {
                break;
            }
        }
    }
    
    // 发送最后一次解析的数据（最后一个星点的数据）
    const AdjustmentGuideData& lastData = adjustmentGuideDataHistory.last();
    Logger::Log("PolarAlignment: 发送最后解析数据 - RA: " + std::to_string(lastData.ra) + 
               "°, DEC: " + std::to_string(lastData.dec) + "°, 偏移RA: " + std::to_string(lastData.offsetRa) + 
               "°, 偏移DEC: " + std::to_string(lastData.offsetDec) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    emit adjustmentGuideData(lastData.ra, lastData.dec, lastData.ra0, lastData.dec0, lastData.ra1, lastData.dec1, lastData.ra2, lastData.dec2, lastData.ra3, lastData.dec3, lastData.targetRa, lastData.targetDec,
                           lastData.offsetRa, lastData.offsetDec, lastData.adjustmentRa, lastData.adjustmentDec,
                           lastData.fakePolarRA, lastData.fakePolarDEC, lastData.realPolarRA, lastData.realPolarDEC);
    
    Logger::Log("PolarAlignment: 已发送" + std::to_string(calibrationPointCount) + "个校准点和1个最后解析数据", 
               LogLevel::INFO, DeviceType::MAIN);
}

void PolarAlignment::clearAdjustmentGuideData()
{
    Logger::Log("PolarAlignment: 清空调整指导数据容器", LogLevel::INFO, DeviceType::MAIN);
    adjustmentGuideDataHistory.clear();
}

void PolarAlignment::saveAndEmitAdjustmentGuideData(double ra, double dec,
                                                   double ra0, double dec0, double ra1, double dec1,
                                                   double ra2, double dec2, double ra3, double dec3,
                                                   double targetRa, double targetDec,
                                                   double offsetRa, double offsetDec, QString adjustmentRa, QString adjustmentDec,
                                                   double fakePolarRA, double fakePolarDEC, double realPolarRA, double realPolarDEC)
{
    // 创建数据结构
    AdjustmentGuideData data;
    data.ra = ra;
    data.dec = dec;
    data.ra0 = ra0; data.dec0 = dec0;
    data.ra1 = ra1; data.dec1 = dec1;
    data.ra2 = ra2; data.dec2 = dec2;
    data.ra3 = ra3; data.dec3 = dec3;
    data.targetRa = targetRa;
    data.targetDec = targetDec;
    data.offsetRa = offsetRa;
    data.offsetDec = offsetDec;
    data.adjustmentRa = adjustmentRa;
    data.adjustmentDec = adjustmentDec;
    data.fakePolarRA = fakePolarRA;
    data.fakePolarDEC = fakePolarDEC;
    data.realPolarRA = realPolarRA;
    data.realPolarDEC = realPolarDEC;
    data.timestamp = QDateTime::currentDateTime();
    
    // 判断是校准点还是解析数据
    bool isCalibrationPoint = (offsetRa == 0.0 && offsetDec == 0.0 && targetRa == -1.0);
    
    if (isCalibrationPoint) {
        // 校准点数据：最多保存3个
        int calibrationPointCount = 0;
        for (const AdjustmentGuideData& existingData : adjustmentGuideDataHistory) {
            if (existingData.offsetRa == 0.0 && existingData.offsetDec == 0.0 && existingData.targetRa == -1.0) {
                calibrationPointCount++;
            }
        }
        
        if (calibrationPointCount < 3) {
            adjustmentGuideDataHistory.append(data);
            Logger::Log("PolarAlignment: 保存校准点" + std::to_string(calibrationPointCount + 1) + 
                       " - RA: " + std::to_string(ra) + "°, DEC: " + std::to_string(dec) + "°", 
                       LogLevel::INFO, DeviceType::MAIN);
        } else {
            Logger::Log("PolarAlignment: 已有3个校准点，跳过保存新的校准点", LogLevel::WARNING, DeviceType::MAIN);
        }
    } else {
        // 解析数据：替换最后一个解析数据（第4个位置）
        // 先移除之前的解析数据
        for (int i = adjustmentGuideDataHistory.size() - 1; i >= 0; i--) {
            const AdjustmentGuideData& existingData = adjustmentGuideDataHistory[i];
            if (existingData.offsetRa != 0.0 || existingData.offsetDec != 0.0) {
                adjustmentGuideDataHistory.removeAt(i);
                Logger::Log("PolarAlignment: 移除旧的解析数据", LogLevel::INFO, DeviceType::MAIN);
                break;
            }
        }
        
        // 添加新的解析数据
        adjustmentGuideDataHistory.append(data);
        Logger::Log("PolarAlignment: 保存解析数据 - RA: " + std::to_string(ra) + "°, DEC: " + std::to_string(dec) + 
                   "°, 偏移RA: " + std::to_string(offsetRa) + "°, 偏移DEC: " + std::to_string(offsetDec) + "°", 
                   LogLevel::INFO, DeviceType::MAIN);
    }
    
    // 发送信号（扩展为四角点）
    emit adjustmentGuideData(ra, dec,
                           ra0, dec0, ra1, dec1, ra2, dec2, ra3, dec3,
                           targetRa, targetDec, offsetRa, offsetDec,
                           adjustmentRa, adjustmentDec,
                           fakePolarRA, fakePolarDEC, realPolarRA, realPolarDEC);
}

void PolarAlignment::addAdjustmentGuideData(const AdjustmentGuideData& data)
{
    // 这个方法现在主要用于重新发送，不再用于添加新数据
    Logger::Log("PolarAlignment: addAdjustmentGuideData已废弃，请使用saveAndEmitAdjustmentGuideData", 
               LogLevel::WARNING, DeviceType::MAIN);
}

int PolarAlignment::getAdjustmentGuideDataCount() const
{
    return adjustmentGuideDataHistory.size();
}

void PolarAlignment::resendAllAdjustmentGuideData()
{
    Logger::Log("PolarAlignment: 重新发送所有保存的调整指导数据", LogLevel::INFO, DeviceType::MAIN);
    
    if (adjustmentGuideDataHistory.isEmpty()) {
        Logger::Log("PolarAlignment: 调整指导数据容器为空", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    
    int calibrationPointCount = 0;
    int parsedDataCount = 0;
    
    for (const AdjustmentGuideData& data : adjustmentGuideDataHistory) {
        if (data.offsetRa == 0.0 && data.offsetDec == 0.0 && data.targetRa == -1.0) {
            // 校准点数据
            calibrationPointCount++;
            Logger::Log("PolarAlignment: 重新发送校准点" + std::to_string(calibrationPointCount) + 
                       " - RA: " + std::to_string(data.ra) + "°, DEC: " + std::to_string(data.dec) + "°", 
                       LogLevel::INFO, DeviceType::MAIN);
        } else {
            // 解析数据
            parsedDataCount++;
            Logger::Log("PolarAlignment: 重新发送解析数据" + std::to_string(parsedDataCount) + 
                       " - RA: " + std::to_string(data.ra) + "°, DEC: " + std::to_string(data.dec) + 
                       "°, 偏移RA: " + std::to_string(data.offsetRa) + "°, 偏移DEC: " + std::to_string(data.offsetDec) + "°", 
                       LogLevel::INFO, DeviceType::MAIN);
        }
        
        // 发送完整的数据
        emit adjustmentGuideData(data.ra, data.dec, data.ra0, data.dec0, data.ra1, data.dec1, data.ra2, data.dec2, data.ra3, data.dec3, data.targetRa, data.targetDec,
                               data.offsetRa, data.offsetDec, data.adjustmentRa, data.adjustmentDec,
                               data.fakePolarRA, data.fakePolarDEC, data.realPolarRA, data.realPolarDEC);
    }
    
    Logger::Log("PolarAlignment: 已重新发送" + std::to_string(calibrationPointCount) + "个校准点和" + 
               std::to_string(parsedDataCount) + "个解析数据", LogLevel::INFO, DeviceType::MAIN);
}


bool PolarAlignment::moveToAvoidObstacleRA1()
{
    Logger::Log("PolarAlignment: 第二次拍摄失败，RA-2*currentRAAngle", LogLevel::INFO, DeviceType::MAIN);
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取当前望远镜位置
    double currentRA_Hours, currentDEC_Degree;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA_Hours, currentDEC_Degree);
    double currentRA = Tools::HourToDegree(currentRA_Hours);
    double currentDEC = currentDEC_Degree;
    
    Logger::Log("PolarAlignment: 当前位置 - RA: " + std::to_string(currentRA) + "°, DEC: " + std::to_string(currentDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 第二次拍摄失败：RA - 2*currentRAAngle（正常移动是RA+currentRAAngle，避障应该是RA-2*currentRAAngle）
    double targetRA = currentRA - 2.0 * currentRAAngle;
    if (targetRA < 0) targetRA += 360.0; // 确保RA在0-360度范围内
    if (targetRA >= 360.0) targetRA -= 360.0; // 确保RA在0-360度范围内
    double targetDEC = currentDEC; // 保持DEC不变
    
    Logger::Log("PolarAlignment: RA-2*currentRAAngle，目标位置 - RA: " + std::to_string(targetRA) + "°, DEC: " + std::to_string(targetDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 移动到目标位置避开遮挡
    bool success = moveTelescopeToAbsolutePosition(targetRA, targetDEC);
    if (success) {
        Logger::Log("PolarAlignment: 第一次RA轴避障移动命令发送成功", LogLevel::INFO, DeviceType::MAIN);
    }
    return success;
}

bool PolarAlignment::moveToAvoidObstacleRA2()
{
    Logger::Log("PolarAlignment: 第三次拍摄失败，根据移动方向取反", LogLevel::INFO, DeviceType::MAIN);
    if (!dpMount) {
        Logger::Log("PolarAlignment: 望远镜设备不可用", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    
    // 获取当前望远镜位置
    double currentRA_Hours, currentDEC_Degree;
    indiServer->getTelescopeRADECJNOW(dpMount, currentRA_Hours, currentDEC_Degree);
    double currentRA = Tools::HourToDegree(currentRA_Hours);
    double currentDEC = currentDEC_Degree;
    
    Logger::Log("PolarAlignment: 当前位置 - RA: " + std::to_string(currentRA) + "°, DEC: " + std::to_string(currentDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 第三次拍摄失败：根据移动方向取反，移动距离为currentRAAngle/2
    double targetRA;
    if (secondCaptureAvoided) {
        // 如果进行了第二次拍摄避障，第三次移动是RA - currentRAAngle，避障应该是RA + currentRAAngle/2
        targetRA = currentRA + currentRAAngle / 2.0;
        Logger::Log("PolarAlignment: 避障后第三次拍摄失败，RA + currentRAAngle/2", LogLevel::INFO, DeviceType::MAIN);
    } else {
        // 如果未进行第二次拍摄避障，第三次移动是RA + currentRAAngle，避障应该是RA - currentRAAngle/2
        targetRA = currentRA - currentRAAngle / 2.0;
        Logger::Log("PolarAlignment: 未避障第三次拍摄失败，RA - currentRAAngle/2", LogLevel::INFO, DeviceType::MAIN);
    }
    
    if (targetRA < 0) targetRA += 360.0; // 确保RA在0-360度范围内
    if (targetRA >= 360.0) targetRA -= 360.0;
    
    double targetDEC = currentDEC; // 保持DEC不变
    
    Logger::Log("PolarAlignment: 根据移动方向取反(currentRAAngle/2)，目标位置 - RA: " + std::to_string(targetRA) + "°, DEC: " + std::to_string(targetDEC) + "°", LogLevel::INFO, DeviceType::MAIN);
    
    // 移动到目标位置避开遮挡
    bool success = moveTelescopeToAbsolutePosition(targetRA, targetDEC);
    if (success) {
        Logger::Log("PolarAlignment: 第二次RA轴避障移动命令发送成功", LogLevel::INFO, DeviceType::MAIN);
    }
    return success;
}
