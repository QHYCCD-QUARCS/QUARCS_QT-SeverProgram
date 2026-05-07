## 离线测试（不需要相机/赤道仪）

这个测试程序用于验证你要求的三块关键能力：
- 选星（三遍扫描：SNR/HFD/饱和/边缘）
- 校准（RA/DEC 约 25px 位移，输出 cameraAngleDeg/orthoErrDeg/raMsPerPixel 等）
- 单向导星门控（允许方向集合不包含理论修正方向时，忽略本次修正）

### 1) 编译

在 `src` 目录下编译（你们工程 CMakeLists 在 `src/`）：

```bash
cd /home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src
mkdir -p build
cd build
cmake ..
make -j"$(nproc)"
```

### 2) 运行

```bash
cd /home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/build
./guiding_offline_test /tmp/guiding_offline
```

### 3) 输出说明

- 终端输出会包含：\n
  - `[Select] ...`：选星结果\n
  - `[Calib] ...`：校准结果（关键字段）\n
  - `[GuideGate] ...`：门控测试结果（默认配置下应 “gated”）\n
- FITS 测试帧会写入你传入的输出目录（默认 `/tmp/guiding_offline`）。

## 真实批次离线分析

当业务机已经采回 `~/images/GuiderDiagnostics/batch_*` 一组真实导星样本时，优先用这个工具做离线分析：

```bash
cd /home/q/workspace_origin/QUARCS_QT-SeverProgram/src
mkdir -p build
cd build
cmake ..
make -j"$(nproc)" guiding_batch_analyzer
./guiding_batch_analyzer /path/to/batch_xxx
```

默认会在该批次目录下生成 `analysis/`：

- `frame_XX_analysis.jpg`
  黄色圆：按原始 ADU 排序的亮峰参考
  红框：当前算法通过筛选的候选星
  蓝框：当前算法拒绝的候选星
  绿框：最终主星
- `summary.json`
  每帧的主星、validated、rejected、brightPeaks 汇总


