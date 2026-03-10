# 内置导星功能逻辑总结（含完整步骤）& 与 PHD2 对比不足点

> 面向当前工程 `QUARCS_QT-SeverProgram` 的 **内置导星（BuiltInGuider / GuiderCore）** 代码梳理。本文按“数据流→状态机→算法细节→前端协议→参数→对比 PHD2 差距”组织，尽量把实现细节与边界条件写清楚，便于排查和二次开发。

---

### 1. 覆盖范围（代码入口/版本点）

- **导星入口**：`MainWindow` 内创建 `GuiderCore`，并通过 Qt signal/slot 把 **曝光** 和 **导星脉冲** 交给外部（INDI / 模拟器）执行。
  - 入口与串联：`src/mainwindow.cpp` 中 `MainWindow::MainWindow()` 里对 `guiderCore` 的 `connect(...)`（`requestExposure` / `requestPulse` / `requestPersistGuidingFits` 等）。
- **导星核心状态机**：`src/guiding/GuiderCore.{h,cpp}`
- **核心算法组件**：
  - **选星**：`src/guiding/GuidingStarDetector.{h,cpp}`
  - **单星质心**：`src/guiding/CentroidUtils.{h,cpp}`
  - **PHD2 风格校准状态机（移植版）**：`src/guiding/phd2/Phd2MountCalibration.{h,cpp}`
  - **PHD2 风格导星闭环（RA Hysteresis + DEC ResistSwitch）**：`src/guiding/phd2/Phd2MountGuiding.{h,cpp}` + `src/guiding/phd2/Phd2GuideAlgorithms.h`
  - **DEC 回差测量/补偿**：`src/guiding/DecBacklashEstimator.{h,cpp}`（测量），补偿/自适应逻辑在 `GuiderCore.cpp` 里
- **落盘与前端预览**：
  - FITS 固定路径：`/dev/shm/guiding.fits`
  - 每帧生成 JPG 给前端：`MainWindow::PersistGuidingFits()` → `saveGuiderImageAsJPG()`（`src/mainwindow.cpp`）
- **离线/模拟测试**：
  - 离线测试：`src/tests/guiding_offline_test.cpp` + `README_GUIDING_TEST.md`
  - 模拟帧源：`src/guiding/SimGuiderFrameSource.{h,cpp}`
- **多星导星（现状：代码存在但尚未接入主流程）**：
  - `src/guiding/MultiStarTracker.{h,cpp}`（PHD2 风格 refineOffset）
  - `GuiderCore` 预留了 `multiStarSecondaryPointsChanged(...)` 信号，但目前未看到在 `GuiderCore.cpp` 中真正产出/emit 多星数据。

---

### 2. 总体架构与数据流（从 UI 到脉冲再到下一帧）

- **2.1 设备连接时的“导星相机输出路径”配置**
  - 当导星相机 `dpGuider` 连接成功时，Qt 端会把 INDI 的上传路径配置到固定位置：
    - `indi_Client->setCCDUpload(dpGuider, "/dev/shm", "guiding")`
    - 目的：让导星帧统一出现在 `/dev/shm/guiding.fits`（便于前端刷新与算法取图）。
  - 位置：`src/mainwindow.cpp` 里设备连接后处理（两处：一次性连接、以及 AfterDeviceConnect）。

- **2.2 前端触发导星/循环曝光的命令入口**
  - `GuiderLoopExpSwitch:true/false`：只控制 **循环曝光**（Loop）。
  - `GuiderSwitch:true/false`：控制 **完整导星流程**（自动选星→校准→导星）并同时打开 Loop。
  - 位置：`MainWindow::onMessageReceived()` 中对这些命令的分支处理。

- **2.3 “曝光→回调→新帧→继续曝光”的闭环**
  - `GuiderCore` 不直接调用 INDI，而是通过信号请求外部执行：
    - `GuiderCore::requestExposure(exposureMs)` → `mainwindow.cpp` 里 `indi_Client->takeExposure(dpGuider, exposureSec)`
  - 当 INDI 收到曝光完成并落盘后，会回调：
    - `indi_Client->setImageReceivedCallback(...)`：若 `devname==dpGuider`，就把 `fitsPath` 投递给 `guiderCore->onNewFrame(fitsPath)`（QueuedConnection）。
  - `GuiderCore::onNewFrame(...)` 内部会：
    - 先 `emit requestPersistGuidingFits(fitsPath)` 让 Qt 侧复制/生成预览
    - 再按当前状态机（Selecting/Calibrating/Guiding）推进一次逻辑
    - 最后 `scheduleNextExposure(delayMs)` 继续下一帧曝光

- **2.4 “脉冲→settle→下一次曝光”的时序约束（关键）**
  - 关键动机：INDI timed guide 是异步；如果立刻曝光，会把星点拖影融合进质心，导致室外“压不住/抖动”。
  - 实现方式：
    - 校准/导星阶段如果本帧发了 pulse，则把下一次曝光延后：
      - `delayMs = pulseMs + settleMsAfterPulse`
    - 由 `GuiderCore::scheduleNextExposure(delayMs)` 完成（并用 `m_schedSeq` 取消旧调度）。

