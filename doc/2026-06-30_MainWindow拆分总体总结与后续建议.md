# MainWindow 拆分总体总结与后续建议

最后更新：2026-06-30

## 1. 背景与目标

本轮工作的起点，是 `src/mainwindow.cpp` 长期承担过多职责，既包含主控入口，也混合了网络、极轴、主相机采集、导星运行时、GPIO、系统维护、历史兼容代码等内容。

前期目标主要是：

- 先把 `mainwindow.cpp` 从超大单文件状态拆开
- 在尽量不改协议、不重设计成员归属的前提下完成低风险迁移
- 优先按“功能域”拆分，而不是先做大规模架构重写

后期目标逐步转变为：

- 不再单纯追求降行数
- 更强调文件职责边界清晰
- 让拆分后的文件在语义上更独立、更容易长期维护

## 2. 当前总体结果

截至本次总结，`mainwindow.cpp` 已从最初的 `8000+` 行下降到约 `1298` 行。

当前与 `MainWindow` 相关的主要实现文件行数如下：

- `src/mainwindow.cpp`：1298
- `src/mainwindow_network.cpp`：1111
- `src/mainwindow_polar_alignment.cpp`：1004
- `src/mainwindow_main_camera_capture.cpp`：1136
- `src/mainwindow_runtime_lifecycle.cpp`：99
- `src/mainwindow_startup_config.cpp`：195
- `src/mainwindow_external_phd2.cpp`：1675
- `src/mainwindow_indi.cpp`：387
- `src/mainwindow_gpio.cpp`：205
- `src/mainwindow_settings_maintenance.cpp`：757
- `src/mainwindow_guiding_runtime.cpp`：725

从结果上看，`mainwindow.cpp` 已经不再是“巨石文件”，而是回到一个更像“主控入口和少量总装逻辑”的体量。

## 3. 本轮拆分的主要阶段

### 3.1 按功能域做第一轮切分

这一阶段优先拆出边界最清晰、业务相对独立的内容：

- `mainwindow_network.cpp`
  - 网络状态
  - Wi-Fi 扫描与保存
  - 热点名称读写
  - 热点切换与重启

- `mainwindow_polar_alignment.cpp`
  - 自动极轴
  - PoleMaster 极轴
  - 模拟极轴
  - 极轴拍摄结果收口

- `mainwindow_main_camera_capture.cpp`
  - 主相机曝光入口
  - SDK 单拍 / Burst
  - 曝光轮询
  - 曝光中止

这一轮的特点是：收益大、风险低，能快速降低主文件体量。

### 3.2 启动与生命周期整理

随后把构造/析构周边内容继续收口：

- `mainwindow_runtime_lifecycle.cpp`
  - 析构与运行时清理
  - worker / timer 生命周期收口

- `mainwindow_startup_config.cpp`
  - 图像保存目录初始化
  - `/img` 映射检查
  - WebSocket 初始化

这一步的意义不只是减行数，更重要的是让构造函数逐渐从“堆满初始化细节”转为“调用若干语义化步骤”。

### 3.3 历史兼容与接入层拆出

后续把接入层和旧兼容代码分离出来：

- `mainwindow_external_phd2.cpp`
  - 旧 PHD2 / 外部导星兼容逻辑
  - 历史共享内存控制
  - 兼容性保留代码

- `mainwindow_indi.cpp`
  - INDI server/client 初始化
  - INDI 消息接入
  - 当前仍包含部分“接入后业务分发”

这里需要特别说明：

- `mainwindow_external_phd2.cpp` 的语义已经比较明确，本质上就是 `legacy / compatibility`
- `mainwindow_indi.cpp` 虽然已经从主文件拆出，但从语义角度仍然偏“接入层 + 到达后业务分发混合”

### 3.4 系统维护与 GPIO 收口

再往后拆出了更偏系统层的模块：

- `mainwindow_gpio.cpp`
  - GPIO 初始化
  - GPIO 读写
  - GPIO 状态同步

