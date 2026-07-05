# 2026-07-01 QHYCCD DEMO 相机添加工作完整说明

## 1. 文档目的

这份文档用于完整说明当前 `QHYCCD SDK` 中 `DEMO` 相机功能的实现现状，方便：

- 后续开发人员理解这项工作的背景与范围
- 主线合并时快速定位关键源码改动
- 日常测试时正确修改 `qhyccd.ini`
- 排查“为什么看不到 DEMO 相机”“为什么改了配置不生效”等问题

本文重点覆盖：

- DEMO 相机功能是如何接入 SDK 的
- 关键源码文件在哪
- `qhyccd.ini` 应该怎么改
- 运行时到底读取哪一份 `qhyccd.ini`
- 一键部署脚本会不会覆盖配置

## 2. 当前代码基线

当前开发机实际使用的 DEMO 相机主修改目录是：

```bash
/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform
```

说明：

- 当前 `deploy_sdk_to_pi.sh` 也是从 `workspace_origin` 这份 SDK 取源码并同步到树莓派
- `workspace_origin/QHYCCD_SDK_CrossPlatform` 中有一部分顶层辅助目录通过软链接复用：

```bash
/home/q/workspace/QHYCCD_SDK_CrossPlatform
```

- 但 DEMO 相机相关的核心改动主要在 `workspace_origin` 自己独立维护的 `src/` 下

因此，涉及 DEMO 相机逻辑时，默认应以：

```bash
/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src
```

为准。

## 3. 功能目标

DEMO 相机的目标是让 SDK 在没有真实 QHY 相机硬件时，也能注册出一个或多个“虚拟相机”，并从磁盘中的 FITS 文件序列读取图像，走尽量接近真实相机的 SDK 数据路径。

当前设计目标可以概括为：

- 通过 `qhyccd.ini` 开关 DEMO 相机
- 支持注册多台 DEMO 相机
- 每台 DEMO 相机都有独立实例 ID
- 从指定目录读取 `frame_01.fits`、`frame_02.fits` 等 FITS 文件
- 让上层程序尽量像使用真实相机一样使用 DEMO 相机

当前 DEMO 相机注册后的设备名形如：

- `QHY_DEMO_0`
- `QHY_DEMO_1`

## 4. 关键源码文件

### 4.1 DEMO 相机类本体

主要文件：

- [qhydemo.h](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhydemo.h)
- [qhydemo.cpp](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhydemo.cpp)

职责：

- 定义 `QHYDEMO` 类
- 保存 `image_dir`、实例编号、当前帧索引
- 读取 FITS 文件并转成 SDK 处理链需要的原始图像数据
- 提供 `ConnectCamera`、`DisconnectCamera`、`GetSingleFrame` 等虚拟实现
- 返回芯片尺寸、位深、binning、曝光等“可用但无硬件依赖”的能力接口

### 4.2 SDK 扫描与注册接入点

主要文件：

- [qhyccd.cpp](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhyccd.cpp)

关键逻辑位置：

- `ScanQHYCCDInside()` 中新增 DEMO 注册块
- `InitQHYCCDResource()` 中读取 `qhyccd.ini`
- `OpenQHYCCD()`、`InitQHYCCDClass()`、部分 capability 查询逻辑对 DEMO 做了兼容

### 4.3 设备类型定义

主要文件：

- [qhyccdcamdef.h](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhyccdcamdef.h)

当前已经定义：

```c
#define DEVICETYPE_QHY_DEMO 9999
```

### 4.4 调试/验证辅助文件

相关文件：

- [CrashTest2.cpp](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/CrashTest2.cpp)
- [CrashTest3.cpp](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/CrashTest3.cpp)
- [qhydemo.cpp.bak](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhydemo.cpp.bak)
- [qhydemo.h.bak](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhydemo.h.bak)

说明：

- `CrashTest*` 主要用于本地或树莓派上验证 DEMO 相机扫描/打开流程
- `.bak` 文件是历史备份，不参与正式构建，但能帮助回顾 DEMO 逻辑演进

## 5. 注册流程说明

当前 DEMO 相机注册发生在：

- `ScanQHYCCDInside()`

相关代码入口见：

- [qhyccd.cpp](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhyccd.cpp:17147)

总体流程如下：

1. 先读取当前工作目录下的 `qhyccd.ini`
2. 读取 `[demo]` 段：
   - `enabled`
   - `count`
   - `image_dir`
3. 若满足启用条件，则按 `count` 循环注册 DEMO 实例
4. 每个实例生成：
   - 设备 ID：`QHY_DEMO_<n>`
   - 虚拟路径：`DEMO:<image_dir>#<n>`