---

### 3. 状态机总览（GuiderCore）

`guiding::State` 定义在 `src/guiding/GuiderTypes.h`：

- **Idle**：空闲
- **Looping**：循环曝光（预览）
- **Selecting**：选星
- **Calibrating**：校准（含可选的 DEC 回差测量阶段）
- **Guiding**：闭环导星
- **Stopped**：停止
- **Error**：错误（仍可能继续曝光以避免“停帧误判卡住”）

状态迁移核心入口：

- **开始循环曝光**：`GuiderCore::startLoop()` → `state=Looping` → 立即请求一次 `requestExposure()`
- **开始导星**：`GuiderCore::startGuiding()`：
  - 若 Loop 未开，会先开启 Loop
  - 清理上次残留（锁星、校准、回差、滤波、诊断状态等）
  - `state=Selecting`
- **停止导星**：`GuiderCore::stopGuiding()`：
  - 取消所有调度（`m_schedSeq++`）
  - 清空锁星/校准等，避免下次复用
  - `state=Stopped`

同时 `MainWindow` 监听 `stateChanged`，向前端发送：

- `GuiderCoreState:<int>`（原样上报枚举值）
- 复用旧 PHD2 风格状态字符串：
  - Selecting → 清理覆盖层 + `ClearPHD2MultiStars`
  - Calibrating → `GuiderStatus:InCalibration`
  - Guiding → `GuiderStatus:InGuiding`（若还在判向期，则由判向结束再发送）
  - Stopped/Idle/Error → 关覆盖层、清锁星

---

### 4. 详细步骤与实现细节（按“每帧驱动”的真实执行顺序）

#### 4.1 Looping（循环曝光预览）

- **触发**：`GuiderLoopExpSwitch:true` 或导星启动时自动开启 Loop
- **动作**：
  - 每次 `requestExposure(exposureMs)` 成功后，等待 INDI 回调并进入 `onNewFrame()`
  - `PersistGuidingFits()` 会把 FITS 复制/覆盖到 `/dev/shm/guiding.fits` 并生成 JPG 给前端刷新

#### 4.2 Selecting（自动选星）

- **触发**：`startGuiding()` 后进入 `Selecting`，并且 `m_hasLock==false`
- **每帧行为（在 `GuiderCore::onNewFrame()`）**：
  - `Tools::readFits()` 读入 16-bit 图像到 `cv::Mat img16`
  - 调 `GuidingStarDetector::selectGuideStar(...)` 自动选星

- **选星算法细节（`GuidingStarDetector`）**
  - Step0：调用 `Tools::DetectFocusedStars(...)` 得到候选（包含 `x/y/snr/hfr` 等），并换算：
    - `HFD = 2 * HFR`
    - 计算候选到边缘距离 `edgeDistPx`
    - 取局部峰值 `peakADU`（用于“近饱和”过滤）
  - Step1：按 `minSNR` 过滤（默认 `10`）
  - Step2：按 `HFD` 范围过滤（默认 `1.5..12`）
  - Step3：
    - 边缘过滤：`edgeDistPx >= edgeMarginPx`（默认 `20px`）
    - 近饱和过滤：`peakADU < 0.9 * maxADU`（仅 8/16bit）
  - 最终评分选最大：
    - **SNR**（主导）
    - **远离边缘**
    - **更靠近画面中心**
    - **HFD 接近中值**（防止太小像热像素、太大像糊团）

- **选星成功后的输出**
  - `m_hasLock=true`
  - `m_lockPosPx = best(x,y)`
  - `m_lastGuideCentroid = m_lockPosPx`（避免沿用旧质心起点）
  - 对外 emit：
    - `lockPositionChanged(lockPos)`
    - `lockStarSelected(x,y,snr,hfd)`
  - 随即进入校准：`state=Calibrating`，并启动 PHD2 校准状态机

#### 4.3 Calibrating（PHD2 风格校准 + 质量门槛）

##### 4.3.1 校准步长（pulse ms）的计算（PHD2 风格简化公式）

`GuiderCore.cpp` 内部按以下信息估算校准脉冲时长：

- **imageScale（角秒/像素）**：
  - 优先使用 `GuidingParams.pixelScaleArcsecPerPixel`
  - 若为 0，则用 $206.265 * pixelSizeUm * binning / focalLengthMm$ 推导
- **总校准时间**：
  - $totalDurationSec = distancePx * imageScale / (15 * guideSpeedSidereal)$
- **每步脉冲**：
  - $pulseMs = totalDurationSec / desiredSteps * 1000$
  - 再做 `cos(dec)` 修正（使用 `calibAssumedDecDeg`，默认 0）
  - 再按 PHD2 的 MIN_STEPS=6 做上限约束，并 **向上取整到 50ms**（且最小 50ms）

> 注：`MainWindow` 在进入 Calibrating 时会尝试读取 INDI 的 `GUIDE_RATE` 并写回 `p.guideSpeedSidereal`，用于让校准步长更贴近真实 mount 响应。

##### 4.3.2 校准状态机（`MountCalibration`：GoWest→GoEast→ClearBacklash→GoNorth→GoSouth）