- `mainwindow_settings_maintenance.cpp`
  - 客户端配置读写
  - ROI 参数同步
  - CPU 信息上报
  - 主相机参数恢复
  - 最近设备选择恢复
  - 日志清理
  - 缓存/更新包/备份清理

这一轮拆分后，主文件中的“杂项堆积”显著减少。

### 3.5 从“降行数”转向“导星运行时语义收口”

在主文件降到大约 `2000` 行后，继续按“哪里长拆哪里”已经不再是最优策略。因此本轮后期改为按语义边界继续整理，重点收口“导星运行时”：

- 新增 `mainwindow_guiding_runtime.cpp`
  - 内置导星 `GuiderCore` 初始化与 signal/slot wiring
  - 极轴单拍运行时 `startPoleCameraSingleCapture`
  - SDK 导星轮询 `onSdkGuiderExposureTimerTimeout`

这一轮的意义在于：

- `mainwindow_guiding.cpp` 更接近“导星控制接口”
- `mainwindow_guiding_runtime.cpp` 更接近“导星运行时与帧流转”
- `mainwindow.cpp` 从具体流程中进一步抽离出来

## 4. 当前模块语义评估

从“语义独立、模块干净、边界清晰”的角度看，目前可分为三类。

### 4.1 语义已经比较清晰的模块

这些文件已经能较好回答“这个文件负责什么”：

- `mainwindow_network.cpp`
- `mainwindow_gpio.cpp`
- `mainwindow_polar_alignment.cpp`
- `mainwindow_main_camera_capture.cpp`
- `mainwindow_settings_maintenance.cpp`
- `mainwindow_runtime_lifecycle.cpp`
- `mainwindow_startup_config.cpp`

这部分整体状态较好，后续不建议频繁再拆。

### 4.2 语义上已经改善，但仍有进一步优化空间的模块

- `mainwindow_guiding.cpp`
  - 当前更像“导星控制接口层”
  - 与 `mainwindow_guiding_runtime.cpp` 之间的边界已经开始清晰，但还可以持续稳定

- `mainwindow_guiding_runtime.cpp`
  - 已经形成较明确的“导星运行时”模块
  - 是本轮最有价值的语义收口之一

- `mainwindow_indi.cpp`
  - 当前既做 transport 接入，也做图像/事件到达后的业务分发
  - 从语义角度讲，这仍然不是最理想状态

### 4.3 本质上属于历史兼容层的模块

- `mainwindow_external_phd2.cpp`

这个文件更适合作为：

- 兼容层
- 历史参考层
- 可选启用层

而不应继续向主流程扩散新的业务逻辑。

## 5. 当前 `mainwindow.cpp` 的角色变化

经过这一轮拆分后，`mainwindow.cpp` 的语义已经发生了明显变化：

### 5.1 过去的角色

过去的 `mainwindow.cpp` 同时承担：

- 主控入口
- 设备接入
- 主相机采集
- 极轴
- 导星
- 网络
- 配置恢复
- GPIO
- 系统维护
- 历史兼容逻辑

这导致它既大又混杂，阅读成本很高。

### 5.2 现在的角色

现在的 `mainwindow.cpp` 更接近：

- 主控入口
- 主控级状态判断
- 少量运行时协调
- 少量跨域逻辑

这已经是比较合理的方向。

因此，后续不建议再把“继续压缩 `mainwindow.cpp` 行数”作为主目标。

## 6. 业务机编译验证情况

本轮拆分不仅在本地做了 `cmake` 配置验证，还实际使用了树莓派本机一键编译部署脚本进行验证：

- 脚本：`build_and_deploy_rpi_native.sh`
- 目标业务机：`172.24.217.51`
- 最近一次通过版本：`202606302042`

验证过程中确实暴露过两类真实问题：

- 拆分后局部 helper / `extern` 可见性丢失
- 新文件中沿用了原来主文件局部 helper，但没有同步搬过去

这些问题已经在本轮处理并重新验证通过。

截至本次总结：

- 树莓派本机编译通过
- 部署通过
- `client` 已可在业务机正常启动

