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

### 主机端编译验证（无 ALSA/libgpiod 的开发机可用）

ALSA 与 libgpiod 是 Linux 专有接口，Windows/macOS 开发机上装不了；但没有
它们并不意味着无法编译检查。`tools/devstubs/` 下提供了按官方签名复刻的
桩头文件，可对 `src/` 下**全部** `.cpp` 做真实的语法与类型检查：

```sh
bash tools/check_host_compile.sh
```

它会：

1. 用 Qt 的 `uic` 预生成 `ui_*.h`（CMake 构建时才生成）；
2. 用 `tools/devstubs/` 的桩替代 ALSA / libgpiod / POSIX / V4L2 头文件；
3. 对每个 `.cpp` 执行 `g++ -fsyntax-only`。

**能发现**：类型不匹配、参数个数/类型错误、拼写错误、缺失头文件、
C++ 标准越界用法。

**不能发现**：链接错误、目标平台 ABI/宏差异、运行时行为。

> 因此本步骤**不能替代** Tina SDK 上的真实交叉编译，它只是把绝大多数
> 低级错误提前到开发机上暴露。桩头文件不是实现，切勿用来产出可执行文件。

Windows 上的工具链（MSYS2 + MinGW-w64）：

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-qt5-base \
          mingw-w64-x86_64-qt5-tools mingw-w64-x86_64-opencv
