# 树莓派 aarch64：sysroot + 交叉编译

## 1. 同步树莓派根文件系统到本机

本机：

```bash
mkdir -p /home/quarcs/rpi-sysroot

# --info=progress2：单行整体进度条；--stats：结束时汇总（需 rsync 3.1+）
rsync -aHAX --numeric-ids --delete \
  --info=progress2 --stats \
  --rsync-path="sudo rsync" \
  --exclude=/dev/* \
  --exclude=/proc/* \
  --exclude=/sys/* \
  --exclude=/tmp/* \
  --exclude=/run/* \
  --exclude=/mnt/* \
  --exclude=/media/* \
  --exclude=/lost+found \
  raspberrypi:/ /home/quarcs/rpi-sysroot
```

**说明：** 树莓派上需先按项目 [README.md](README.md) 装好依赖（Qt5、OpenCV、INDI、StellarSolver、QHY SDK 等），再 rsync，否则 sysroot 里缺少头文件和 `.so`。

## 2. 宿主机安装工具链与 Qt 宿主工具

在 **x86_64 Ubuntu** 等宿主机上：

```bash
sudo apt update
sudo apt install -y \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  pkg-config cmake \
  qtbase5-dev qttools5-dev-tools
```

`moc` / `uic` / `rcc` 必须与宿主机 CPU 架构一致（在 Pi 上跑的 ARM 版不能在本机执行）。

## 3. 配置与编译

在仓库根目录：

```bash
cmake -S src -B build-rpi \
  -DCMAKE_TOOLCHAIN_FILE=../toolchain-rpi-arm64.cmake

cmake --build build-rpi -j$(nproc)
```

### 在 `src/BUILD` 目录下配置与编译（构建目录为当前文件夹）

若已使用或习惯在源码下的 `BUILD` 目录工作：

```bash
cd ~/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD

cmake -S .. -B . -DCMAKE_TOOLCHAIN_FILE=../../toolchain-rpi-arm64.cmake
cmake --build . -j$(nproc)
```

- **`-S ..`**：CMake 源码目录为上一级 `src`（含 `CMakeLists.txt`）。
- **`-B .`**：在当前目录 `BUILD` 内生成 Makefile / Ninja 与缓存；产物如 `client` 也在本目录。
- **`../../toolchain-rpi-arm64.cmake`**：从 `src/BUILD` 指向仓库根目录下的工具链文件。

若需指定非默认 sysroot，可在第一行 `cmake` 末尾追加：  
`-DCMAKE_SYSROOT=/你的/sysroot`。

自定义 sysroot 路径：

```bash
cmake -S src -B build-rpi \
  -DCMAKE_TOOLCHAIN_FILE=../toolchain-rpi-arm64.cmake \
  -DCMAKE_SYSROOT=/你的/sysroot
```

## 4. 常见问题

| 现象 | 处理 |
|------|------|
| 找不到 Qt5 | Pi 上安装 `qtbase5-dev` 等后再 rsync；或 `-DCMAKE_PREFIX_PATH=` 指向 sysroot 内 `.../lib/aarch64-linux-gnu/cmake` |
| 找不到 OpenCV / StellarSolver | 确保 Pi 上已安装对应 dev 包或 `/usr/local` 安装后再同步 |
| `moc` 报错 / `aarch64-binfmt` 执行 moc | `find_package(Qt5)` 会指向 sysroot 里的 ARM 版 `moc`；工程已在 `CMAKE_SYSROOT` 下把 `Qt5::moc/uic/rcc` 指回宿主机，并需安装 `qtbase5-dev` |
| 链接缺 `libqhyccd` 等 | 在 Pi 上安装 SDK 到 sysroot 可见路径后重新 rsync |
| `libopencv_core` 缺 `sgesdd_` / `liblapack.so.3` not found | `alternatives` 的符号链接在交叉链接时可能解析异常；工程已额外链接 `.../usr/lib/aarch64-linux-gnu/atlas/lib{blas,lapack}.so.3` |
| `undefined reference to cv::String` / `imwrite` 等 | 若 Pi 的 `/usr/local/include` 里有**旧版 opencv2 源码树**，`-I/usr/local/include` 会盖住系统的 `opencv4` 头；工程在 sysroot 模式下用 `-idirafter` 包含 `/usr/local/include`，使 QHY 等仍可用且 OpenCV 与 apt 库一致 |
| `CV_BGR2GRAY` 等未声明 | OpenCV 4 需在用到旧宏的翻译单元中包含 `opencv2/imgproc/types_c.h`（工程已在 `tools.h` 中加入） |

## 5. Ubuntu 上编译 → 树莓派上运行

- 产物为 **aarch64** 可执行文件（可用 `file build-rpi/client` 确认），**不要**在 x86_64 Ubuntu 上直接运行。
- 将 `client`（及 `qhyccd.ini` 等配置）拷到树莓派后执行；运行库版本应与做 sysroot 时 Pi 上环境一致（或同步后重新 rsync）。

## 6. 与原生编译对比

不交叉编译时，直接在树莓派 `src/build` 里 `cmake .. && make` 即可，无需本机 sysroot。