校准是逐帧推进的，每帧输入“当前质心位置（像素）”，返回：

- `hasPulse=true`：本帧需要发一个导星脉冲（dir + durationMs）
- `done=true`：校准完成（成功或失败）
- `failed=true`：校准失败并携带 `errorMessage`
- `result`：成功时的 `CalibrationResult`（PHD2 风格字段）

校准各阶段要点：

- **GoWest**：
  - 持续发 WEST 脉冲直到星点位移达到 `distancePx`
  - 记录 RA 轴角度 `xAngle` 与速率 `xRate = dist / (steps*durationMs)`
  - 若步数超过 `MAX_CALIBRATION_STEPS`（默认 60）仍不够位移 → 失败（“star did not move enough”）

- **GoEast（回中）**：
  - 用 East 脉冲把刚才的 West 位移回拉（按累积时间分段回拉）

- **ClearBacklash（清回差，校准内部的“北向啮合确认”）**
  - 反复 North 脉冲，要求观察到 **至少 3 次**“单步位移超过期望阈值且累计位移不反向”的移动，认为啮合建立
  - 若尝试过多且累计位移仍很小（<3px） → 失败；否则“接受并继续”

- **GoNorth**：
  - North 脉冲直到 DEC 位移达到 `distancePx`，计算 `yAngle/yRate`

- **GoSouth（回中）**：
  - 用 South 脉冲回拉

最终 `CalibrationResult` 的关键含义：

- **raUnitVec / decUnitVec**：单位向量（图像坐标系），表示“正向导星脉冲”会让星点朝哪个方向移动
  - 本实现定义：RA 正向=WEST 脉冲；DEC 正向=NORTH 脉冲
- **raMsPerPixel / decMsPerPixel**：像素→毫秒换算，用于闭环将“需要纠正多少像素”换算成“发多长脉冲”
- **cameraAngleDeg / orthoErrDeg**：用于诊断 RA/DEC 是否正交（串扰大则导星难稳定）
- **raTravelPx / decTravelPx**、**raStepCount / decStepCount**：质量诊断

##### 4.3.3 校准质量门槛（硬失败 + 警告）

在 `GuiderCore` 里校准完成后会做质量评估：

- **硬失败（直接进入 Error，不进入 Guiding）**
  - `orthoErrDeg > calibMaxOrthoErrDeg`（默认 25°）
  - `raTravelPx < calibMinAxisMovePx` 或 `decTravelPx < calibMinAxisMovePx`（默认 12px）
- **警告（继续 Guiding，但提示检查 guide-rate/脉冲有效性）**
  - `raMsPerPixel`、`decMsPerPixel` 超过 `[calibMinMsPerPixel, calibMaxMsPerPixel]`（默认 `[1,500]`）
  - 同时会输出“基于 imageScale 与 guideSpeed 推算的期望 ms/px”供对照

##### 4.3.4 校准结束后的锁点处理

- 校准阶段方框跟随“当前质心”（UI 框移动），十字线保持锁点（UI 十字）；
- 校准完成后，`GuiderCore` 会把十字线锁点 `lockPos` 更新到 `m_lastGuideCentroid`，避免进入导星时初始误差巨大。

#### 4.4 （可选）DEC 回差测量（导星前）

在 `GuiderCore` 里，校准完成后如果 `enableDecBacklashMeasure=true`（默认 true），会进入一个 **仍处于 Calibrating 状态** 的“回差测量子阶段”，由 `DecBacklashEstimator` 逐帧推进：

- **Phase1：PushNorth（北向预加载）**
  - 持续发 NORTH 脉冲，直到 DEC 轴投影位移达到 `decBacklashNorthTargetPx`（默认 20px）
  - 超时：
    - 先用 `decBacklashNorthMaxTotalMs`（默认 8000ms）作为上限
    - 但会根据校准速度 `decMsPerPixel` 自动放宽（避免 guide-rate 慢导致误失败），上限封顶 60s

- **Phase2：ProbeSouth（南向探测）**
  - 先至少发 1 次 SOUTH 探测脉冲（避免“切换阶段+seeing噪声”导致回差=0 的偏差）
  - 然后累计 SOUTH 探测总时长 `m_southTotalMs`
  - 当观测到从北向极值点开始“回拉位移”达到 `decBacklashDetectMovePx`（默认 0.4px）并连续满足 `decBacklashDetectConsecutiveFrames`（默认 2 帧），判定 **啮合开始**：
    - 记录回差估计 `backlashMs ≈ 当前 m_southTotalMs`

测量完成后：

- 成功：`m_decBacklashMsBase = backlashMs`，`m_decBacklashMsRuntime = base`
- 失败：回差置 0，但 **不阻止导星进入 Guiding**（仅提示“回差测量失败，继续导星不补偿”）
- 关键修复：测量过程会把星推离 lockPos，因此会把 `lockPosPx` 重对齐到当前质心，再进入 Guiding。

#### 4.5 Guiding（闭环导星）

##### 4.5.1 每帧质心追踪（丢星判定的关键）

每帧读 FITS 后，在 `lockPos / lastCentroid` 附近找质心：