```

已验证：全部 18 个 `.cpp` 通过（GCC 16.2.0 + Qt 5.15.19）。

### 主机端链接检查（进一步验证符号完整性）

在语法检查之上，还可以用 `tools/devstubs/stub_impl.c` 提供的 ALSA /
libgpiod / POSIX 桩实现把整个工程**真的链接**起来：

```sh
bash tools/check_host_link.sh
```

它能发现 `-fsyntax-only` 覆盖不到的缺陷：

- `undefined reference`——声明了却没有定义；
- `multiple definition`——重复定义；
- 库顺序错误、漏链某个 Qt 模块。

> 产物**不可运行**（桩函数不做真实 I/O），仅用于验证链接完整性。
> 已验证：整个工程（18 个 .cpp + Qt MOC/UIC/RCC）链接成功。

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

### TCP 分包处理

TCP 是字节流，没有消息边界。一次 `readyRead` 可能拿到一条完整信令、
半条信令，或一次拿到多条（粘包）。两端统一使用
`protocol::StreamDecoder` 做跨包重组，它处理三种情况：

1. **粘包**：`"call\nunlock\n"` → 切出两条信令；
2. **半包**：`"unl"` 先到、`"ock\n"` 后到 → 缓冲后还原成 `unlock`；
3. **歧义前缀**：`"stop"` 是 `"stopsend"` 的前缀。若 TCP 恰好把
   `"stopsend\n"` 拆成 `"stop"` + `"send\n"`，**不能**凭 `"stop"`
   立即判定为 `Stop`——否则会触发错误行为（停掉监看推流，而不是
   通知对端停止发送）。解码器会继续等待以消歧。

> **兼容性注意**：历史版本的室内机发出的 `recstop` **没有** `'\n'`
> （见 baseline `desktop/smartserver/widget.cpp:566`）。旧版解析用
> `split('\n', SkipEmptyParts)`，会把末尾无终止符的片段也当作完整信令。
> `StreamDecoder` 刻意保留这一宽容语义：末尾片段若**本身已是完整信令**
> 且无歧义，则立即识别。否则“新版门口机 + 未升级的旧版室内机”组合
> 会永远收不到挂断指令。

---

## 生命周期约定（由自动化检查强制）

以下三条是本工程最容易在重构中被悄悄破坏、又最难靠运行测试发现的约定。
`tools/verify.py` 已把它们变成提交前即可拦截的结构化检查。

### 1. 音视频线程仅在收到 `accept` 后创建

`CameraPushThread` / `AudioSenderThread` / `AudioReceiverThread` 的
`new` 必须全部位于 `Widget::startCallSession()` 内，且该函数只能在
`Signal::Accept` 分支被调用。这是“待机零占用”的依据。

由 `check_accept_gated_threads` 强制。若有人把线程创建挪到构造函数
“预建”，功能上可能仍然正常，但空闲功耗与资源占用立刻退化。

### 2. 延时任务必须检查会话有效性

音频播放线程是**延迟 400ms** 创建的（错开声卡争用）。若这 400ms 内通话
被挂断，已排队的 lambda 会拿到已经被 `close()` 并置为 `-1` 的
`m_audioSocketFd`，线程进入 `run()` 后对 `fd = -1` 执行 `fcntl`/`FD_SET`，
属于未定义行为。

因此所有引用通话期资源（`m_audioSocketFd` / `m_audioSenderThread` /
`m_audioReceiverThread` / `m_cameraPushThread`）的延时任务，都必须检查
`m_callActive` 或 `m_callGeneration`。

用代次令牌而不只检查 `m_callActive` 的原因：若通话在 400ms 内挂断后
**立刻又接通新的一次**，`m_callActive` 已重新为 true，仅凭它无法区分
“上一代遗留的延时任务”，旧任务会误在新通话上再建一个播放线程。

由 `check_deferred_session_guard` 强制。

### 3. 异步任务不得在对象析构后访问它

`QtConcurrent::run` 起的任务若捕获裸 `this`，在对象析构后仍可能运行。
`AudioManager::~AudioManager()` 会先 `stop()` 再 `waitForFinished()`；
人脸检测任务改用 `QPointer` 观察 `this` 并在提交时按值拷贝分类器。

用 `.clang-format` 统一格式，但**格式不是正确性**——上述三条请以
`verify.py` 的结果为准。

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
# 1. 静态一致性（include 解析、构建列表、qrc、.ui 控件引用、旧标识符、
#    前置声明一致性、JSON 配置）
python3 tools/verify.py .

# 2. 签名一致性（.h 声明 vs .cpp 定义）
python3 tools/check_signatures.py .

# 2b. 指纹串口帧解析算法（把 header 里的算法照搬为 Python 独立执行，
#     含「原实现对照」与随机模糊测试；无需编译即可验证定帧逻辑）
python3 tools/verify_fingerprint_parser_algorithm.py

# 3. 跨仓库协议一致性
python3 tools/check_protocol_sync.py

# 4. 真实编译验证（本机类型检查；静态检查无法发现类型不匹配等问题）
bash tools/check_host_compile.sh

# 5. 链接完整性检查（发现 undefined reference / 重复定义）
bash tools/check_host_link.sh

# 6. 跨工程信令互通（需同级目录存在 Accessterminal_desktop）
bash tests/interop/run_interop.sh
```

以上脚本在 CI 中也可直接使用，退出码非 0 即表示失败。

第 1~3 步是静态检查，速度快但覆盖有限；**第 4、5 步不能省**——
像“前置声明了不存在的类名”“把 ALSA 枚举存成 int 再传回枚举形参”
这类缺陷只有编译器能发现，“漏定义某个符号”只有链接器能发现。

**第 6 步是协议兼容性的最终防线**：它把门口机的 `SignalingClient` 与
室内机的 `SignalingServer`（两个仓库里的**真实**实现）链接到一起，
在 127.0.0.1 上做真实 TCP 收发，逐条校验信令。端口、信令字符串、
`'\n'` 分包、粘包与半包处理，任何偏差都会在这里失败。

---

## 已知限制

- **交叉编译无法在开发机验证**：需要 Tina SDK 与 ARM Qt5/OpenCV 环境，
  请按上文在 Linux 主机上执行 `deploy/build-t113.sh`。
- `libgpiod` 在目标平台通常只提供静态库，需通过 `GPIO_ROOT` 指定。
- 人脸识别使用 LBPH，光照变化下准确率有限；建议配合补光灯使用。
- `zeng_sim.onnx` 为历史遗留模型，当前代码未使用（已在 `.gitignore` 中排除，
  仅保留在 `deploy/vendor/` 备查）。
