# 2026-04-14 QfdNative星点识别与HFR迁移总结

## 1. 背景

本次工作目标是参考 `/home/q/workspace_origin/nina` 中 N.I.N.A. 当前版本使用的本地星点识别与 `STARHFR` 评价思路，
将其中适合 Qt 服务端本地实现的核心算法迁移到：

- `/home/q/workspace_origin/QUARCS_QT-SeverProgram/src/focused_star_detection.cpp`

用户要求：

- 把相关代码“抽取出来”迁移到 Qt 服务端
- 给迁移过来的实现增加统一前缀，便于与原实现区分
- 前缀最好不要使用 `NINA`

最终本次迁移采用统一前缀：

- `QfdNative`

含义可理解为：

- `QUARCS Focus Detection Native`

这样既能表明它是 Qt 服务端内置的本地实现，也能与旧逻辑、Python 脚本逻辑、外部库逻辑区分开。

## 2. 迁移前原状

迁移前 `focused_star_detection.cpp` 是一个明显简化版实现，基本流程是：

1. 在整幅 ROI 图上寻找全局峰值
2. 在峰值附近固定窗口内做加权质心
3. 基于阈值窗口内信号做简单 HFR 估计
4. 通常只返回 0 或 1 颗星

这一版的特点是：

- 实现简单
- 适合单星/导星场景快速兜底
- 但不具备多星检测、局部背景稳健估计、质心迭代细化、曲线增长 HFR 等能力

## 3. N.I.N.A. 侧参考到的核心算法点

本次参考的 N.I.N.A. 代码主入口为：

- `nina/NINA.Image/ImageAnalysis/StarDetection.cs`

核心参考点包括：

1. 图像预处理后做结构检测，而不是只看单个峰值
2. 使用局部背景估计，而不是全图均值背景
3. 对每颗候选星做质心细化
4. HFR 使用 curve-of-growth（半通量半径）定义，而不是旧的一阶矩近似
5. 同时可计算 FWHM 与 eccentricity
6. 对候选星做几何尺寸、亮度、局部背景阈值、偏心率等多重筛选

需要说明：

- 本次是“参考实现思想并做 Qt/OpenCV 本地迁移”
- 不是逐行照搬 C# 代码
- 也没有把 N.I.N.A. 的插件体系、WPF 依赖、完整分析对象模型一起迁入

## 4. 本次实际改动

### 4.1 修改文件

本次改动涉及：

- `/home/q/workspace_origin/QUARCS_QT-SeverProgram/src/focused_star_detection.cpp`
- `/home/q/workspace_origin/QUARCS_QT-SeverProgram/src/tools.h`
- `/home/q/workspace_origin/QUARCS_QT-SeverProgram/src/tools.cpp`

### 4.2 新增的 QfdNative 内部实现

在 `focused_star_detection.cpp` 中新增了一套 `QfdNative` 前缀的内部算法函数与数据结构，主要包括：

- `QfdNativeState`
- `QfdNativePixelData`
- `QfdNativeRadialSample`
- `QfdNativeMeasuredStar`
- `QfdNativeToSingleChannelGray`
- `QfdNativeToGray32`
- `QfdNativeNormalizeTo8U`
- `QfdNativeEstimateBackground`
- `QfdNativeCalculateCentroid`
- `QfdNativeCollectRadialSamples`
- `QfdNativeCalculateHalfFluxRadius`
- `QfdNativeCalculateFwhm`
- `QfdNativeCalculateMomentEccentricity`
- `QfdNativeMeasureStar`
- `QfdNativeBuildState`
- `QfdNativeDetectFocusedStarsInternal`

### 4.3 当前入口行为变化

当前 `Tools::DetectFocusedStars(...)` 的行为改为：

1. 优先执行 `QfdNativeDetectFocusedStarsInternal(...)`
2. 如果 `QfdNative` 没有产出有效星点
3. 自动回退到原来的单峰值质心兜底实现 `detectPeakCentroidInternal(...)`

也就是说：

- 新算法已经接管主入口
- 旧算法没有删除
- 当前是“新算法优先 + 旧算法兜底”的保守切换方式

### 4.4 文档与日志同步

同步更新了：

- `tools.h` 中关于 `DetectFocusedStars` 的注释
- `tools.cpp` 中 `FindStarsByFocusedCppFromFile(...)` 的日志描述

使当前代码语义从：

- “simplified peak-centroid detector”

变为：

- “local C++ focused-star detector (QfdNative + fallback)”

## 5. QfdNative 当前算法流程

当前 `QfdNative` 实现可概括为：

1. 输入图像统一转灰度 `CV_32F`
2. 若图像过大，按宽度上限进行缩放以加快候选检测
3. 归一化到 8bit 检测图
4. 可选 Gaussian 平滑
5. 使用 `Canny + dilation + findContours` 获取候选结构
6. 对每个 contour：
   - 按尺寸过滤
   - 用外接圆/外接框估计星点中心和半径
   - 用矩形宽高比粗略过滤高偏心结构
7. 构造：
   - 星点小框区域
   - 外围大框背景区域