- 优先 `FindCentroidWeightedStrict(...)`（严格版）：若 ROI 中没有像素超过阈值（sumW==0）则返回 false
  - **目的**：可靠判定“丢星”，避免在空帧/噪声中随便锁到峰值点导致假导星
- 失败时会扩大搜索窗口重试（8→16→以 lockPos 为中心 16/24）
- 有一个“谨慎的非严格回退”：
  - 如果严格失败，但 lockPos 附近 ROI 有明显峰值 `maxV>=1000`，才允许用非严格版（避免少数阈值误杀）

##### 4.5.2 误差坐标（RA/DEC 轴坐标）的计算

`MountGuiding::compute(...)` 内部：

- 误差向量：`err = currentPos - lockPos`（图像坐标，像素）
- 用校准得到的 `raUnitVec/decUnitVec` 把误差分解到轴坐标（px）：
  - 优先解 2×2 线性方程（基向量不正交也可解）
  - 退化时用点积投影

得到：

- `raErrPx`：沿 RA 轴的偏差（像素）
- `decErrPx`：沿 DEC 轴的偏差（像素）

##### 4.5.3 PHD2 风格导星算法（当前实现使用：RA Hysteresis + DEC ResistSwitch）

算法实现位于 `Phd2GuideAlgorithms.h`，行为目标是“尽量贴近 PHD2”：

- **RA：Hysteresis**
  - `dReturn = (1-h)*input + h*lastMove`
  - `dReturn *= aggression`
  - 若 `|input| < minMove` 则输出 0
  - 更新 `lastMove = dReturn`

- **DEC：ResistSwitch**
  - 核心思路：**抗反向/抗来回切换**，只在趋势可靠时允许纠正；对“微小稳定漂移”用 leaky-integrator 积累到触发阈值再打一次纠正。

重要符号约定：

- `raErr/decErr` 是“current-lock”，而算法输入是“correction”，所以喂入算法时取负号：
  - `raAlgoOutPx = raAlgo.result(-raErr)`
  - `decAlgoOutPx = decAlgo.result(-decErr)`

像素→脉冲时长（ms）：

- `ms = abs(px) * msPerPixel`，并 clamp 到 `[minPulseMs, maxPulseMs]`

方向映射（与校准定义一致）：

- RA：`algoOutPx >= 0` → WEST，否则 EAST
- DEC：`algoOutPx >= 0` → NORTH，否则 SOUTH

允许方向门控（单向导星）：

- RA/DEC 的方向若不在 `allowedRaDirs/allowedDecDirs` 集合中，则视为 **本帧该轴不允许纠正**

> 现状提醒：`Phd2MountGuiding::compute()` **只返回一个 pulse**（单轴优先），因此即使 `enableMultiAxisPulses=true`，也主要影响“选择 RA 还是 DEC 优先”。`GuiderCore` 里曾有“同一帧发 RA+DEC 两个脉冲”的旧逻辑，但已被 `#if 0` 整段禁用。

##### 4.5.4 DEC 单向策略（两种情况：小漂移 vs 大漂移）

这是内置导星的“工程化策略层”，在 PHD2 的算法之上做门控方向的自动锁定（由 `GuidingParams` 控制）：

- `autoDecGuideDir=true`（默认 true）
- 机制：
  - 导星开始先临时允许 DEC 双向（North+South），并进入“判向期”（对前端会发 `InDirectionDetection`）
  - 每帧收集 `decErrPx`，统计：
    - RMS：$\sqrt{\sum decErr^2 / N}$
    - 均值：$\sum decErr / N$
    - 同时采样 `decErr(t)`，用最小二乘拟合 slope（px/s）作为“长期漂移趋势”

两种情况：

- **case(2) 大漂移（对极轴不准）**
  - 当 RMS ≥ `decUniLargeMoveRmsPx`（默认 2.0px）认为“大漂移”
  - 只收集前 `decUniInitialFrames`（默认 5）帧快速锁定
  - 若 `|mean| >= decUniMinAbsMeanPx`（默认 0.05）：
    - mean>0 → 允许 SOUTH
    - mean<0 → 允许 NORTH

- **case(1) 小漂移（对极轴很准）**
  - 收集 `decUniCollectFrames`（默认 30）帧
  - 优先用 slope 判定（|slope|≥`driftDetectMinAbsSlopePxPerSec`，默认 0.02px/s）：
    - slope>0 → 允许 SOUTH
    - slope<0 → 允许 NORTH
  - slope 不可靠则退化为“当前误差方向”锁定

锁定完成后：

- 更新 `allowedDecDirs={chosenDir}`（单向）
- 发送 `directionDetectionStateChanged(false)`，前端恢复 `InGuiding`

##### 4.5.5 DEC 回差补偿（运行时）

当 `enableDecBacklashCompensation=true` 且运行时回差 `m_decBacklashMsRuntime>0`：

- 若本帧要发 DEC pulse，且方向相对上一次实际 DEC pulse 方向发生反转：
  - 追加一次性回差补偿：`cmd.durationMs += m_decBacklashMsRuntime`
  - 总时长 clamp 到 `maxPulseMs`

