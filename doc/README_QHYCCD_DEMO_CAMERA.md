# QHYCCD DEMO 相机配置说明

## 目的

这份文档用于说明：

- 树莓派业务机上 `qhyccd.ini` 的实际位置
- DEMO 相机的启用/关闭方式
- DEMO 相机配置项含义
- 修改配置后的生效方式
- 与部署脚本相关的注意事项

## 结论先说

当前 SDK 对 DEMO 相机的启用与关闭，确实是通过 `qhyccd.ini` 的 `[demo]` 段控制。

SDK 在运行时直接按当前工作目录读取：

```ini
qhyccd.ini
```

相关代码位置：

- `InitQHYCCDResource()`：`QHYCCD_SDK_CrossPlatform/src/qhyccd.cpp`
- `ScanQHYCCDInside()`：`QHYCCD_SDK_CrossPlatform/src/qhyccd.cpp`

核心逻辑为：

- `INIReader reader("qhyccd.ini");`
- 读取 `[demo]` 段中的 `enabled`、`count`、`image_dir`

## 树莓派业务机上的实际文件位置

### 1. 当前线上实际生效的配置文件

树莓派业务机当前实际生效的配置文件位置是：

```bash
/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD/qhyccd.ini
```

这是因为 QUARCS 业务程序当前从 `src/BUILD` 目录运行，SDK 会在该运行目录下直接查找 `qhyccd.ini`。

### 2. SDK 源码目录中的模板文件

树莓派上另外还有一份 SDK 源码目录中的模板文件：

```bash
/home/quarcs/QHYCCD_SDK_CrossPlatform/src/qhyccd.ini
```

这份更接近“源码自带默认模板”，通常不作为当前线上 QUARCS 运行时的主配置入口。

## DEMO 相机配置项说明

当前使用的配置段格式如下：

```ini
[demo]
enabled=true
count=2
image_dir=/home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/
```

各字段含义如下。

### `enabled`

- `true`：启用 DEMO 相机支持
- `false`：关闭 DEMO 相机支持

只有当 `enabled=true` 时，SDK 才会继续检查其它 DEMO 配置项。

### `count`

- 表示要注册的 DEMO 相机数量
- 例如 `count=2` 时，SDK 会尝试注册：
  - `QHY_DEMO_0`
  - `QHY_DEMO_1`

如果 `count<=0`，即使 `enabled=true`，也不会真正注册 DEMO 相机。

### `image_dir`

- DEMO 图像目录
- 目录中应提供 FITS 文件，例如：
  - `frame_01.fits`
  - `frame_02.fits`
  - `frame_03.fits`

当前 DEMO 相机实现会把这些 FITS 文件当作全图输入，并按 SDK 单帧处理链生成输出图像。

如果 `image_dir` 为空，或者目录中没有可用的 FITS 文件，则 DEMO 相机无法正常注册或取图。

## 配置生效条件

SDK 当前的 DEMO 注册条件可以概括为：

```text
enabled == true
AND count > 0
AND image_dir 非空
```

只有同时满足这三个条件，DEMO 相机才会被加入扫描结果。

## 常见配置示例

### 1. 完全关闭 DEMO 相机

```ini
[demo]
enabled=false
count=2
image_dir=/home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/
```

说明：

- `enabled=false` 即可关闭 DEMO 支持
- 此时 `count` 和 `image_dir` 即使保留，也不会注册 DEMO 相机

### 2. 逻辑上关闭 DEMO 相机

```ini
[demo]
enabled=true
count=0
image_dir=/home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/
```

说明：

- 虽然 `enabled=true`
- 但因为 `count=0`，最终也不会注册 DEMO 相机

### 3. 启用 2 台 DEMO 相机

```ini
[demo]
enabled=true
count=2
image_dir=/home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/
```

说明：

- 会注册 `QHY_DEMO_0` 和 `QHY_DEMO_1`

### 4. 切换到新的 DEMO FITS 数据目录

```ini
[demo]
enabled=true
count=2
image_dir=/home/quarcs/images/your_demo_frames/
```

说明：

- 只要新的目录下存在可用的 `frame_XX.fits` 文件，DEMO 相机就会从这里读取数据

## 扫描数量的理解

当前扫描总数遵循下面的规则：

- 开发机没有接物理相机时：
  - 扫描数量 = `DEMO count`
- 树莓派业务机接了 2 台物理相机时：
  - 扫描数量 = `2 + DEMO count`

也就是说，DEMO 相机数量是在物理相机扫描结果基础上叠加进去的。

## 修改后如何生效

通常在修改完下面这份文件后：

```bash
/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD/qhyccd.ini
```

需要让当前使用 SDK 的业务进程重新启动，修改才会稳定生效。

当前 QUARCS 环境下，通常需要重启：

- `websocketclient -n`
或
- 当前直接加载 QHY SDK 的相关业务程序

## 与部署脚本相关的注意事项

### 1. `deploy_sdk_to_pi.sh` 会覆盖这份配置

当前脚本：

```bash
/home/q/workspace_origin/deploy_sdk_to_pi.sh
```

在部署过程中会直接重写树莓派上的：

```bash
/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD/qhyccd.ini
```

也就是说，如果你手工改了业务机上的 DEMO 配置，之后又执行了该脚本，那么这份手工修改有可能被脚本覆盖。

### 2. 当前脚本写入的是固定 DEMO 配置

当前默认写入内容为：

```ini
[demo]
enabled=true
count=2
image_dir=/home/quarcs/images/GuiderDiagnostics/batch_20260505_235711_438_31e76836/
```

如果后续希望让部署脚本支持不同 DEMO 目录或不同数量，建议把这些值进一步参数化，而不是每次部署后手工再改。

## 建议的日常操作方式

### 临时测试 DEMO 相机

建议直接修改：

```bash
/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD/qhyccd.ini
```

修改后重启业务进程进行验证。

### 正式固化到部署流程

如果希望每次 SDK 一键部署后都自动带上同一套 DEMO 配置，建议修改：

```bash
/home/q/workspace_origin/deploy_sdk_to_pi.sh
```

中写入 `qhyccd.ini` 的那一段默认内容。

## 相关文件

- 线上运行配置：
  - `/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD/qhyccd.ini`
- SDK 源码模板：
  - `/home/quarcs/QHYCCD_SDK_CrossPlatform/src/qhyccd.ini`
- SDK 部署脚本：
  - `/home/q/workspace_origin/deploy_sdk_to_pi.sh`
- SDK DEMO 注册逻辑：
  - `/home/q/workspace_origin/QHYCCD_SDK_CrossPlatform/src/qhyccd.cpp`
