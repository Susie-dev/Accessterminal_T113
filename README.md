# Accessterminal_T113 — 门口机

别墅门禁可视化对讲系统的**门口机（T113 终端）**。运行在全志 T113-S3
（ARM Cortex-A7）上，系统为 Tina Linux / OpenWrt。

与室内机仓库 [Accessterminal_desktop](../Accessterminal_desktop) 通过
TCP 信令 + UDP 音视频协同工作。

---

## 功能

| 功能 | 实现 |
|------|------|
| 呼叫室内机 | 触摸「呼叫」→ TCP 发 `call`，室内机接听后回 `accept` |
| 双向语音 | UDP 12345，裸 PCM（S16_LE / 16000Hz / 单声道），320 帧一包 |
| 视频推流 | UDP 23456，每包一帧完整 JPEG，不重组不重传 |
| 人脸识别 | OpenCV4：V4L2 采集 + Haar 检测 + LBPH 识别 |
| 指纹识别/注册 | 海林指纹模块，串口 `/dev/ttyS4` |
| 密码开门 | 触摸键盘，密码可配置 |
| 人体感应 | GPIO PE4（线号 132）轮询，有人时启动识别 |
| 补光/开锁 | GPIO PE5（线号 133），两者共用同一物理引脚 |
| 远程监看 | UDP 50000，室内机发 `getframe` 后才推帧 |

---

## 目录结构

```
src/
  main.cpp              程序入口
  config/               AppConfig：内置默认值 → JSON → 环境变量
  core/
    net/                protocol.h（协议唯一事实来源）、SignalingClient
    media/              音频收发、视频推流、远程监看服务
    vision/             V4L2 摄像头封装
  hal/                  libgpiod 封装、海林指纹驱动
  managers/             指纹 / 人脸 / 密码 / 提示音业务逻辑
  ui/                   主窗口与子窗口
  resources/            qrc、图片、提示音
config/appconfig.json   运行时配置
cmake/                  交叉编译工具链、FindALSA
deploy/                 部署脚本、init 脚本、udev 规则、第三方资源
tests/                  协议契约单元测试
tools/                  静态校验脚本
```

---

## 构建

### 交叉编译（目标板）

```sh
export T113_SYSROOT=/path/to/Tina-Linux/out/<board>/staging_dir/target
export T113_TOOLCHAIN_DIR=/path/to/toolchain/bin
export OPENCV_ROOT=/path/to/opencv_arm_build/_install
export GPIO_ROOT=/path/to/libgpiod/_install

./deploy/build-t113.sh
```

或直接调用 CMake：

```sh
cmake -S . -B build-t113 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-t113.cmake \
      -DT113_SYSROOT=$T113_SYSROOT \
      -DOPENCV_ROOT=$OPENCV_ROOT \
      -DGPIO_ROOT=$GPIO_ROOT \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-t113 -j$(nproc)
```

### 本机编译（仅用于语法检查）

需要本机安装 Qt5 + OpenCV + ALSA + libgpiod：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DACCESSTERMINAL_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

也保留了 qmake 工作流（Qt Creator 可直接打开 `smartpro.pro`）：

```sh
qmake T113_SYSROOT=$T113_SYSROOT OPENCV_ROOT=$OPENCV_ROOT GPIO_ROOT=$GPIO_ROOT
make -j$(nproc)
```

---

## 部署

```sh
scp build-t113/bin/smartpro              root@<T113_IP>:/tmp/
scp -r deploy/vendor/pcm                 root@<T113_IP>:/tmp/
scp config/appconfig.json                root@<T113_IP>:/tmp/
scp deploy/install.sh                    root@<T113_IP>:/tmp/
scp deploy/accessterminal.init           root@<T113_IP>:/tmp/
ssh root@<T113_IP> 'cd /tmp && ./install.sh'
```

`install.sh` 会安装到 `/opt/smartpro`、配置到 `/etc/accessterminal`、
提示音到 `/tmp/pcm`，并检查设备权限。

### 开机自启

```sh
cp deploy/accessterminal.init /etc/init.d/accessterminal
chmod 755 /etc/init.d/accessterminal
/etc/init.d/accessterminal start
```

### 设备权限

```sh
cp deploy/99-accessterminal.rules /etc/udev/rules.d/
udevadm control --reload-rules && udevadm trigger
```

---

## 配置

所有参数集中在 `config/appconfig.json`，优先级为
**内置默认值 → JSON 文件 → 环境变量**。