同时支持 **自适应回差（runtime）**（`enableDecBacklashAdaptive=true`）：

- 在方向反转并补偿后，观察 `decBacklashAdaptiveWindowFrames`（默认 3）帧：
  - 记录误差是否有足够改善（`decBacklashAdaptiveMinImprovePx`，默认 0.15px）
  - 若改善不足：runtime += `decBacklashAdaptiveStepMs`（默认 20ms）
  - 若出现 overshoot（误差符号反转且仍明显超出 deadband）：runtime -= step
  - runtime 上限：`min(decBacklashAdaptiveMaxMs, maxPulseMs)`（默认 2000ms）

##### 4.5.6 脉冲有效性检测（Pulse Effect Check）

目的：室外常见问题是“Tracking=OFF 导致驱动拒绝 timed guide / 导星速率太低 / 脉冲根本不生效”，肉眼难排。

- 当发出脉冲且当前误差达到 `pulseEffectMinStartAbsErrPx`（默认 1.0px）时，启动观察窗口：
  - 观察 `pulseEffectWindowFrames`（默认 3）帧内误差最小值
  - 若改善量 `< pulseEffectMinImprovePx`（默认 0.08px）认为“几乎无效”
  - 连续无效达到 `pulseEffectMaxFailures`（默认 5）则：
    - `errorOccurred("PulseNoEffect:CheckGuideRateOrMountDriver")`
    - 状态进入 `Error`

同时 `MainWindow` 层也做了一个重要兜底：

- 发脉冲前若发现 mount Tracking=OFF，会先 set Tracking=ON，并延迟 300ms 再发脉冲（避免 INDI 驱动拒绝）。

##### 4.5.7 Rolling RMS 诊断

- `GuiderCore` 维护一个滚动窗口（默认 60 帧），计算：
  - RA RMS、DEC RMS、Total RMS（$\sqrt{\text{mean}(ra^2+dec^2)}$）
- 每 `rmsEmitEveryFrames`（默认 5）帧输出一次 infoMessage：
  - 若已知像素尺度（arcsec/px），同时输出角秒 RMS

##### 4.5.8 丢星恢复（硬恢复策略）

- 连续质心失败次数 `>= maxConsecutiveCentroidFails`（默认 10）：
  - 清空锁星/校准
  - 回退到 `Selecting`
  - 重新走“选星→校准→导星”

> 对比 PHD2：PHD2 通常有更细的 re-acquire 策略，可能不必每次都重新校准；当前实现属于“更稳但更重”的恢复方式。

##### 4.5.9 中天翻转后强制重新校准（PHD2 常规做法）

- `MainWindow::onTimeout()` 会监测 `PierSide` 变化
- 若发生中天翻转且当前处于 Guiding/Calibrating：
  - `guiderCore->stopGuiding(); guiderCore->startGuiding();`
  - 同时给前端提示：`GuiderCoreInfo:MeridianFlipDetected:Recalibrating`

---

### 5. 导星图像落盘与前端显示（/dev/shm/guiding.fits + JPG）

#### 5.1 FITS 固定路径策略

`PersistGuidingFits(sourceFitsPath)`：

- 若 INDI 返回路径不是 `/dev/shm/guiding.fits`：
  - 删除旧的 `/dev/shm/guiding.fits`
  - `QFile::copy(sourceFitsPath, "/dev/shm/guiding.fits")`
- 目的：前端与其它模块都只需盯固定路径，不用追踪临时文件名。

#### 5.2 每帧生成 JPG（前端复用旧协议）

流程：

- `Tools::readFits()` 读入图像
- 更新 `glPHD_CurrentImageSizeX/Y`（用于覆盖层坐标换算）
- AutoStretch：计算 B/W（黑白点），并把 16bit 拉伸到 8bit
- `saveGuiderImageAsJPG(img8)`：
  - 清理旧 `GuiderImage*.jpg`
  - 写入新文件 `GuiderImage_<uuid>.jpg`
  - 创建软链接到前端静态目录
  - 通知前端：
    - `GuideSize:w:h`
    - `SaveGuiderImageSuccess:<fileName>`

#### 5.3 覆盖层与曲线协议（WebSocket）

内置导星为了减少前端改动，复用了大量旧 PHD2 前端覆盖层协议（框/十字/多星点），并新增了更明确的 BuiltInGuider 消息。

- **状态与提示**
  - **GuiderCore 状态枚举**：`GuiderCoreState:<int>`
  - **阶段字符串（PHD2 风格 UI）**：
    - `GuiderStatus:InCalibration`
    - `GuiderStatus:InGuiding`
    - `GuiderStatus:InDirectionDetection`
  - **启停标志**：`GuiderUpdateStatus:1/0`
  - **信息/错误**：
    - `GuiderCoreInfo:<text>`
    - `GuiderCoreError:<reason>`

- **校准结果上报（PHD2 风格字段，便于 UI 直接显示）**
  - `GuiderCalibration:cameraAngleDeg=...:orthoErrDeg=...:raRatePxPerSec=...:decRatePxPerSec=...:raMsPerPixel=...:decMsPerPixel=...`

- **选星信息**
  - `GuiderStarSelected:x=...:y=...:snr=...:hfd=...`