5. 为每个实例创建 `QHYDEMO`
6. 提前调用 `InitDemoCamera()` 读取首帧 FITS 头，确定图像尺寸
7. 放入 `cydev[]` 设备槽位，作为 SDK 扫描结果的一部分返回

如果此前已经注册过 DEMO，相同 ID 的设备会尽量复用，并根据新配置更新实例信息。

同时 SDK 还加入了“清理已关闭 DEMO 槽位”的逻辑，避免切换 `count` 或关闭 DEMO 后残留旧虚拟设备。

## 6. DEMO 图像读取方式

当前 DEMO 相机不是读取 JPG/PNG，而是读取 FITS。

约定输入目录中应至少有：

```bash
frame_01.fits
```

通常建议准备：

```bash
frame_01.fits
frame_02.fits
frame_03.fits
...
```

当前实现特点：

- 会先读 `frame_01.fits` 头部获得 `NAXIS1/NAXIS2`
- 使用 FITS 头中的图像尺寸初始化 DEMO 相机几何参数
- 单帧读取时再从当前帧文件取出 16-bit 图像数据
- 图像会尽量走现有 SDK 通用数据链路，而不是单独发明一套完全不同的输出格式

如果 `image_dir` 不存在，或其中没有可用 FITS，`InitDemoCamera()` 会失败，进而导致该 DEMO 实例无法注册成功。

## 7. qhyccd.ini 配置说明

### 7.1 DEMO 相机配置段

当前 DEMO 相机由 `qhyccd.ini` 中的 `[demo]` 段控制：

```ini
[demo]
enabled=true
count=2
image_dir=/home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/
```

字段说明：

### `enabled`

- `true`：启用 DEMO 相机逻辑
- `false`：关闭 DEMO 相机逻辑

### `count`

- 要注册的 DEMO 相机数量
- `count=2` 时会注册：
  - `QHY_DEMO_0`
  - `QHY_DEMO_1`

### `image_dir`

- FITS 图像目录
- 当前实现要求该目录下存在类似：
  - `frame_01.fits`
  - `frame_02.fits`
  - `frame_03.fits`

## 8. DEMO 生效条件

当前逻辑下，DEMO 相机真正注册成功的条件可以概括为：

```text
enabled == true
AND count > 0
AND image_dir 非空
AND image_dir 下存在可正常读取的 FITS 数据
```

如果只满足前 3 条，但 `frame_01.fits` 无法打开或头部不合法，那么 `InitDemoCamera()` 仍会失败。

## 9. qhyccd.ini 应该改哪一份

这是最容易混淆的地方。

### 9.1 树莓派线上运行时真正生效的配置

当前 QUARCS 业务程序运行目录是：

```bash
/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD
```

因此，当前线上实际生效的配置文件是：

```bash
/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD/qhyccd.ini
```

原因是 SDK 在运行时按当前工作目录直接读取：

```ini
qhyccd.ini
```

也就是没有写死绝对路径，而是读取运行目录下那一份。

### 9.2 SDK 源码目录里的模板配置

树莓派或开发机 SDK 源码目录里还有一份：

```bash
/home/quarcs/QHYCCD_SDK_CrossPlatform/src/qhyccd.ini
```

或开发机上的：

```bash
/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhyccd.ini
```

这类更接近“源码模板”或安装模板，不一定是当前业务进程正在读取的那份。

### 9.3 Qt 工程自带的 qhyccd.ini

Qt 服务端仓库里还有一份：

```bash
/home/q/workspace_origin/QUARCS_QT-SeverProgram/qhyccd.ini
```

它会在 Qt 构建/部署链路中被复制到运行目录，但最终是否生效仍取决于程序启动时所在目录。

## 10. qhyccd.ini 如何修改

### 10.1 临时在业务机上改

如果只是想快速测试 DEMO，相对推荐直接改业务机当前运行目录里的：

```bash
/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD/qhyccd.ini
```

例如启用 2 台 DEMO：

```ini
[demo]
enabled=true
count=2
image_dir=/home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/
```

例如关闭 DEMO：

```ini
[demo]
enabled=false
count=2
image_dir=/home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/
```

例如保留开关但逻辑关闭：

```ini
[demo]
enabled=true
count=0
image_dir=/home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/
```

### 10.2 想让一键部署后自动带上固定 DEMO 配置

则应修改：

- [deploy_sdk_to_pi.sh](/home/q/workspace_origin/deploy_sdk_to_pi.sh:255)