```json
{
  "network":  { "server_ip": "192.168.1.4", "self_ip": "192.168.1.8" },
  "hardware": { "camera_device": "/dev/video0",
                "fingerprint_port": "/dev/ttyS4",
                "audio_device": "hw:audiocodec",
                "door_gpio_line": 133 },
  "resources": { "face_cascade": "/root/haarcascade_frontalface_default.xml",
                 "pcm_dir": "/tmp/pcm" },
  "behavior":  { "frame_interval_ms": 50, "face_detect_interval": 10 },
  "security":  { "door_password": "5788" }
}
```

配置文件查找顺序：

1. 命令行第一个参数
2. 可执行文件同目录的 `appconfig.json`
3. 当前工作目录的 `config/appconfig.json`
4. `/etc/accessterminal/appconfig.json`

### 环境变量覆盖

兼容历史变量名：`SMARTPRO_SERVER_IP`、`SMARTPRO_SELF_IP`

统一前缀（推荐）：

| 变量 | 含义 |
|------|------|
| `ACCESSTERMINAL_NETWORK_SERVER_IP` | 室内机地址 |
| `ACCESSTERMINAL_NETWORK_SELF_IP` | 本机地址 |
| `ACCESSTERMINAL_HARDWARE_CAMERA_DEVICE` | 摄像头设备 |
| `ACCESSTERMINAL_HARDWARE_FINGERPRINT_PORT` | 指纹串口 |
| `ACCESSTERMINAL_HARDWARE_DOOR_GPIO_LINE` | 开锁/补光 GPIO 线号 |
| `ACCESSTERMINAL_SECURITY_DOOR_PASSWORD` | 门禁密码 |

---

## 通信协议

协议契约定义在 `src/core/net/protocol.h`，与室内机仓库**内容完全一致**
（SHA256 相同），由 `tests/test_protocol.cpp` 与
`tools/check_protocol_sync.py` 共同守护。

| 通道 | 端口 | 格式 |
|------|------|------|
| 信令 | TCP 8887 | UTF-8 文本行，`\n` 结束 |
| 音频 | UDP 12345 | 裸 PCM，S16_LE / 16000Hz / 单声道，320 帧（640 字节）一包 |
| 视频 | UDP 23456 | 每包一帧完整 JPEG，不重组不重传 |
| 远程监看 | UDP 50000 | 室内机发 `getframe` / `stop` |

信令一览：

| 信令 | 方向 | 含义 |
|------|------|------|
| `call` | 门口机 → 室内机 | 请求呼叫 |
| `accept` | 室内机 → 门口机 | 接通，**门口机据此才创建音视频线程** |
| `recstop` | 室内机 → 门口机 | 挂断 |
| `unlock` | 双向 | 远程开锁（门口机侧播放开门音） |
| `stopsend` | 门口机 → 室内机 | 对端停止发送 |
| `getframe` | 室内机 → 门口机 | 开始推帧 |
| `stop` | 室内机 → 门口机 | 停止推帧 |

> **重要**：音视频线程在收到 `accept` 之前**不会创建**，空闲时零资源占用。
> 修改代码时务必保留此约束。

---

## 开发约定

- **标准**：C++11（不可使用 C++17 的 inline 变量等特性）
- **Qt**：Qt5 Widgets；代码同时避免使用 Qt6 已移除的 API
- **格式**：遵循 `.clang-format`（4 空格缩进、100 列）

```sh
clang-format -i $(git ls-files '*.cpp' '*.h')
```

### 提交前自检

```sh
# 1. 静态一致性（include 解析、构建列表、qrc、.ui 控件引用、旧标识符）
python3 tools/verify.py .

# 2. 跨仓库协议一致性
python3 tools/check_protocol_sync.py
```

两个脚本在 CI 中也可直接使用，退出码非 0 即表示失败。

---

## 已知限制

- **交叉编译无法在开发机验证**：需要 Tina SDK 与 ARM Qt5/OpenCV 环境，
  请按上文在 Linux 主机上执行 `deploy/build-t113.sh`。
- `libgpiod` 在目标平台通常只提供静态库，需通过 `GPIO_ROOT` 指定。
- 人脸识别使用 LBPH，光照变化下准确率有限；建议配合补光灯使用。
- `zeng_sim.onnx` 为历史遗留模型，当前代码未使用（已在 `.gitignore` 中排除，
  仅保留在 `deploy/vendor/` 备查）。