- **脉冲日志**
  - `GuiderPulse:<DIR>:<ms>:raErrPx=<...>:decErrPx=<...>`
  - 注：校准阶段会用 `raErrPx/decErrPx=N/A`（NaN）表示没有误差概念。

- **导星曲线数据（每帧都发，哪怕本帧不出脉冲）**
  - 折线：`AddLineChartData:<x>:<raErrPx>:<decErrPx>`（前端拆成两条曲线）
  - X 轴窗口自动滚动：
    - `SetLineChartRange:0:50` 或 `SetLineChartRange:<x-50>:<x>`
  - 散点：`AddScatterChartData:<-raErrPx>:<-decErrPx>`（这里取了负号，用于与前端既有坐标系对齐）

- **导星画面覆盖层（复用旧 PHD2 覆盖层协议）**
  - **显示开关**
    - `PHD2StarBoxView:true/false`
    - `PHD2StarCrossView:true/false`
  - **十字线位置（锁点）**
    - `PHD2StarCrossPosition:<imgW>:<imgH>:<x>:<y>`
  - **方框位置（当前质心跟随）**
    - `PHD2StarBoxPosition:<imgW>:<imgH>:<x>:<y>`
  - **多星副星点（绿色圆圈）**
    - 清理：`ClearPHD2MultiStars`
    - 下发：`PHD2MultiStarsPosition:<imgW>:<imgH>:<x>:<y>`（最多 8 个副星点）
  - 注：`PersistGuidingFits()` 会维护 `glPHD_CurrentImageSizeX/Y`，以保证上述覆盖层坐标换算正确；当尺寸尚未知时，部分消息会“缓存等尺寸就绪/进入 InGuiding 再补发”。

---

### 6. 关键参数（GuidingParams 默认值、单位、影响面）

> 参数结构体：`src/guiding/GuiderTypes.h` 的 `guiding::GuidingParams`。  
> 前端通过 WebSocket 命令动态修改，Qt 侧在 `MainWindow::onMessageReceived()` 中映射到 `guiderCore->setParams(...)`。

#### 6.1 曝光与时序

- **`exposureMs`（默认 1500ms）**：导星相机曝光时长，直接影响“每帧误差更新频率”和“脉冲闭环周期”。
  - 前端命令：`GuiderExpTimeSwitch:<ms>`
- **`settleMsAfterPulse`（默认 150ms）**：每次脉冲后额外等待；用于避免脉冲/曝光重叠导致拖影。
  - 前端命令：`GuiderSettleMsAfterPulse:<ms>`
- **`interPulseDelayMs`（默认 0）**：多轴脉冲间隔（当前 PHD2 路径通常只发单轴脉冲，实际作用有限）。
  - 前端命令：`GuiderInterPulseDelayMs:<ms>`
- **`enableMultiAxisPulses`（默认 true）**：理论上允许同一帧周期发 RA+DEC 两个脉冲；但当前 `Phd2MountGuiding::compute()` 只输出单脉冲，真正“多轴同帧”的旧逻辑已被禁用（`#if 0`）。
  - 前端命令：`GuiderEnableMultiAxisPulses:true/false`

#### 6.2 校准（PHD2 风格步长估算）相关

- **`guideSpeedSidereal`（默认 0.5）**：导星速率（恒星时速倍数），用于估算校准 pulse ms。
  - Qt 在进入 `Calibrating` 时会尝试从 INDI 读取 GUIDE_RATE 并同步此值。
- **像素尺度输入（影响校准 pulse ms 估算与 RMS 角秒换算）**
  - **`pixelScaleArcsecPerPixel`**：角秒/像素（优先使用）
  - 若为 0，则用：
    - `guiderPixelSizeUm`、`guiderFocalLengthMm`、`guiderBinning` 推导
  - Qt 侧会在收到前端：
    - `GuiderFocalLength:<mm>` 与 `GuiderPixelSize:<um>` 后计算：
      - `pixelScaleArcsecPerPixel = (pixelSizeUm * 206.265) / focalLengthMm`
- **`calibDesiredSteps`（默认 12）**：期望步数（用于估算每步脉冲）
- **`calibDistancePx`（默认 25px）**：期望校准位移距离
- **`calibAssumedDecDeg`（默认 0）**：用于 `cos(dec)` 修正（当前未看到自动从 mount DEC 写入；若后续要对齐 PHD2，可考虑在校准前读取 mount DEC 写入此字段）。

#### 6.3 导星算法（PHD2 风格）

- **死区/最小移动阈值**
  - **`deadbandPx`（默认 0.40px）**：小于此误差不出脉冲（也作为 PHD2 算法的 `minMove`）。
    - 前端命令：`GuiderDeadbandPx:<px>`
- **RA（Hysteresis）**
  - **`enableRaHysteresis`（默认 true）**
    - 前端命令：`GuiderEnableRaHysteresis:true/false`
  - **`raHysteresis`（默认 0.7，0..1）**
    - 前端命令：`GuiderRaHysteresis:<0..1>`
  - **`raAggression`（默认 0.8）**：RA 纠正幅度系数（PHD2 风格）
    - 前端命令：`GuiderRaAggression:<float>`
