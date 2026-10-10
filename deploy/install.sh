#!/bin/sh
#
# Accessterminal_T113 部署脚本（在 T113 目标板上以 root 运行）
#
# 用途：把编译产物安装到目标板并配置好运行环境。
#
# 用法（在目标板上）：
#   ./install.sh                    # 安装到默认路径
#   PREFIX=/opt/smartpro ./install.sh
#
# 从开发机推送的典型流程：
#   scp build-t113/bin/smartpro root@<T113_IP>:/tmp/
#   scp -r deploy/vendor/pcm root@<T113_IP>:/tmp/
#   scp deploy/install.sh root@<T113_IP>:/tmp/
#   ssh root@<T113_IP> 'cd /tmp && ./install.sh'
#
set -e

PREFIX="${PREFIX:-/opt/smartpro}"
BIN_DIR="$PREFIX/bin"
ETC_DIR="/etc/accessterminal"
SHARE_DIR="$PREFIX/share"
PCM_DIR="/tmp/pcm"

echo "=== Accessterminal T113 部署 ==="
echo "安装前缀: $PREFIX"

# ---- 1. 可执行文件 ----
if [ ! -f "./smartpro" ]; then
    echo "错误: 当前目录下找不到 smartpro，请先上传编译产物" >&2
    exit 1
fi
mkdir -p "$BIN_DIR"
cp -f ./smartpro "$BIN_DIR/smartpro"
chmod 755 "$BIN_DIR/smartpro"
echo "[1/5] 已安装可执行文件: $BIN_DIR/smartpro"

# ---- 2. 配置文件 ----
mkdir -p "$ETC_DIR"
if [ -f "./appconfig.json" ]; then
    if [ -f "$ETC_DIR/appconfig.json" ]; then
        # 保留现场修改，避免升级覆盖用户的 IP 与密码配置
        cp -f "$ETC_DIR/appconfig.json" "$ETC_DIR/appconfig.json.bak"
        echo "      已备份原有配置到 appconfig.json.bak"
    fi
    cp -f ./appconfig.json "$ETC_DIR/appconfig.json"
    echo "[2/5] 已安装配置: $ETC_DIR/appconfig.json"
else
    echo "[2/5] 未提供 appconfig.json，将使用程序内置默认值"
fi

# ---- 3. 人脸分类器 ----
mkdir -p "$SHARE_DIR"
if [ -f "./haarcascade_frontalface_default.xml" ]; then
    cp -f ./haarcascade_frontalface_default.xml "$SHARE_DIR/"
    echo "[3/5] 已安装人脸分类器: $SHARE_DIR/haarcascade_frontalface_default.xml"
else
    echo "[3/5] 未提供人脸分类器，人脸识别功能不可用"
fi

# ---- 4. 提示音（放 /tmp/pcm，与程序默认配置一致）----
if [ -d "./pcm" ]; then
    mkdir -p "$PCM_DIR"
    cp -f ./pcm/*.pcm "$PCM_DIR/" 2>/dev/null || true
    echo "[4/5] 已安装提示音到 $PCM_DIR"
else
    echo "[4/5] 未提供 pcm 目录，程序将无语音提示"
fi

# ---- 5. 设备权限 ----
# 摄像头、串口、GPIO 需要相应权限；以 root 运行时通常已足够。
for dev in /dev/video0 /dev/ttyS4; do
    if [ -e "$dev" ]; then
        chmod 666 "$dev" 2>/dev/null || true
    else
        echo "      警告: 设备不存在 $dev（可在 appconfig.json 中改配置）"
    fi
done
echo "[5/5] 设备权限已检查"

# ---- 开机自启（若存在 init 脚本）----
if [ -f "./accessterminal.init" ]; then
    if [ -d /etc/init.d ]; then
        cp -f ./accessterminal.init /etc/init.d/accessterminal
        chmod 755 /etc/init.d/accessterminal
        echo "      已安装 SysV init 脚本: /etc/init.d/accessterminal"
    fi
fi

echo ""
echo "=== 部署完成 ==="
echo "运行: $BIN_DIR/smartpro"
echo "配置: $ETC_DIR/appconfig.json"
echo "日志: 直接查看前台输出，或用 accessterminal.init 启动后看 syslog"
