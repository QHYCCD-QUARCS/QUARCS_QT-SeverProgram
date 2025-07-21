#!/bin/bash

# 强制使用X11显示服务器启动KStars
export QT_QPA_PLATFORM=xcb
export XDG_SESSION_TYPE=x11

# 清理可能的临时文件
rm -f /tmp/indififo*

# 启动KStars
echo "正在使用X11启动KStars..."
kstars "$@"