当前仍然存在的只是旧告警，不阻塞编译部署，例如：

- `QNetworkConfigurationManager` 已弃用告警
- `mainwindow_mount_control.cpp` 中若干旧函数返回路径不完整

## 7. 对当前结构的总体判断

### 7.1 已经达到的效果

本轮拆分已经完成了三个重要目标：

- `mainwindow.cpp` 从“超大单文件”回到可维护区间
- 多数功能域已经形成相对独立文件
- 后续继续重构时，可以按模块局部演进，而不必再碰整块巨石文件

### 7.2 当前最重要的变化

真正最重要的并不是行数下降本身，而是：

- 主相机、极轴、网络、GPIO、维护杂项、导星运行时，已经开始拥有自己的独立语义空间
- 继续开发时，新增逻辑不再必须塞回 `mainwindow.cpp`

这意味着项目已经从“单点拥塞”转向“模块化可继续演化”的状态。

## 8. 后续工作建议

后续建议不再以“继续拆主文件”为优先，而应改为“稳定边界、整理语义”。

### 8.1 建议优先级最高：不要继续以压缩 `mainwindow.cpp` 为目标

原因：

- 主文件已经进入合理体量区间
- 继续强拆边际收益明显下降
- 如果过度拆分，可能反而造成跳转过多、语义碎裂

### 8.2 建议继续关注 `mainwindow_indi.cpp` 的边界

这是当前语义上最值得继续优化的一块。

建议方向：

- 把 `INDI transport` 和 “图像/消息到达后的业务分发” 分离
- 让 `mainwindow_indi.cpp` 更接近纯接入层
- 让主相机/导星/极轴收到帧后的业务处理，回归各自语义模块

可以考虑未来演化成：

- `mainwindow_indi_transport.cpp`
- `mainwindow_indi_callbacks.cpp`

或者按业务域把 callback 分流回对应模块。

### 8.3 建议把 `mainwindow_external_phd2.cpp` 明确视为 legacy 模块

建议：

- 不再往里面添加新的主流程逻辑
- 后续如有条件，可改名为更明确的 `mainwindow_legacy_phd2.cpp`
- 文档中明确说明其历史兼容身份

### 8.4 建议做一次“模块职责约定”文档化

目前拆分已经做出来了，但如果没有约定，后续开发很容易再次把新逻辑塞回主文件。

建议未来补一份简短约定，例如：

- `mainwindow.cpp`
  - 只保留主控入口、总装、少量跨域协调
- `mainwindow_guiding.cpp`
  - 只保留导星控制接口
- `mainwindow_guiding_runtime.cpp`
  - 保留导星运行时、帧轮询、GuiderCore wiring
- `mainwindow_indi.cpp`
  - 尽量只保留接入层
- `mainwindow_settings_maintenance.cpp`
  - 保留配置恢复、系统信息、维护清理

### 8.5 建议后续优先处理“旧告警与小缺陷”，而不是继续拆分

比起继续分文件，更有现实价值的工作包括：

- 修补 `mainwindow_mount_control.cpp` 中非 `void` 函数缺返回值的问题
- 评估并逐步替换弃用的 `QNetworkConfigurationManager`
- 清理少量历史注释块和无效残留代码

这类工作虽然不如“拆文件”显眼，但对长期稳定性更有帮助。

## 9. 本次阶段结论

可以把这次拆分工作视为一个阶段性完成点。

阶段结论如下：

- `mainwindow.cpp` 的“巨石化问题”已经得到明显缓解
- 模块化已经初步建立
- 继续工作的重点应从“继续拆主文件”转向“稳定模块职责与边界”

因此，后续的推荐策略是：

1. 暂停继续压缩 `mainwindow.cpp`
2. 以语义边界为中心，谨慎整理 `mainwindow_indi.cpp`
3. 把 `external_phd2` 固化为 legacy 层
4. 优先处理现存旧告警和局部质量问题
5. 通过文档约束，避免后续新逻辑重新回流到主文件

