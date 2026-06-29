# 2026-06-29 MainWindow 拆分阶段总结

## 1. 背景

`src/mainwindow.cpp` 长期承载了过多职责，包括：

- WebSocket 消息入口与命令分发
- 设备连接生命周期
- 图像处理与保存链路
- 导星相关控制
- 电调与自动对焦
- 计划任务
- 设备配置/串口/绑定恢复
- 望远镜控制与解算同步

在开始本轮工作前，`mainwindow.cpp` 体量约为 `34151` 行，已经明显超出单文件可维护范围。  
本轮工作的目标不是一次性重写，而是采用“低风险结构拆分”方式，将已有逻辑按职责域迁移到新的实现文件中，同时保证：

- 不改协议
- 不改业务行为
- 不改变成员状态归属
- 每一轮都经过真实编译/部署验证

## 2. 拆分原则

本次拆分始终遵循下面几条原则：

- 优先拆“实现文件归属”，不优先拆“类设计”
- 优先拆职责清晰、边界稳定的功能块
- 尽量保持函数签名不变
- 尽量避免同时改逻辑和改结构
- 每轮以树莓派原生编译部署通过作为收口标准

这意味着本次更像是“为后续继续重构铺路”的第一阶段，而不是终局性的架构重做。

## 3. 拆分结果

### 3.1 当前 `mainwindow.cpp`

本轮结束后：

- `src/mainwindow.cpp`：`8752` 行

相比开始时的约 `34151` 行，主文件已经显著收缩，核心收益是：

- `mainwindow.cpp` 不再继续堆积所有领域逻辑
- 后续继续拆分时，切入点更清楚
- 编译和部署验证已经证明现有拆分方式可持续复用

### 3.2 已拆出的实现文件

本阶段累计拆出了以下实现文件：

- `src/mainwindow_commands_capture.cpp`：`1610` 行
- `src/mainwindow_commands_control.cpp`：`1462` 行
- `src/mainwindow_commands_device.cpp`：`807` 行
- `src/mainwindow_device_connection.cpp`：`9386` 行
- `src/mainwindow_image_pipeline.cpp`：`3515` 行
- `src/mainwindow_guiding.cpp`：`994` 行
- `src/mainwindow_focuser.cpp`：`1645` 行
- `src/mainwindow_focus_loop.cpp`：`596` 行
- `src/mainwindow_autofocus.cpp`：`807` 行
- `src/mainwindow_schedule.cpp`：`900` 行
- `src/mainwindow_storage.cpp`：`1358` 行
- `src/mainwindow_device_config.cpp`：`1819` 行
- `src/mainwindow_mount_control.cpp`：`707` 行

这些文件已经加入 `src/CMakeLists.txt`，并参与当前正常构建。

## 4. 各轮拆分内容

### 第一轮：命令分发瘦身

提交：

- `8aa5a06 Split mainwindow command dispatch`

结果：

- 将 `onMessageReceived()` 内的大量命令分发逻辑拆分到独立命令实现文件
- 新增：
  - `mainwindow_commands_capture.cpp`
  - `mainwindow_commands_control.cpp`
  - `mainwindow_commands_device.cpp`

意义：

- 把“消息入口”从“业务实现堆叠区”中剥离出来
- 为后续按功能域继续拆分提供稳定入口

### 第二轮：设备连接生命周期

提交：

- `fec517b Extract mainwindow device connection flow`

结果：

- 新增 `mainwindow_device_connection.cpp`
- 迁出连接与连接后初始化相关逻辑，如：
  - `ConnectAllDeviceOnce`
  - `continueConnectAllDeviceOnce`
  - `BindingDevice`
  - `UnBindingDevice`
  - `AfterDeviceConnect`
  - `ConnectDriver`
  - `DisconnectDevice`

### 第三轮：图像处理主链路

提交：

- `066b920 Extract mainwindow image pipeline`

结果：

- 新增 `mainwindow_image_pipeline.cpp`
- 迁出 FITS/JPG/PNG 处理、GPM、瓦片、前端图像输出相关链路

### 第四轮：导星主流程

提交：

- `14b6758 Extract mainwindow guiding flow`

结果：

- 新增 `mainwindow_guiding.cpp`
- 迁出内置导星循环与相关控制流程

### 第五轮：电调控制

提交：

- `2cd6cf9 Extract focuser and schedule flows`

结果的一部分：

- 新增 `mainwindow_focuser.cpp`
- 迁出电调持续移动、步进移动、停止、位置/范围/状态读取等逻辑

### 第六轮：ROI 对焦循环

结果并入后续阶段：

- 新增 `mainwindow_focus_loop.cpp`
- 迁出：
  - `FocusingLooping`
  - `focusLoopShooting`
  - `getFocuserLoopingState`

### 第七轮：自动对焦启动链路

结果并入后续阶段：

- 新增 `mainwindow_autofocus.cpp`
- 迁出：
  - `startAutoFocus`
  - `startAutoFocusFineHFROnly`
  - `startAutoFocusSuperFineOnly`
  - `startScheduleAutoFocus`
  - `cleanupAutoFocusConnections`

### 第八轮：计划任务

提交仍并入：

- `2cd6cf9 Extract focuser and schedule flows`

结果的另一部分：

- 新增 `mainwindow_schedule.cpp`
- 迁出计划任务表、流程推进、等待与拍摄调度相关逻辑

### 第九轮：存储与 USB

提交：

- `9ae8f47 Extract mainwindow storage and mount flows`

结果的一部分：

- 新增 `mainwindow_storage.cpp`
- 迁出：
  - 图像保存
  - 解算失败图像保存
  - USB 拷贝
  - 文件列表读取
  - 存储空间检查