- **DEC（ResistSwitch）**
  - **`decAggression`（默认 0.8）**
    - 前端命令：`GuiderDecAggression:<float>`

#### 6.4 脉冲限幅

- **`minPulseMs`（默认 20ms）**、**`maxPulseMs`（默认 1200ms）**
  - 前端命令：`GuiderMinPulseMs:<ms>`、`GuiderMaxPulseMs:<ms>`

#### 6.5 单向导星（Allowed Dirs）与 DEC 单向策略

- **RA 方向**：当前实现“强制双向”（East+West），会忽略前端对 RA 单向/AUTO 的请求。
  - 前端命令：`GuiderRaGuideDir:<...>`（Qt 侧会强制设为双向）
- **DEC 方向**
  - 手动：`GuiderDecGuideDir:NORTH|SOUTH`
  - AUTO：`GuiderDecGuideDir:AUTO`（可带 `AUTO (NORTH)`/`AUTO (SOUTH)` 作为初始提示方向）
  - DEC 单向策略参数：
    - `GuiderDecUniCollectFrames`
    - `GuiderDecUniInitialFrames`
    - `GuiderDecUniLargeMoveRmsPx`

#### 6.6 DEC 回差（测量/补偿/自适应）

- **测量开关**：`enableDecBacklashMeasure`（默认 true）
- **测量参数**（单位：px / ms）
  - `decBacklashNorthTargetPx`（默认 20px）
  - `decBacklashNorthPulseMs`（默认 300ms）
  - `decBacklashNorthMaxTotalMs`（默认 8000ms，且可被“按校准速度”自动放宽）
  - `decBacklashProbeStepMs`（默认 100ms）
  - `decBacklashProbeMaxTotalMs`（默认 6000ms）
  - `decBacklashDetectMovePx`（默认 0.4px）
  - `decBacklashDetectConsecutiveFrames`（默认 2）
- **补偿与自适应**
  - `enableDecBacklashCompensation`（默认 true）
  - `enableDecBacklashAdaptive`（默认 true）
  - `decBacklashAdaptiveWindowFrames`（默认 3）
  - `decBacklashAdaptiveMinImprovePx`（默认 0.15px）
  - `decBacklashAdaptiveStepMs`（默认 20ms）
  - `decBacklashAdaptiveMaxMs`（默认 2000ms）

#### 6.7 丢星恢复、脉冲有效性检测、RMS 诊断

- **丢星恢复**：`maxConsecutiveCentroidFails`（默认 10）
  - 前端命令：`GuiderMaxConsecutiveCentroidFails:<n>`
- **脉冲有效性检测**
  - `enablePulseEffectCheck`（默认 true）
  - `pulseEffectWindowFrames`（默认 3）
  - `pulseEffectMinImprovePx`（默认 0.08px）
  - `pulseEffectMinStartAbsErrPx`（默认 1.0px）
  - `pulseEffectMaxFailures`（默认 5）
  - 前端命令：`GuiderEnablePulseEffectCheck`、`GuiderPulseEffectWindowFrames`、`GuiderPulseEffectMinImprovePx`、`GuiderPulseEffectMinStartAbsErrPx`、`GuiderPulseEffectMaxFailures`
- **RMS 统计**
  - `rmsWindowFrames`（默认 60）
  - `rmsEmitEveryFrames`（默认 5）
  - 前端命令：`GuiderRmsWindowFrames`、`GuiderRmsEmitEveryFrames`

---

### 7. 与 PHD2 对比：已实现点与主要不足点

#### 7.1 已对齐/已具备（与 PHD2 思路一致的部分）

- **校准状态机**：GoWest→GoEast→ClearBacklash→GoNorth→GoSouth（PHD2 `scope.cpp` 风格）
- **校准步长估算**：按 imageScale、guideSpeed、distance、steps 推导 pulse ms（含 `cos(dec)` 修正位）
- **导星算法骨架**：RA Hysteresis + DEC ResistSwitch（PHD2 常用组合）
- **单向门控**：通过 `allowed{Ra,Dec}Dirs` 进行方向门控（尤其 DEC 单向）
- **工程化增强（更偏“设备鲁棒性”）**
  - 校准质量硬门槛（ortho/位移）
  - DEC 回差测量（导星前）+ 运行时回差补偿/自适应
  - PulseNoEffect 检测（帮助定位“脉冲无效/追踪未开/guide-rate 异常/驱动拒绝”）
  - Rolling RMS 提示
  - 中天翻转后强制重新校准（PHD2 常规做法）

#### 7.2 主要不足点（对照 PHD2 的功能面/成熟度）

下面按“直接影响导星效果/稳定性”的优先级排序：

- **(A) 多星导星未真正接入**
  - 现状：工程里已有 `MultiStarTracker`（PHD2 风格 refineOffset），`MainWindow` 也支持向前端画副星圆圈，但 `GuiderCore` 当前并未产出/emit 多星点与 refined offset。
  - 影响：在 seeing 抖动明显时，PHD2 的多星平均对降低噪声、减少脉冲抖动非常关键；当前实现主要依赖单星 + hysteresis/EMA，抗 seeing 能力有限。