当前脚本会在部署时直接重写树莓派上的运行配置：

```bash
/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD/qhyccd.ini
```

当前默认写入的是：

```ini
[demo]
enabled=true
count=2
image_dir=/home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/
```

因此：

- 如果你在业务机上手工改了 `qhyccd.ini`
- 之后又执行了 `deploy_sdk_to_pi.sh`

那么这份手工修改很可能会被部署脚本覆盖。

## 11. 修改配置后如何生效

修改 `qhyccd.ini` 后，通常需要重启当前加载 SDK 的业务进程。

当前 QUARCS 环境下，一般对应：

- `websocketclient -n`
- 或其拉起的 `client`

如果只改了 `qhyccd.ini`，但没有重启进程，很可能仍看到旧扫描结果。

## 12. 常见验证方式

### 12.1 看是否扫描到 DEMO 设备

预期至少能在设备列表或 SDK 扫描结果中看到：

- `QHY_DEMO_0`
- `QHY_DEMO_1`

### 12.2 看数量是否符合预期

例如：

- 没接任何物理相机，`count=2`
  - 扫描结果通常应为 `2`
- 已接 2 台物理相机，`count=2`
  - 扫描结果通常应为 `4`

### 12.3 看日志

当前 `qhyccd.cpp` 中仍保留了一些 DEMO 调试输出，例如：

- `Entering demo camera registration block`
- `demo_enabled=... demo_count=... image_dir=...`
- `Demo camera ... registered at index ...`

如果开启对应日志输出，可据此判断：

- `qhyccd.ini` 是否被正确读取
- `enabled/count/image_dir` 是否符合预期
- `InitDemoCamera()` 是否成功

## 13. 当前已知实现特点

### 13.1 DEMO 相机是“虚拟设备”

它不会访问真实 USB 相机硬件，而是把 FITS 文件模拟成相机输出。

### 13.2 当前实现依赖 FITS 文件格式正确

至少首帧 `frame_01.fits` 必须能成功读取头信息，否则 DEMO 初始化失败。

### 13.3 当前实现已支持多实例

设备 ID 使用：

- `QHY_DEMO_0`
- `QHY_DEMO_1`
- ...

而不是旧备份版本里那种单一的 `QHY_DEMO` 命名。

### 13.4 当前实现已加入 DEMO 槽位清理

这有助于处理：

- `count` 变小
- `enabled` 从 `true` 改成 `false`
- 切换 `image_dir`

时旧 DEMO 虚拟设备残留的问题。

## 14. 与主线合并时建议重点关注的文件

如果后续要把这项工作合并回主线，建议至少重点检查：

- [qhydemo.h](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhydemo.h)
- [qhydemo.cpp](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhydemo.cpp)
- [qhyccd.cpp](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhyccd.cpp)
- [qhyccdcamdef.h](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhyccdcamdef.h)
- [deploy_sdk_to_pi.sh](/home/q/workspace_origin/deploy_sdk_to_pi.sh)
- [README_QHYCCD_DEMO_CAMERA.md](/home/q/workspace_origin/QUARCS_QT-SeverProgram/doc/README_QHYCCD_DEMO_CAMERA.md)

重点核对：

- DEMO 注册是否发生在硬件扫描之前
- 多实例命名是否符合预期
- FITS 读取路径和格式假设是否要进一步抽象
- `qhyccd.ini` 的覆盖方式是否要参数化
- 调试 `printf` 是否需要在主线保留或改为更规范的日志输出

## 15. 建议的日常维护规则

建议后续统一按下面规则理解：

- DEMO 功能主代码以 `workspace_origin` 为准
- 临时验证时改业务机运行目录中的 `qhyccd.ini`
- 想固化默认配置时改 `deploy_sdk_to_pi.sh`
- 需要交接给其它开发者时，优先同时提供：
  - SDK 源码包
  - 本文档
  - 当前实际使用的 `qhyccd.ini` 示例

## 16. 相关文件入口

- DEMO 相机主类：
  - [qhydemo.h](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhydemo.h)
  - [qhydemo.cpp](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhydemo.cpp)
- SDK 扫描接入：
  - [qhyccd.cpp](/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhyccd.cpp)
- SDK 部署脚本：
  - [deploy_sdk_to_pi.sh](/home/q/workspace_origin/deploy_sdk_to_pi.sh)
- 简版配置说明：
  - [README_QHYCCD_DEMO_CAMERA.md](/home/q/workspace_origin/QUARCS_QT-SeverProgram/doc/README_QHYCCD_DEMO_CAMERA.md)