### 第十轮：设备配置 / 串口 / 绑定恢复

提交：

- `9ae8f47 Extract mainwindow storage and mount flows`

结果的另一部分：

- 新增 `mainwindow_device_config.cpp`
- 迁出：
  - 驱动选择
  - 设备分组/设备确认
  - 已连接设备恢复
  - 串口枚举与 by-id 选择
  - SDK 驱动选择与绑定恢复

### 第十一轮：望远镜控制 / 解算同步

提交：

- `9ae8f47 Extract mainwindow storage and mount flows`

结果的另一部分：

- 新增 `mainwindow_mount_control.cpp`
- 迁出：
  - `TelescopeControl_*`
  - `MountGoto`
  - `MountOnlyGoto`
  - `solveCurrentPosition`
  - `LoopSolveImage`
  - 时间/地点同步

## 5. 一个重要结论：旧 PHD2 代码不是当前在线路径

在继续拆分过程中，曾尝试把 `mainwindow.cpp` 中一大段旧 PHD2 逻辑单独迁出。  
真实树莓派编译验证后确认：

- 该段代码被 `#if 0` 包裹
- 它不是当前参与编译的在线逻辑
- 它依赖大量文件级静态状态、共享内存常量和历史辅助函数

因此最终结论是：

- 这段旧 PHD2 代码不应作为“当前在线拆分成果”统计
- 后续如果要处理，应先明确是“删除历史死代码”还是“重新启用并重构”
- 本轮没有把该块保留为有效拆分文件

这个结论很重要，因为它避免了后续继续在一块历史死代码上投入结构拆分成本。

## 6. 验证方式

本轮验证采用“双路径”理解：

### 本机验证

执行：

```bash
cd /home/q/workspace_origin/QUARCS_QT-SeverProgram/src
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Debug
```

结果：

- CMake 生成正常
- `git diff --check` 通过

本机完整编译仍受既有环境依赖限制，不属于本次拆分引入的问题：

- 缺 `stellarsolver.h`
- 缺 `opencv2/imgcodecs.hpp`

### 树莓派真实验证

每轮都以这条命令作为主要验收方式：

```bash
cd /home/q/workspace_origin
./deploy_all_to_pi.sh --backend-only
```

验证内容包括：

- 源码同步
- 树莓派本机编译
- 替换 `src/BUILD/client`
- 重启后端
- 校验版本号和运行进程

本阶段最终有效收口版本号为：

- `202606292110`

并且已经确认：

- 树莓派编译成功
- `client` 成功部署并启动
- `All requested deploy steps completed successfully`

## 7. 当前收益

本轮拆分带来的主要收益有：

- `mainwindow.cpp` 体量显著下降
- 新功能不必再默认堆到 `mainwindow.cpp`
- 后续定位问题时，职责边界更清楚
- 编译错误更容易收敛到具体功能域文件
- 部署验证已经证明当前拆分方法在项目中可行

## 8. 当前仍然存在的问题

虽然主文件已经缩小很多，但还不能认为任务彻底结束。

目前仍存在的现实情况：

- `mainwindow_device_connection.cpp` 仍然很大，超过 `9000` 行
- `mainwindow.cpp` 自身仍有 `8752` 行，仍然偏大
- 某些 helper 仍然停留在 `mainwindow.cpp` 顶部匿名命名空间中
- 个别函数仍然存在历史 warning，例如非 `void` 函数末尾缺省返回

也就是说，本阶段已经完成“从极端集中式单文件，进入可继续拆分状态”，但还没有达到理想的长期结构。

## 9. 后续建议

后续可以继续沿着下面方向推进：

### 优先级 1：继续拆 `mainwindow_device_connection.cpp`

建议按连接阶段再细分：

- 驱动启动
- 设备绑定
- 连接后初始化
- 断开清理

### 优先级 2：继续拆 `mainwindow.cpp` 剩余杂项

当前 `mainwindow.cpp` 里仍有不少可继续迁出的部分，例如：

- GPIO / 定时轮询
- 网络状态/AP-STA
- 若干系统工具辅助

### 优先级 3：清理历史辅助函数归属

把那些当前仍散落在 `mainwindow.cpp` 顶部匿名命名空间中的 helper，逐步迁到：

- `mainwindow_command_support.h`
- 各自领域 cpp 的匿名命名空间

### 优先级 4：处理死代码

对旧 PHD2 `#if 0` 大块逻辑做单独决策：

- 删除
- 保留文档化
- 或重启重构

不建议继续把它当作当前在线代码拆分对象。

## 10. 本阶段对应提交

本阶段相关提交按时间顺序如下：

- `8aa5a06 Split mainwindow command dispatch`
- `fec517b Extract mainwindow device connection flow`
- `066b920 Extract mainwindow image pipeline`
- `14b6758 Extract mainwindow guiding flow`
- `2cd6cf9 Extract focuser and schedule flows`
- `9ae8f47 Extract mainwindow storage and mount flows`

这些提交共同构成了本次 `MainWindow` 拆分阶段的主要里程碑。

## 11. 结论

这次工作已经把 `MainWindow` 从“一个超大单文件集中承载绝大多数后端职责”的状态，推进到了“多个职责域已拆出、可继续分治”的状态。

本阶段最重要的成果不是单纯减少了多少行，而是确认了三件事：

- 当前项目允许通过“低风险结构拆分”持续推进
- 每轮都可以通过树莓派原生部署做真实验收
- 后续继续拆分时，已经有了稳定的方法、节奏和边界判断标准

如果后面继续推进，建议优先以 `mainwindow_device_connection.cpp` 和 `mainwindow.cpp` 剩余杂项作为下一阶段目标。
