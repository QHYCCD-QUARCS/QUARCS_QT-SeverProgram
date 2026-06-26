# QHYCCD SDK 一键部署脚本说明

## 脚本位置

```
~/workspace_origin/deploy_sdk_to_pi.sh
```

## 功能

将 QHYCCD SDK（含 Demo 相机支持）从开发机同步到树莓派业务机，在树莓派上编译动态库，部署到 QUARCS 程序目录并重启客户端。

## 使用方式

```bash
# 基本用法（使用默认参数）
./deploy_sdk_to_pi.sh

# 自定义参数
PI_HOST=192.168.1.100 ./deploy_sdk_to_pi.sh
CLEAN_REMOTE_BUILD=1 ./deploy_sdk_to_pi.sh
START_CLIENT=0 ./deploy_sdk_to_pi.sh
```

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `PI_HOST` | 172.24.217.51 | 树莓派 IP |
| `PI_USER` | quarcs | SSH 用户名 |
| `PI_PASS` | quarcs | SSH 密码 |
| `JOBS` | 2 | 编译并行数（树莓派建议 2） |
| `CLEAN_REMOTE_BUILD` | 0 | 1=清理旧构建缓存 |
| `START_CLIENT` | 1 | 1=部署后自动重启 QUARCS 客户端 |
| `SYNC_DELETE` | 0 | 1=rsync 时删除远程多余文件 |
| `BUILD_VERSION` | 时间戳 | 构建版本号 |

## 执行流程

1. **同步源码** — rsync 本地 SDK 源码到树莓派
2. **编译** — 在树莓派上 cmake + make qhyccd_shared（仅动态库）
3. **部署** — 复制 libqhyccd.so 到 QUARCS 程序目录和 /usr/local/lib/
4. **配置** — 更新 qhyccd.ini（含 Demo 相机配置）
5. **重启** — 重启 QUARCS websocketclient 客户端
6. **验证** — 检查库加载、符号、版本

## 注意事项

- **只编译动态库**：静态库编译在树莓派上容易因内存不足失败
- **make -j2**：树莓派内存有限，不要用 -j4
- **Demo 相机**：默认启用 2 台 Demo 相机，图片路径在 qhyccd.ini 的 [demo] 段配置