8. 对背景区做 `sigma-clipped median` 背景估计
9. 检查候选是否满足：
   - 星内平均亮度高于局部背景
   - 足够多像素高于 `background + 1.5 * sigma`
10. 通过窗口加权法迭代细化质心
11. 基于背景扣除后的径向样本：
   - 计算 HFR（curve-of-growth）
   - 计算 FWHM
   - 计算二阶矩 eccentricity
12. 再按半径分布做一次群体过滤
13. 最终按亮度/通量排序后输出 `FocusedStar`

## 6. HFR 迁移后的定义

本次迁移后，本地 HFR 计算思路已经从原来的“简单阈值窗口近似”明显向 N.I.N.A. 靠拢，具体表现为：

- HFR 基于局部背景扣除后的像素通量
- 按半径排序做累计通量
- 找到达到总正通量 50% 的半径
- 使用线性插值获得亚像素级 HFR

这比原先单窗口简单累计的方式更符合天文图像里常用的 half-flux radius 定义。

## 7. 与 N.I.N.A. 原实现的差异

虽然本次参考了 N.I.N.A. 的核心思路，但当前 Qt 版本仍与其存在差异：

### 7.1 已吸收的部分

- 局部背景估计
- sigma-clipped median
- 窗口加权质心细化
- curve-of-growth HFR
- FWHM 计算
- 二阶矩 eccentricity
- 候选星点多重过滤

### 7.2 尚未迁入的部分

- N.I.N.A. 中更细的 ROI / donut ROI 参数体系
- 按 sensitivity 不同使用更复杂的缩放策略
- 基于固定“最亮若干星”跨帧匹配同一批星的 autofocus 特化逻辑
- 完整的 `AverageHFR / HFRStdDev / AverageFWHM / AverageEccentricity` 聚合结果对象
- 将 `HFRStdDev` 直接作为 autofocus 曲线拟合误差权重输入

### 7.3 当前 Qt 版仍保留的特性

- 保留旧单峰兜底逻辑
- 对外接口 `Tools::FocusedStar` 结构未大改
- 尽量不破坏现有调用链

## 8. 当前验证情况

本次已完成的验证：

- 已检查改动后的 diff
- `git diff --check` 通过
- 代码入口、注释、日志已对齐

本次未完成的验证：

- 未能在当前 Ubuntu 虚拟机上完成 `focused_star_detection.cpp` 的真实编译验证

原因不是本次代码本身先报语法错误，而是当前环境缺少可直接用于该仓库的 OpenCV 开发头路径配置，单文件语法检查时卡在：

- `tools.h` 的 OpenCV 头文件 include 解析

因此当前结论是：

- patch 文本层面健康
- 逻辑迁移已完成
- 但仍需要在你们可正常编译 Qt 服务端的开发环境中做一次实际构建确认

## 9. 当前限制与风险

### 9.1 当前 DetectFocusedStars 仍未输出完整统计对象

当前对外输出仍是：

- `std::vector<Tools::FocusedStar>`

但 `FocusedStar` 结构目前只包含：

- 坐标
- flux
- hfr
- radius
- area
- snr
- localMax
- bgStd
- snrQuality

也就是说：

- 内部虽然已经算了 `fwhm`、`eccentricity`
- 但还没有写入对外结构

### 9.2 autofocus 主流程尚未切到本地 QfdNative 聚合逻辑

当前自动对焦主流程里仍有多处依赖：

- Python `median_HFR`

本次只完成了本地星点/HFR计算能力迁入 `focused_star_detection.cpp`，
还没有把 `autofocus.cpp` 的主采样流程系统性改为优先使用 `QfdNative` 结果。

### 9.3 当前 contour/edge 结构检测仍是 OpenCV 风格实现

N.I.N.A. 原版使用的是更完整的 blob/shape 检测链路。
当前 Qt 迁移版为了尽量在现有工程里快速落地，采用了：

- `Canny`
- `dilate`
- `findContours`

这在实现难度和可维护性上比较合适，但在极端场景下的稳定性仍需要后续实测。

## 10. 后续建议

建议后续按下面顺序继续推进：

1. 在可正常编译 Qt 服务端的环境中完成一次真实构建验证
2. 为 `Tools::FocusedStar` 增加：
   - `fwhm`
   - `eccentricity`
   - `background`
   - 可选 `supportFlux` / `backgroundSigma`
3. 在 `FindStarsByFocusedCppFromFile(...)` 中把这些字段同步灌入 `FITSImage::Star`
4. 评估是否把 `autofocus.cpp` 中依赖 Python 的 `median_HFR` 流程切到本地 `QfdNative`
5. 若后续要做更接近 N.I.N.A. 的 autofocus 拟合，可继续补：
   - 多星 HFR 均值
   - HFR 标准差
   - brightest stars 跨帧匹配
   - 用标准差做拟合权重

## 11. 一句话总结

本次工作已将 N.I.N.A. 当前版本星点识别/HFR 的核心思想，以 `QfdNative` 前缀迁移到 Qt 服务端本地 `focused_star_detection.cpp` 中，并以“新算法优先、旧算法兜底”的方式接入现有调用链，为后续彻底摆脱 Python `median_HFR` 路径打下了基础。