- **(B) 算法选项与调参空间远少于 PHD2**
  - PHD2 支持多种 RA/DEC 算法（例如 Predictive PEC、Lowpass2 等）和更丰富的内部状态/诊断。
  - 当前实现基本固定为：RA Hysteresis、DEC ResistSwitch（且部分 DEC 行为又被“单向策略层”覆盖），缺少更高级的预测/滤波/特定 mount 场景优化。

- **(C) “同帧双轴脉冲”能力未完整落地**
  - `GuidingParams.enableMultiAxisPulses=true` 但 PHD2 路径输出只给一个 pulse；旧实现里“同一帧序列发 RA+DEC”被禁用。
  - 影响：某些场景下（例如 RA 和 DEC 同时偏离且幅度都明显）需要更快的双轴纠正，单轴优先会拉长收敛时间。

- **(D) 校准步长的 dec 修正未接入真实 DEC**
  - `calibAssumedDecDeg` 默认 0，代码里有 `cos(dec)` 修正位，但未看到在校准前从 mount 实时 DEC 写入该值。
  - 影响：高纬度目标校准步长可能偏小/偏大，导致校准步数异常、质量门槛触发或精度下降。

- **(E) PHD2 级别的“诊断与可视化”缺失**
  - PHD2 通常提供：导星日志文件、校准细节图、星质量/星质量警报、SNR/HFD/星质量曲线、助手（Guiding Assistant）、漂移对准等。
  - 当前实现已有基础日志与 RMS/PulseEffect，但整体“可解释性、可回放分析、可自动给建议”的能力不足。

- **(F) 相机校正链路不足（暗场/热像素/坏点/背景建模）**
  - 当前质心算法主要靠 ROI 均值+方差阈值，没有暗场库、坏点图、热点抑制、背景模型等。
  - 影响：热像素/云层/薄雾/渐变背景时，锁星稳定性和丢星判断更容易受影响。

- **(G) 丢星恢复策略偏“重启式”**
  - 当前连续失败直接回到 Selecting 并重新校准。
  - PHD2 往往会尝试更轻量的 re-acquire/搜索/降级策略，减少中断和不必要的重新校准。

- **(H) 实用功能缺项（工作流差异）**
  - **Dither（抖动）与 settle 判据**：当前未看到 dither 相关实现（对拍摄流程影响很大）。
  - **校准复用/持久化**：未看到保存/恢复校准（PHD2 可在条件满足时复用）。
  - **AO 支持、复杂 mount 建模、更多边界处理**：当前范围内未覆盖。

---

### 8. 建议的改进路线（按投入产出比排序）

- **第一优先级（直接提升效果）**
  - **接入 `MultiStarTracker` 到 `GuiderCore`**：
    - Selecting 时选出副星参考点
    - Guiding 时对 primary offset 做 refine，必要时 emit `multiStarSecondaryPointsChanged` 让 UI 画圈
  - **把 mount 实时 DEC 写入 `calibAssumedDecDeg`**（校准开始前读取一次）
  - **完善 enableMultiAxisPulses 真正“同帧双轴脉冲序列”**（当前 PHD2 路径仅单脉冲）

- **第二优先级（提升鲁棒性/可调性）**
  - 增加更多导星算法选项（至少提供 PHD2 常用几种）并暴露关键参数
  - 加入暗场/坏点/背景处理（哪怕是简单的热像素/坏点掩膜也能明显提升锁星）
  - 丢星恢复：先尝试扩大搜索/重捕获，不立即强制重新校准

- **第三优先级（工程化体验）**
  - 实现导星日志落盘（每帧：时间戳、RA/DEC error、pulse、SNR/HFD 等），支持回放与自动诊断
  - 增加“Guiding Assistant”类的自动建议（极轴误差估计、min-move/曝光建议、延迟建议等）
  - 增加 dither + settle 判据，与拍摄调度联动

---

### 9. 快速排障清单（结合当前实现的“最常见坑”）

- **脉冲无效/误差不降**
  - 看前端/日志是否出现 `PulseNoEffect:CheckGuideRateOrMountDriver`
  - 确认 mount Tracking 是否被打开（Qt 会尝试自动打开，但某些驱动可能仍失败）
  - 检查 INDI 的 GUIDE_RATE / guide speed 是否合理（过小会导致校准/导星步长异常）
- **频繁校准失败**
  - 看 `CalibrationQualityFailed` 的硬失败原因（orthoErr / axisMove）
  - 重点排查：校准步长是否太小、脉冲是否被驱动拒绝、导星速率是否异常
- **DEC 单向锁错方向**
  - 调 `decUniCollectFrames/decUniInitialFrames/decUniLargeMoveRmsPx`，并观察判向阶段的 infoMessage（frames/rms/mode）
  - 极轴误差较大时，快速判向更可靠；极轴很准时，收集更久的 slope 更可靠
- **星点丢失**
  - 看质心失败次数是否达到阈值触发重选星
  - 若经常误判丢星：可能曝光过短/过曝/云层/阈值过严/搜索窗口太小，需要改 ROI 策略或加入坏点/暗场处理

