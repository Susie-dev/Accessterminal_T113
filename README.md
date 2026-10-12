# Accessterminal_T113 — 门口机

别墅门禁可视化对讲系统的**门口机（T113 终端）**。运行在全志 T113-S3
（ARM Cortex-A7）上，系统为 Tina Linux / OpenWrt。

与室内机仓库 [Accessterminal_desktop](../Accessterminal_desktop) 通过
TCP 信令 + UDP 音视频协同工作。

> **准备在另一台机器上继续开发？先读
> [docs/交接与继续开发.md](docs/交接与继续开发.md)。**
> 那份文档只讲三件事：做到哪一步了、哪些没做完、下一步怎么做 ——
> 包括"本机已证明什么 / 尚未证明什么"的完整清单、7 类待办的优先级、
> 以及本项目的硬约束（协议逐字节兼容、C++11、冻结文件哈希）。
> 本文（README）是**参考手册**，那份是**交接说明**，两者互补。

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

### 单元测试（无 ALSA 的开发机也能跑）

主工程的根 `CMakeLists.txt` 需要 ALSA，而 ALSA 在 Windows/macOS 上装不了，
因此上面那条命令在开发机上通常走不通。单元测试只依赖 `Qt5::Core`/`Qt5::Test`，
**独立构建**即可：

```sh
cmake -S tests -B /tmp/t113-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/t113-tests
ctest --test-dir /tmp/t113-tests --output-on-failure
```

当前 20 个目标：`t113_protocol`（协议编解码）、`t113_fingerprint_parser`
（指纹帧解析）、`t113_app_config`（配置读取与夹取）、`t113_audio_manager`
（提示音播放，用假 ALSA 注入设备错误）、`t113_audio_sender`（UDP 12345
上行音频）、`t113_audio_receiver`（UDP 12345 下行播放）、`t113_video_push`
（UDP 23456 视频推流）、`t113_camera_preview`（UDP 50000 远程监看）、
`t113_fingerprint_manager`（指纹串口互斥调度，用**链接期替身**）、
`t113_signaling_client`（TCP 8887 发送失败与断线重连）、
`t113_fingerprint_wire`（指纹报文构造，链接**真实**实现）、
`t113_password_manager`（门禁密码校验不得进日志）、
`t113_gpio`（物理门锁/补光那条线，用可编程假 libgpiod）、
`t113_door_actuator`（**唯一的开锁出口**：真 `GpioLine` + 真 `DoorActuator` +
可编程假 libgpiod，断言"不假装开门 / 不阻塞 / 不重复驱动"三条硬约束）、
`t113_access_controller`（**唯一的放行入口**：静默期为 0 时不得抑制任何请求、
大于 0 时抑制且到期恢复、被抑制的请求不得触达执行器）、
`t113_face_manager`（人脸门禁，链接真实 LBPH；**缺 OpenCV face 模块时
自动跳过**）、`t113_v4l2_camera`（画面源头，用可编程假 syscall 层 +
`-Wl,--wrap`）、`t113_form_passwd`（密码输入窗体，用影子 `Widget`/
`AudioManager` 驱动真实按钮点击并捕获真实日志）、
`t113_form_call`（通话窗体，**stopsend 的发源地**：真 `QTcpServer` + 真
`SignalingClient`，点下停止按钮后断言对端收到 `"stopsend\n"`）、
`t113_form_finger`（指纹注册窗体，**现场真正执行**的分步注册状态机：真
`FingerprintManager` + 假串口设备，断言整条线上字节序列、串口互斥的成对
挂起/恢复，以及一条真实的 use-after-free 回归）。

注意最后两个目标的取向**相反**，这是刻意的：
`t113_fingerprint_manager` 用替身替换 `fingerprint_hailin.cpp`，
`t113_fingerprint_wire` 则链接真实实现。前者测流程调度，后者测报文字节。
同理 `t113_form_call` 对 `Widget` 用替身，但对 `SignalingClient` 用**真实
实现** —— 否则"点按钮发出 stopsend"就退化成"替身自己记了一笔"。

除前三个是纯逻辑外，其余都用**可注入的假替身**在真实接口边界上跑：
假 ALSA（设备错误按需注入）、假套接字（记录/投递数据报）、假 V4L2 相机
（链接期替身）。因此它们不碰任何真实硬件，却对**线上字节**做逐字节断言——
详见后面各节。

> 至此，**四类线上通路（TCP 8887 信令、UDP 12345 音频、UDP 23456 视频、
> UDP 50000 监看）全部有可执行的线上字节断言**，不再存在"看起来测过、
> 其实一步没跑"的通路。

跨工程信令互通测试同理，见 `tests/interop/run_interop.sh`（同样不依赖 ALSA）。
**注意（第 40 轮更正）**：该脚本目前是 **Windows/MSYS2 专用** —— 它把
`/mingw64/bin` 加进 PATH、并用 `cygpath` 换算路径，Linux 上直接跑不起来
（属交接文档 P8 的"夹具/脚本跨平台化"工作，尚未移植）。Linux 上要验跨工程
契约，现在可用的是 `python3 tools/check_protocol_sync.py`（两侧 protocol.h
逐字节一致 + 常量取值钉死）。

> **注意**：这两条路径此前实际上是坏的 —— `tests/CMakeLists.txt` 缺
> `cmake_minimum_required`/`project()`，配置阶段就直接失败；补上之后又会
> 因为缺 `enable_testing()` 而"构建成功、`ctest` 一个用例都不跑"。这类
> 失败**只看构建退出码会被误判为通过**，故已由 `check_subproject_cmake_is_selfcontained`
> 强制（自检见 `tools/verify_subcmake.py`，8/8 用例）。

### 指纹串口互斥（FingerprintManager）的行为断言

门口机上只有**一根指纹串口**，却有两个使用者：

- 常驻识别轮询（`startIdentify`，每 500 ms 下发 `CMD_GET_IMAGE`）；
- 注册流程（`startEnroll`，每 500 ms 走一步 `GET_IMAGE`/`GEN_CHAR`/…）。

两者都会 `readPacket()` 抢读**同一串口**的应答，互为对方之应答。
`setSuspended()` 就是为此而存在的互斥开关——而它此前**没有任何测试**。

测试方式：`HailinFingerprint` 的 `buildPacket/sendPacket/readPacket` 都不是
virtual，无法继承覆盖，因此用**链接期替身** `tests/fake_fingerprint_device.cpp`，
并**不链接** `src/hal/fingerprint_hailin.cpp`。替身里 `buildPacket()` 是
**真实算法的逐字节拷贝**——报文字节是唯一需要守住的契约，若这里也换成
"简化版"，测试就退化成"替身和自己比"。

> AUTOMOC 陷阱同前：本目标不含与 `fingerprint_hailin.h` 同名的源文件，
> 必须把该头文件显式列进 `add_executable`，否则链接期报
> `undefined reference to 'HailinFingerprint::identifyResult(...)'`。

| 用例 | 钉住的行为 |
| --- | --- |
| `identifyPollsWithGetImageCommand` | 轮询下发的帧结构（包头/地址/标识/大端长度/大端校验和）与 payload 恰为 `CMD_GET_IMAGE` |
| `identifyKeepsPolling` | 以 500 ms 周期**持续**下发，不是只发一次 |
| `identifySuccessEmitsResultAndStopsPolling` | 确认码 0x00 后停止轮询，并上报 `id`/`score` |
| `identifyKeepsPollingWhenDeviceSilent` | 模块不应答时继续轮询，**不误报成功** |
| `enrollSendsExactCommandSequence` | 注册的 8 帧命令序列、BufferID 递增 1/2/3、STORE_CHAR 的 id 大端、以及每步的超时参数 |
| `suspendedStopsPollingImmediately` | 挂起后**立刻**停止下发 |
| `identifyStaysSilentForWholeEnrollWindow` | 整个注册窗口内识别不得下发任何帧（跨 5 个轮询周期） |
| `resumeRestartsIdentify` | 恢复后常驻识别自动重新开始 |
| `startIdentify/startEnrollWhileSuspendedDoesNotAnnounce` | 挂起时连提示信号都不发（见下方说明） |
| `repeatedStartIdentify/startEnrollDoesNotStackTimers` | 重复调用不叠加定时器（否则下发频率翻倍） |
| `searchResponseByteLayoutIsPinned` | 应答布局 `[9]=confirm [10]=param [11..12]=id [13..14]=score` |
| `commandFrameByteLayoutIsPinned` | 下发帧的包头/地址/标识/长度/校验和逐字节 |
| `shortFramesDoNotCauseOutOfBoundsRead` | 11 字节"短但合法"的帧不得越界 |
| `commandTagFrameIsNotAValidAck` | 包标识 0x01 的帧被当成成功（记录为**已知偏差**） |
| `destructorStopsPolling` | 析构后整体安静（**不具区分度**，见下） |

#### 对照实验：两道守卫与四类分层的诚实结论

7 个对照项（6 个有区分度 + 1 个补测）全部符合预期。过程中发现并厘清了
**四类不同性质**的代码，这个分层本身就是本轮的主要产出：

**（一）真正有区分度、且发现了我自己测试缺陷的**

| 对照 | 结果 | 结论 |
| --- | --- | --- |
| 去掉 `startIdentify` 入口守卫 | 首版**测不出来** | 见（二） |
| STORE_CHAR 的 `id >> 8` 改成常量 0 | 首版**测不出来** | 我用了 `id=0x0005`，其高字节本就是 `0x00`，**断言没有区分度**。改用 `0x0105`（两字节都非零）后立即暴露 |
| BufferID 不递增 / 超时 500→999 | 测出来 | 命令序列断言有效 |

**（二）两道守卫互为遮蔽 —— 只测"不下发"是不够的**

`startIdentify`/`startEnroll` 各有**两道**挂起守卫：入口处的
`if (m_suspended) return;`，与轮询回调内的让路分支。我只断言"一帧都不发"时，
**去掉入口守卫也照样通过**：新定时器第一次触发时会被回调内的守卫拦住。
也就是说那道入口守卫当时是**未被验证**的。

它的唯一可观测效果是**提示信号**——`startIdentify()` 在守卫之后、建定时器
之前就 `emit identifyResult(..., "请按下指纹进行识别")`。补上
`*DoesNotAnnounce` 两条用例（断言挂起时**连提示都不发**）后，对照实验立刻
暴露了入口守卫的缺失。

**（三）防御纵深，无法独立观测**

- 轮询回调内的挂起让路：需要"`setSuspended` 到达时已有一个 timeout 事件
  排在队列里"这一竞态才能触发，无法确定性复现。
- `setSuspended(true)` 里的 `stopPoll()`：回调内的守卫会在 500 ms 内自行停表，
  线上不可观测。

两者都保留（防御纵深），但**不写"看似覆盖、实则空转"的断言**，而是明确记录。

**（四）被我误判为缺陷的"非缺陷"**

源码注释称 `~FingerprintManager()` 里的 `stopPoll()` 是为了防
"use-after-free"。对照实验证实：删掉它测试**照样通过**——定时器以 `this`
为 parent，随管理器一起被 QObject 析构销毁，销毁时连接断开，已排队的
timeout 不会再调用 lambda。**那句注释描述的 use-after-free 在当前结构下
无法发生**，该行属于防御性写法。已在测试里如实标注"不具区分度"。

### 产线代码覆盖率：第一次量化，以及"94.4%"为什么是误导

第 24 轮之前，本工程能说"9/9 通过"，但**从未量化过测试执行了多少产线代码**。
补上 gcov 度量后，第一个数字是 **94.4%** —— 看着很好。实际是 **25.0%**。

> **当前值（第 42 轮实测）：近似总覆盖率 54.8%**
> —— 已测文件 1774 行（覆盖 1467，82.7%）+ 未测文件约 903 行。
> 本节以下内容是**逐轮的历史记录**，用途是说明"分母"这件事是怎么被发现、
> 以及每次接进新文件时数字为什么有时**反而变难看**。**不要抄这里的数字当现状**：
> 引用覆盖率前务必重跑 `bash tools/run_coverage.sh`（本节末尾也有同样的告诫）。

原因在于**分母**：那个 94.4% 只统计了**被链接进测试目标的 17 个文件**。
另有 **18 个产线源文件**从未被任何测试目标链接（第 30 轮把
`v4l2_camera.cpp` 接了进来），压根不出现在
gcov 数据里：

| 从未被任何测试链接的产线文件 | 近似可执行行 |
| --- | --- |
| `src/ui/widget.cpp` | 678 |
| `src/hal/fingerprint_hailin.cpp` | 332 |
| `src/ui/form_finger.cpp` | 162 |
| `src/hal/gpio.cpp` | 105 |
| `src/managers/face_manager.cpp` | 52 |
| `src/ui/form_call.cpp` | 46 |
| `src/managers/password_manager.cpp` | 19 |
| 其余头文件与 `main.cpp` | — |

（上表是**第 24 轮首次量化时**的快照，保留以说明"分母"这件事是怎么被发现的。
此后 `form_passwd.cpp`（第 35 轮）、`v4l2_camera.cpp`（第 30 轮）、
`form_call.cpp`（第 36 轮）等已陆续接进测试目标，故**不再是当前状态**；
当前状态见下文"从未被任何测试目标链接的产线源文件（8 个）"。）

**数字正确、口径错误，比没有数字更危险** —— 它会让人停止寻找缺口。

`tools/run_coverage.sh` 一键产出：

```
已测文件 1196 行（覆盖 939）+ 未测文件约 1090 行 = 分母 2286
近似总覆盖率 = 41.1%   （上表那个 78.5% 仅描述已测文件）
```

（第 25 轮新增 `t113_test_fingerprint_wire` 后，把 332 行的
`fingerprint_hailin.cpp` 从"从未链接"移进了"已测"一栏，因此：分子从 671 升到
714，**已测文件的百分比反而从 94.9% 降到 73.7%**——因为刚接进来的那个文件
只覆盖了纯逻辑部分，硬件路径仍为空。分母从 2679 降到 2609，真实总覆盖率
从 25.0% 升到 **27.4%**。

第 30 轮把 `v4l2_camera.cpp`（63 行）接进 `t113_test_v4l2_camera` 后：
分子 714 -> 823，已测文件百分比 73.7% -> 77.0%，分母 2609 -> 2343，
真实总覆盖率 **27.4% -> 35.1%**。

第 35 轮把 `form_passwd.cpp`（46 行）接进 `t113_test_form_passwd` 后：
分子 823 -> 918，已测文件百分比 77.0% -> 78.2%，分母 2343 -> 2335（该文件
46 行从"未测"挪进"已测"，未测侧同时少了它原先的近似行数），
真实总覆盖率 **35.1% -> 39.3%**。该文件自身覆盖率 **97.8%**（46 行覆盖 45）。

第 36 轮把 `form_call.cpp`（25 行）接进 `t113_test_form_call` 后：
分子 918 -> 939，已测文件百分比 78.2% -> 78.5%，分母 2335 -> 2286，
真实总覆盖率 **39.3% -> 41.1%**。该文件自身覆盖率 **100.0%**（25 行全覆盖）——
它只有 25 个可执行行，却包含门口机**唯一**发出 `stopsend` 的那条接线。
"从未被任何测试目标链接的产线源文件"从 10 个降到 **8 个**。

这正是"分母必须一起报"的价值：只报已测文件百分比的话，**做对了事数字反而
变难看**，会诱导人不去补齐未测文件。）

#### 已测文件的明细与未覆盖行

被测到的文件覆盖度相当高（1196 行中 939 行，78.5%）：

| 文件 | 可执行行 | 覆盖 | 覆盖率 |
| --- | --- | --- | --- |
| `src/config/app_config.cpp` | 89 | 89 | 100% |
| `src/config/app_config.h` | 22 | 22 | 100% |
| `src/core/media/audio_sender_thread.cpp` | 51 | 51 | 100% |
| `src/core/net/signaling_client.cpp` | 61 | 61 | 100% |
| `src/core/vision/v4l2_camera.cpp` | 68 | 68 | 100% |
| `src/managers/face_manager.cpp` | 29 | 29 | 100% |
| `src/managers/password_manager.cpp` | 10 | 10 | 100% |
| **`src/ui/form_call.cpp`** | **25** | **25** | **100%** |
| `src/managers/audio_manager.cpp` | 69 | 68 | 98.6% |
| `src/ui/form_passwd.cpp` | 46 | 45 | 97.8% |
| `src/core/net/protocol.h` | 58 | 56 | 96.6% |
| `src/hal/fingerprint_frame_parser.h` | 50 | 48 | 96.0% |
| `src/core/media/audio_receiver_thread.cpp` | 93 | 89 | 95.7% |
| `src/core/media/video_push_thread.cpp` | 31 | 29 | 93.5% |
| `src/hal/gpio.cpp` | 50 | 45 | 90.0% |
| `src/managers/fingerprint_manager.cpp` | 128 | 115 | 89.8% |
| `src/core/media/camera_preview_server.cpp` | 52 | 45 | 86.5% |
| `src/hal/fingerprint_hailin.cpp` | 262 | 43 | 16.4% |

（另有 8 个单行/零行头文件，未列入本表；合计正好 1196 行 / 939 行。
本表是第 36 轮 `tools/run_coverage.sh` 的**实测值** —— 第 30 轮之后列过的数字
已经漂移（新接了文件、行号也随之变化），所以**每次引用覆盖率都要重新跑一遍**，
不要抄旧数字。）

> `fingerprint_hailin.cpp` 的 16.4% 是**可解释的**：262 行里有 195 行位于
> `autoIdentify`/`autoEnroll`/`cancel`/`stepRegister`/`stepIdentify` 这些
> **零调用者**的方法内（baseline 同样如此，属继承死代码），不是"该测没测"。

`coverage_report.py --json` 会给出每个文件的**未覆盖行号**，直接指出测试没碰到
的分支 —— 这比百分比可行动得多。"94%"不指向任何缺口，"第 87-88、91-92、
124-131 行没执行"可以立刻去补。

#### 由此补出的测试：SignalingClient 的错误与重连路径

覆盖率数据立刻暴露出 `src/core/net/signaling_client.cpp` 有 16/61 行从未执行，
恰好是**发送失败**与**断线重连**两条路径（47-51、87-92、124-139）—— 全在
TCP 8887 这条硬约束路径上。互操作测试覆盖了正常收发，但"连不上时会不会退化
成静默失败或疯狂重试"一直没人验证。

新增 `tests/test_signaling_client.cpp`（12 个用例），用真实
`QTcpServer`/`QTcpSocket` 走 127.0.0.1，覆盖发送失败、半包/多包解码、
连接被拒、按间隔真的重试、`stop()` 后不再重试。该文件的覆盖率从 73.8% 提升，
错误与重连分支全部被走到。

##### 实测时序事实（改这些用例前必读）

向 `127.0.0.1` 上无人监听的端口 `connectToHost`，失败**不是立刻返回**：
`Connection refused` 约 **4.1 秒**后才到达。因此一轮重连的真实周期是

```
3 秒（重连定时器） + 约 4.1 秒（这次尝试失败） ≈ 7.1 秒
```

实测时间线：`t≈4.1s / 11.2s / 18.2s / 25.3s` 各一次 `errorOccurred`。

**教训**：绝不要用"3 秒后应出现第二次错误"这类**基于设计常量的固定等待**。
首版就是这么写的，断言失败 —— 看上去像"重连只安排未执行"的**真实缺陷**，
实际是等待窗口短于一次真实往返。差一步就把正常代码误判成 bug。
正确做法是用 `QTRY_*` 等待**属性**（计数增加），给它足够宽的上界。

另一个反例：`start()` 后立刻 `stop()` 时，那次连接尝试**仍会**以错误收场
（探针实测 t≈8.6s）。所以"stop 后不得重试"绝不能断言"零错误" —— 那会让
正确代码失败。必须先把这次不可避免的错误等出来记为**基线**，再断言之后
不再新增。

### 报文契约被复制成两份，却没有人盯着"两份还一样吗"

第 20/21 轮用「链接期替身」测指纹流程，前提写在替身文件顶部：

> 关键约束：**buildPacket() 必须是真实算法**。报文字节是唯一需要逐字节守住
> 的契约，如果连这里都换成"简化版"，测试就退化成"替身和自己比"。

这个前提**是对的，但从未被自动验证**。第 25 轮补查后暴露两个后果：

1. **生产的 `buildPacket()` 覆盖率为 0。** 它从未被链接进任何测试目标，
   报文字节实际上没有被任何运行时测试直接验证过——所有指纹测试跑的都是替身。
2. **契约存在两份副本**（生产一份、替身一份），却没有一条检查盯着两者是否
   仍然一致。有人改了生产的校验和起始下标或长度字段，替身不跟着变，
   测试照样全绿，而真机报文已经错了。

这是第 23 轮"平行实现"教训的**又一种形态**：第 23 轮是"同一功能有两套实现
（`FingerprintManager::startEnroll` 与 `Form_finger` 内联状态机）"；
本轮是"同一契约有两份拷贝"。前者靠静态检查钉住**线上字节**，后者要靠
静态检查钉住**两份拷贝的一致性**。

#### 查明其实不必用替身

`buildPacket()` 是 **public 且纯函数**：只拼字节，不碰串口；构造函数也只
`setPortName/setBaudRate` 等设置参数，**不 `open()`**。所以可以**直接链接
真实实现**来测，无需替身、也无需给生产代码加任何测试钩子。

新增 `tests/test_fingerprint_wire.cpp`（14 个用例，链接真实的
`src/hal/fingerprint_hailin.cpp`），覆盖：

- GET_IMAGE 帧的**逐字节**期望值（`ef 01 ff ff ff ff 01 00 03 01 00 05`）
- 包头 `0xEF01` 与广播地址 `0xFFFFFFFF` 恒定
- 长度字段 = payload + 2、**大端**，含跨字节边界（253/254/300）
- 校验和**自下标 6 起算**（附反证：从下标 0 起算会差 1260）
- 校验和**大端**（特意构造 ≥ 0x100 的值，否则大小端写反也看不出来）
- 6 种已知命令字面量的完整帧
- 与 `FingerprintFrameParser` 的**互操作**（构造出的帧必须能被自己的解析器解回）
- 设备不可用时不崩：`open()` 失败返回 false、`sendPacket()` 返回 false

##### 实测：未打开的 `QSerialPort` 上调 `waitForReadyRead()` 会**段错误**

首版用例里有一条"串口未打开时 `readPacket()` 应返回空"——它让测试**段错误**
（退出码 139）。探针查明这不是测试的 bug：

```
=== A: 未打开的 QSerialPort 上调 waitForReadyRead ===
isOpen = false  即将调用 waitForReadyRead(50)
Segmentation fault        退出码=139
```

崩溃栈落在 Qt5 `QSerialPort` 内部。`readPacket()` 的循环第一句就是
`m_serial.waitForReadyRead(100)`，所以**对未打开的设备调它属于 Qt 未定义行为**。

原来的用例是"想当然地认为调一下没坏处"。改正后改为两条**安全且更有价值**的
用例：不存在的串口 `open()` 必须返回 false 且不崩（真实场景：接错线/驱动没起来），
以及设备不可用时纯逻辑仍正确、`sendPacket()` 返回 false 而不是崩。

##### 实测：`calcChecksum()` 曾是**死代码**，校验和逻辑在同一文件里有两份
##### —— 第 31 轮已消除重复

```
src/hal/fingerprint_hailin.cpp:484  quint16 HailinFingerprint::calcChecksum(...)
                                    调用者：0（第 31 轮前）
buildPacket() 体内：不含 calcChecksum，自己内联了同一个累加循环
文件里 `for (int i = 6; ...) sum += ...` 出现次数：2
```

实测 baseline（`terminal/smartpronew/HailinFingerprint.cpp`）**同样零调用者**——
是**继承来的**，不是重构引入的。

但它是个真实隐患，故障注入给出了确切后果：**只把 `calcChecksum` 的起始下标
从 6 改成 0，18 个指纹用例全部照常通过**（因为没人调用它）。若有人"顺手修好"
这个死函数并开始调用，报文校验和就会与 `buildPacket()` 不一致。

**第 31 轮取了"让 `buildPacket()` 调用它"这条方案**：重复消除、死代码消失，
行为逐字节不变（累加范围仍是自下标 6 起算）。配套地：

  - `tests/fake_fingerprint_device.cpp` 的替身也改为调用辅助函数（否则替身里
    又会出现第二份循环），替身与生产的两份辅助函数体由
    `check_fake_fingerprint_builds_same_bytes` **逐操作比对**；
  - `check_fingerprint_checksum_logic_is_not_duplicated` 从**告警**升级为
    **断言**：循环又出现 >= 2 次、或只剩一份却零调用者，都会 fail；
  - 原先那条"（已知不可区分·死代码）"故障注入**变成了可区分**，已转为正常
    致命变异（见 `tools/verify_fingerprint_wire_test.py`）。

这正是"**把猜测写成契约比不写更糟**"（约定第 46 条）的正面收尾：第 21 轮
如实标注"不可区分"，第 31 轮让那个标注失效的前提（死代码）不再成立。

### 未覆盖行必须再分一维：不可达 vs 可达未测

第 25 轮量出 `fingerprint_hailin.cpp` 覆盖率 **17.9%**，看着像个大洞。第 26 轮
把"未覆盖行"按**可达性**拆开，结论完全不同：

| | 行数 |
| --- | --- |
| 可执行行 | 285 |
| 其中：所在函数**零调用者**（静态不可达） | **204**（71.6%） |
| 其中：所在函数有调用者，但测试没跑到 | 81（未执行 30） |

也就是说"未覆盖 234 行"里，只有 **30 行**是可以靠写测试补上的。其余 204 行是
**基线就没接线**的死代码：

| 零调用者方法 | 可执行行 | baseline 是否也有调用者 |
| --- | --- | --- |
| `autoIdentify` | 40 | 无 |
| `autoEnroll` | 46 | 无 |
| `cancel` | 20 | 无 |
| `stepRegister` | 50 | 无 |
| `stepIdentify` | 44 | 无 |
| `calcChecksum` | 4 | 无（**第 31 轮已接线**，见上文） |

四个入口在**生产代码里零调用者**，实测 baseline 同样如此 —— **继承而来**，
既不是重构缺陷，也不该被算作待办。为它们写运行时测试只会测到"平行实现"
（第 23/30 条）。（`calcChecksum` 原也在此列，第 31 轮改由 `buildPacket()`
调用，已从"静态不可达"转为**可达**。）

**只报一个 17.9% 会同时误导两个方向**：既把死代码算成"待补缺口"，又把真正
可补的那 30 行淹没在噪声里。所以覆盖率必须再拆这一维：

```
未覆盖 = 不可达（基线没接线，不是待办） + 可达未测（**待办看这一列**）
```

`coverage_report.py` 现在会输出这张分类表，并直接列出"可达但未执行的具体行"。

#### 由这一分类补出的真实缺口：识别流程的失败分支

分类表指出 `fingerprint_manager.cpp` 有 6 行"可达未测"，其中 4 行属于**现场
真正在跑的识别流程**（`widget.cpp` 的定时器调 `startIdentify`）：

```
L145  emit identifyResult(false, -1, 0, "生成特征失败");
L149  return;
L186  qDebug() << "指纹识别失败";
L188  emit identifyResult(false, -1, 0, "指纹识别失败");
```

这两条失败分支是**用户能看到的界面提示**，此前一条测试都没走到 —— 典型的
"有测试 ≠ 被测到"。

新增 3 条用例（`identifyReportsGenCharFailure`、`identifyReportsSearchFailure`、
`identifySuccessRequiresZeroConfirm`）。其中两条失败用例都带**负面断言**：

- GEN_CHAR 失败后**不得**再下发 SEARCH（只看"出现了失败信号"不够 —— 若代码在
  emit 之后继续走完流程，信号照样出现，但现场会多发一条无意义的 SEARCH）；
- SEARCH 未命中时 id/score 必须清零，不得把未命中的 id 报上去。

第三条是**对照实验**：确认码 0x00 时必须成功。缺了它，前两条即使断言写反也
可能碰巧通过。

实测效果（同一口径）：`fingerprint_manager.cpp` 可达未测 **6 → 1** 行
（剩下那行 L107 是已记录为无法确定性复现的竞态），已测文件覆盖 714 → **719**
行，真实总覆盖率 27.4% → **27.6%**。

#### 第 35 轮：`form_passwd.cpp` 唯一的未覆盖行是**逻辑不可达**

`form_passwd.cpp` 接进测试后 46 行覆盖 45（97.8%），剩下一行是 L171：

```cpp
void Form_passwd::playKeySound(const QString& path)
{
    QWidget* mainWin = getMainWidget();      // 沿父链找第一个 Widget
    if (mainWin) {
        auto widget = qobject_cast<class Widget*>(mainWin);
        if (widget) { widget->audioManager()->playAsync(path); }
        else { qDebug() << "[passwd] mainWin is not Widget*"; }   // <- L171
    } else { qDebug() << "[passwd] mainWin is nullptr"; }
}
```

L171 **不可达**，而且是**逻辑上**不可达，不是"没人接线"。
`getMainWidget()` 的循环条件就是
`while (w && !qobject_cast<class Widget*>(w))` —— 它**只在 `qobject_cast`
成功时才返回非空**：

```cpp
QWidget* Form_passwd::getMainWidget() {
    QWidget* w = parentWidget();
    while (w && !qobject_cast<class Widget*>(w)) w = w->parentWidget();
    return w;      // 返回非空  =>  w 必然已经是 Widget*
}
```

于是 `playKeySound` 里对同一指针再 `qobject_cast` 一次**必然成功**，`else`
分支永远走不到。这与上面"零调用者"的不可达**成因不同**（那是基线没接线），
但同样不是待办 —— 要覆盖它，只能构造一个"`qobject_cast` 两次结果不同"的
对象，那在 Qt 的元对象系统里不可能发生。

> 保留它（不删）是对的：真 `Widget` 与影子 `Widget` 的类型关系在产线/测试
> 下不同，删掉这个分支等于放弃对"父链上存在非 `Widget` 的 QWidget"这种
> 退化情形的防御。判断为不可达只影响**覆盖率口径**，不影响代码本身。

其余 8 个"从未被任何测试目标链接"的产线文件（`widget.cpp`、`form_finger.cpp`、
`main.cpp` 等）仍未接入，见上文表格 —— 那是下一批待办。
`form_call.cpp` 第 36 轮已接进 `t113_test_form_call`（见下文专节）。

#### 故障注入夹具本身也暴露了两个问题

给这 3 条用例做故障注入时，5 个变异全部能让对应用例失败。但过程中夹具自己
两次挡住了我：

1. **锚点不唯一自证起了作用。** 变异目标
   `if (resp2.size() < 12 || …)` 在文件里出现**两次**（`startIdentify` 与
   `startEnroll`）。若不收窄锚点，就会改错函数 —— 测试照样失败，但我以为
   验证的是识别路径，实际验证的是注册路径。这是"夹具自证锚点唯一"第一次真正
   发挥作用。
2. **"规则未触发"其实是夹具缺陷。** 我对新规则做故障注入时，前 3 个用例都报
   "期望报错、实得通过"。原因是关键字在文件里各出现 5 次，而我只替换了**第一处**，
   其余仍满足规则 —— 规则不报错是**正确**行为。这正是第 33 条警告的情形。

还有一条**我写错的期望**值得记下来：我曾期望"把分类函数一致改名后规则应失败"。
但改名是**保持行为**的重构，规则检查的是"能力是否存在"而不是"有没有被改动"，
通过才是对的；而且新名字 `_classify_uncovered` 仍包含原子串。这条已改成**对照
实验**保留，并在规则 docstring 里写明它的**能力边界**：它**不能**发现"函数还在
但被掏空成 `return {}`"，那需要真的跑一次脚本。不写清这一点，就会高估这条规则。

### 门禁密码不得进日志：静态规则之外，还要一条**运行期**测试

baseline 的 `PasswordManager::checkPassword()` 把四样东西打进 `qDebug`：

```
input= <用户输入>  password= <正确密码>  inputHex= <输入hex>  pwdHex= <密码hex>
```

门口机是**实体门禁设备**，密码就是物理准入凭据。`qDebug` 在嵌入式 Linux 上
落到 stderr / systemd journal：任何有日志读取权限的人都能拿到，且跨重启留存。
**输错一次，门禁密码就交出去了。**

重构已修掉（只记录"本次是否匹配"）。但此前守它的只有一条**静态文本规则**
`check_no_secret_in_logs` —— 它扫源码里敏感变量名有没有出现在日志语句里。
静态规则可能被绕过：**改名、拼串、经辅助函数转一手**，正则就看不见了。

第 27 轮补上 `tests/test_password_manager.cpp`：真的把密码设成哨兵串、真的调
校验、真的把 qDebug 抓下来，断言哨兵串及其 hex 一次都没出现。这也是
`password_manager.cpp` 第一次被链接进任何测试目标（第 26 轮统计的 18 个
"从未链接"文件之一）。

故障注入 8 个变异全部行为正确：6 种**真实泄漏形态**都被抓到，
2 个对照（只打长度、只打空判定）不误报。

#### 但第一版测试里有一条**恒真**的断言

这是本轮最值得记的事。

第一版用**环境变量**注入一个**非 ASCII** 哨兵
（`门禁-哨兵-密码-8842`），然后断言"日志里不含该哨兵的每一个前缀"。

故障注入发现"**前缀脱敏** `password.left(4)`"这个变异**抓不到**。探针一查：

```
ASCII  设定 == 取回 ? true      （26 码元 -> 26 码元）
UTF8   设定 == 取回 ? false     （13 码元 -> 16 码元）
```

原因是 `AppConfig::toQString()` 用的是 `QString::fromLocal8Bit()`
（`app_config.cpp:67`）。在本机 GBK 代码页下，写进去的 UTF-8 字节被按 GBK
解码，13 个码元进、16 个出。

于是"日志里不含哨兵前缀"这条断言，在比较**两个不同的串** —— **恒真**。
断言在，覆盖面看着很全，实际什么都没验证。

修法有两步，缺一不可：

1. 非 ASCII 与前缀脱敏场景改用 `setPassword()`（进程内 `QString`，
   **不经过编码边界**），构造上保证哨兵原样；
2. 另加一条 `asciiFixtureSurvivesEnvRoundTrip`，显式断言**走环境变量的
   ASCII 哨兵原样回到 AppConfig** —— 让夹具在变形时先炸，而不是让安全断言
   静默失效。

**凡夹具跨过一次编码边界（环境变量、文件、argv），必须先断言它原样到达。**
否则后面围绕它的所有断言都可能是空的。

另外，前缀断言改成**逐一覆盖每个前缀**（`for n in 1..len`），而不是只查一个
固定长度：只查 `left(4)` 会漏掉 `left(2)`、`mid(0,n)`、`.at(0)` 等其它脱敏
写法，而前缀脱敏对短密码等于全泄。

### V4L2Camera：画面源头，以及一条**修掉的真泄漏**

`V4L2Camera` 是整个视频链路的源头 —— UDP 23456 通话视频、UDP 50000 远程监看、
人脸检测、人脸门禁**全都取自它的 `getFrame()`**。它此前从未被链接进任何测试
目标，而它的危险分支**全在系统调用失败上**（设备不存在、权限不足、`S_FMT`、
`REQBUFS`、`QUERYBUF`、`mmap`、`QBUF`、`STREAMON`、`DQBUF`、JPEG 解不出来）。
这些在现场（工业门口机）都是常态。

现有 `tools/devstubs/stub_impl.c` 是**固定**桩（`ioctl` 恒 `-1`、`mmap` 恒
`MAP_FAILED`），只能做链接检查 —— 走不到任何成功路径，也无法按需让某一环
失败。所以第 30 轮写了**可编程假 syscall 层**（`tests/fake_v4l2_io.{h,cpp}`）
并用 **`-Wl,--wrap`** 接上去（29 个用例，链接真实的 `v4l2_camera.cpp`）。

#### `--wrap` 的两个坑（都是探针实测出来的）

`open`/`close` 在 MinGW 的 CRT 里已存在（直接定义同名符号会冲突），
`ioctl`/`mmap`/`munmap` 则根本不存在，因此必须用链接期 `--wrap`：

1. **`--wrap` 生成的是 C 符号引用**（`__wrap_open`）。定义若写成普通 C++
   函数就会被名字修饰掉，链接期报 `undefined reference to '__wrap_open'`。
   → 所有 `__wrap_*` 必须 `extern "C"`。
2. **必须只劫持目标路径**（`/dev/video*`），其余一律委托 `__real_open`。
   否则整个进程的文件 I/O（Qt 的字体、图标、qrc）都被接管，测试会因无关
   原因失败。

#### 量出来的真实缺陷：`mmap` 成功后的失败分支**泄漏映射**

`open()` 在 `mmap` **成功之后**，`VIDIOC_QBUF` 与 `VIDIOC_STREAMON` 的失败
分支只 `::close(v4l2_fd)` 而**不 `munmap`**。而 `close()` **不会**自动解除
映射；重试 `open()` 又会用新映射覆盖 `v4l2_buffer` —— 旧映射的指针就此丢失，
**永久无法回收**。临时探针量到的数值：

| 场景 | 修复前 `close()` 后仍映射 | 修复后 |
| --- | --- | --- |
| 单次失败后对象析构 | 0（析构回收了，属延迟释放） | 0 |
| 失败一次 → 重试成功 → `close()` | **1048576 字节** | 0 |
| 连续 3 次失败 → `close()` | **2097152 字节** | 0 |
| `STREAMON` 失败 → 重试成功 → `close()` | **1048576 字节** | 0 |

> 单次失败时析构能回收，所以**光测"一次失败"是抓不到的** —— 必须测
> "失败之后重试"这条序列，这正是现场的真实情况（启动时设备未就绪，稍后重试）。

**这是继承缺陷，不是重构引入的回归**：比对脚本确认 baseline 与重构的
`open()` 失败分支**逐字节等价**（两者 `munmap` 计数都是 1、`STREAMOFF`
都是 0）。修法是在两条失败分支里先把映射解除掉再 `close()`，
`munmap` 计数因此从 1 变成 3。

**行为兼容性**：该修复不改变任何线上字节、不改 UI、不改线程生命周期 ——
只在"打开失败"这条**本来就没有画面**的路径上多释放一块内存。

#### 另一条更隐蔽的：解码失败也必须**归还驱动缓冲区**

`req.count = 1`（只有一个缓冲）。`DQBUF` 把缓冲从驱动取走，必须用 `QBUF`
还回去；若解码失败就提前 `return` 而不归还，**下一次 `DQBUF` 会永远失败，
整条画面链路彻底停摆**，现场表现是"画面刚出来一下就再也不动了"，且**没有任何
报错**。测试用一条独立用例把"解码失败仍须 `QBUF`"钉住。

#### 故意**不**测的一条

我原本想写"`getFrame()` 返回内部引用（去掉 `.copy()`）"的变异，但
**`QImage` 是写时复制（COW）的** —— 返回内部引用与返回副本在可观测行为上
完全一致，任何断言都抓不到它。按第 50 条（"没有判别力的断言要当作没有断言"），
这条**没有**写成用例，限制记在这里。

### 人脸门禁：决定门开不开的那个比较，终于被测到了

`FaceManager` 决定**门开不开** —— `widget.cpp:889` 检出人脸后调用
`recognize()`，由 `recognizeResult` 决定是否开门。它此前从未被链接进任何测试
目标。好在本机 OpenCV 带 face 模块（`libopencv_face.dll.a`），所以第 29 轮
**链接真实的 `face_manager.cpp` 并用真实 LBPH 跑**（20 个用例），而不是写个
替身自我验证。

#### 先做探针，再写断言 —— 这次推翻了我自己的假设

动手前写了三个临时探针（未入库），量到三条决定性事实：

**事实 1：模型未训练时 `predict()` 抛 `cv::Exception`。**

```
OpenCV(5.0.0) ... lbph_faces.cpp:406: error: (-5:Bad argument)
This LBPH model is not computed yet. Did you call the train method?
```

而 `recognize()` 的守卫是 `registeredFaces.size() < collectTarget` —— 数的是
**样本数**，不是"模型已训练"。两者在"样本填满但跳过 `train()`"时不相等。

**事实 2：单 label 时 `predictedLabel == 1` 恒为真。**

训练时 `labels` 全是 1（单用户门禁），LBPH 只有一个类别。于是判据
`predictedLabel == 1 && confidence < threshold` 里**前半永远成立**，
**阈值是唯一的门**。

**事实 3（推翻我的假设）：我怀疑的崩溃链不成立。**

我原本推断"人脸框越界 -> `QImage::copy()` 返回空图 -> 存进空样本 ->
`train()`/`predict()` 抛异常 -> UI 线程崩溃"。探针实测：

```
QImage::copy(越界矩形) isNull=0 size=100x100     <- 不返回空图！
空图也能正常转 cv::Mat、正常 train/predict
```

**`QImage::copy()` 传越界矩形返回的是"指定尺寸、内容未初始化"的图。**
这条猜测是错的，因此**没有**写成断言 —— 探针的价值有一半在于否掉错误假设。

#### 由事实 2 决定的断言取向：不硬编码 confidence

既然阈值是唯一的门，那么"硬编码某个 confidence 数值"是没有意义的（换
OpenCV 版本或输入就会变）。所以阈值断言一律**用被测代码自己吐出的
confidence 当基准**：

| 阈值设置 | 期望 | 证明了什么 |
| --- | --- | --- |
| `conf + 1` | 接受 | 比较方向正确 |
| `conf` | **拒绝** | 比较是严格 `<` 而不是 `<=` |
| `-1` | 拒绝 | 边界收得住 |

再配一条阳性对照（同图案距离必须**小于**异图案距离），防止两张图 conf 恰好
都是 0 时整组断言变成恒真。

#### 一个"钉住隐患、但不改代码"的决定

事实 1 描述的是**隐患**而非已爆的缺陷：唯一调用点 `widget.cpp:767-768` 在
`sampleCount() == sampleTarget()` 时**立即**调用 `train()`，所以"样本数到 3"
与"模型已训练"是同一件事，该路径当前不可达。baseline 的守卫完全相同
（`face_manager.cpp:76`），属**继承**行为。

但它一旦被别的新路径触发（界面预加载样本、把 `train()` 改成异步……），异常会
从 UI 线程的槽里逃出事件循环，**直接终止进程**。所以第 29 轮用一条用例
（`samplesWithoutTrainingMakeRecognizeThrow_documentedHazard`）把**现状显式
钉住**：若将来有人把守卫改成检查模型训练状态，该用例会失败，从而迫使"是否
收敛"成为一个**显式决定**，而不是随手的改动。

#### 门禁阈值：上限**已收紧到 LBPH 距离的饱和点以下**（第 31 轮发现、第 38 轮修复）

探针证实门禁安全**完全取决于** `behavior.face_confidence_threshold`。

第 29 轮的规则写成"它**没有任何范围约束**（`app_config.cpp` 里
`qBound`/`clamp` 调用总数为 0）"—— **这是误报**。数值访问器统一在
`app_config.h` 出口兜底，夹取根本不在 `.cpp` 里。规则只搜了 `.cpp`，
于是把"读错文件"当成了"没有约束"。

**真实问题**是：下限 `0.0` 有效（挡负数），但原上限 `1e9` **大到形同虚设**。
第 31 轮把规则改成读头文件、解析真实上下限，但**判据定成了 `1e6`**，于是
`1000` 这种同样致命的写法被判为合格。

第 38 轮用探针把 LBPH 的距离量了一遍，才拿到真正的判据：

| 对象 | 实测距离（LBPH `create()` 默认参数） |
| --- | --- |
| 同一身份（含噪声/光照抖动） | 0 ~ ~103 |
| 完全不同的图像（冒充者） | ~170 ~ ~256 |
| 随机噪声 | 饱和于 ~256 |

距离在 **~256 处饱和**，所以阈值 `>= 256` 时**任何**图像（含完全陌生的脸）
都判"通过"：`1e9`、`1000`、`256` 在安全意义上完全等价。上限因此收到 `150.0`
（落在"本人 <= ~103"与"冒充者 >= ~170"之间）：

```cpp
// src/config/app_config.h
double faceConfidenceThreshold() const
{ return clampDouble(m_faceConfidenceThreshold, 0.0, 150.0); }
```

默认值 `90` 仍在区间内，故**未配置时行为逐字节不变**；只有把阈值配到
`150` 以上（含 `900` / `1e9`）才会被夹到 `150` —— 这是第 38 轮的一次
**有意的行为变化**。配套守卫：`tools/verify.py` 的 face-gate 由告警升级为
**失败**（上限 `>= 256` 即红，两侧边界由 `tools/verify_face_gate_rule.py`
的 `256.0` / `255.0` 用例钉住），`test_app_config.cpp` 钉死夹取结果，
`test_face_manager.cpp` 从行为侧证明"阈值推到上限 150 时冒充者仍被拒绝"。

### 指纹串口打不开时：从"每 2.5 秒刷两条日志"到"静默重试"

第 39 轮在本机跑**真实可执行文件**时发现的现场级缺陷 —— 静态检查与单元测试
都没覆盖到它，因为两边都只测"串口正常"的路径：

```
每 15 秒：
  QIODevice::write (QSerialPort): device not open   × 24
  [指纹] 串口写入不完整                              × 24
```

根因是一条**只看了一半**的守卫链：

1. `widget.cpp` 启动时 `open()` 一次，失败后**再没有任何代码**会重新打开串口
   （全工程只有这一处 `open()`）；
2. `FingerprintManager::startIdentify()` 检查了 `!fingerprint`、`m_suspended`、
   `m_waitingFinger`，**唯独没检查 `fingerprint->isOpen()`**；
3. `HailinFingerprint::sendPacket()` 直接 `m_serial.write()`，**也没检查
   `isOpen()`**。同文件的 `cancel()` / `readPacket()` 等**都检查了**，只有这个
   唯一的写出口漏了。而 `QSerialPort::write()` 在关闭状态下会由 **Qt 自己**往
   stderr 打一条 warning —— 它不受本工程日志开关控制，也压不住；
4. 识别流程随即"结束"，`widget.cpp` 500ms 后重新 `startIdentify()` → 死循环。

**修法**：串口不可用时不进识别循环，改为**每 5 秒重试一次 `open()`**，设备一旦
出现就自动恢复识别，无需重启进程。

关键取舍：**只加守卫是不够的**。因为没有任何代码会重新 `open()`，裸加
`isOpen()` 守卫等于把指纹功能**永久关死**。要么"守卫 + 重试"，要么"干脆不做
指纹"—— 不能只有守卫。这条已写成开发约定第 71 条。

顺带把**日志速率**也一并收了：重试每 5 秒打一条同样的话，一个月约 **52 万条**，
在嵌入式 flash 上只是从"刷屏"变成"慢性泄漏"。现在"打不开串口"只在**进入**该
状态时提示一次，成功打开后复位（于是"插上设备 → 再掉线"仍会在日志里留痕）。

实测（同一台机器、本机无指纹硬件）：

| 日志 | 修复前（15s 窗口） | 修复后（20s 窗口） |
| --- | --- | --- |
| `device not open` | 24 | **0** |
| `串口写入不完整` | 24 | **0** |
| `打开串口失败` | 4 | **1**（只提示一次） |
| 日志总行数 | 60 | **13** |

配套测试（`tests/test_fingerprint_manager.cpp`，两条**都能变红**，已实测）：

- `closedPortDoesNotEnterIdentifyLoop` —— 串口没开时**不得**有任何下发/读串口
  （去掉守卫，实测立刻变红）；
- `reopenRetryResumesIdentifyWhenPortBecomesAvailable` —— 设备后来可用时**必须**
  自动恢复（让重试失效，实测立刻变红）。

同时修掉一个被这两条用例暴露出来的**替身自身缺陷**：
`tests/fake_fingerprint_device.cpp` 的 `reset()` 没有清 `g_openDevices`，而替身是
按**对象地址**记"哪些设备已打开"的。测试里的设备都是栈对象、地址会被后续用例
复用，于是上一个用例 `open()` 过又没 `close()` 的设备会把地址留在集合里，下一个
用例新建的设备"一出生就是打开的"（`QVERIFY(!device.isOpen())` 因此莫名其妙地
失败）。已把 `g_openDevices.clear()` 补进 `reset()`。

### 物理门锁那条线：GpioLine 终于有了可注入失败的测试

`GpioLine` 管的是门口机**物理门锁/补光**那条线（T113 的 PE5 = 线号 133，开锁
与补光共用同一条线，见 `widget.cpp:224-226`；PE4 = 132 是红外输入）。
它此前从未被链接进任何测试目标，而它的危险分支**全在硬件出错上**：

| 故障 | 处理错的后果 |
| --- | --- |
| 打不开 gpiochip（权限/节点缺失） | 程序起不来，或静默无灯无锁 |
| 拿不到线（线号越界/被占） | 同上 |
| 请求输出失败（线已被占） | **该开门时不开** |
| 读电平失败 | 红外误判为"没人" |
| 请求泄漏（忘了 release） | 线一直 busy，**下次开锁直接 EBUSY** |

真机 gpiochip 无法按需制造这些失败，开发机也根本没有 gpiochip。所以第 28 轮
写了**可编程假 libgpiod**（`tests/fake_gpiod.{h,cpp}`）并接出
`t113_test_gpio`（25 个用例）。故障注入 13 个危险退化全部被抓到。

#### 夹具自造过两次假象 —— 两次都是夹具错，不是生产代码错

这一轮最值得记的是：**第一版测试失败时，我先怀疑的方向全都是错的。**

**假象一：线号上限设成了 64。** 假 libgpiod 第一版给每个 chip 64 条线，而工程
用的是 132 / 133。于是**所有正常用例全部失败**（`openOutput` 返回 false）——
看起来像"生产代码开不了线"。实际是夹具把合法线号判成了越界。

**假象二：内存所有权模型不对。** 我一开始让 `gpiod_chip_get_line` 各自
`new`、`gpiod_line_release` 各自 `delete`。但 **libgpiod 1.x 里线对象属于
chip**（`get_line` 返回 chip 内部的数组元素），`release` **只清除"已请求"
状态、不释放内存**，`chip_close` 才释放。于是生产代码在"请求失败"路径里
先 `release` 再 `close` 时，夹具报出 `release:unknown`，看着像生产代码重复释放。

两次都是**夹具与真实 ABI 不符**造成的。教训：写硬件替身时，**先把真实库的
内存所有权与取值范围查清**，否则夹具会稳定地产生假阳性 —— 而且假阳性看起来
比真缺陷更像真缺陷。

#### 由此得到真正的现场度量：`busyLineCount()`

第一版我用"线对象个数"判断泄漏，那在错误的所有权模型下毫无意义。改成假实现
与真实 ABI 对齐后，真正该数的是 **仍持有有效请求的线数**：

> 一条泄漏的请求会让线一直 busy，下次开锁直接 EBUSY 失败。

并且假实现让**对已 busy 的线再次请求返回失败**（真实 EBUSY 行为），
于是"泄漏了一条请求"能被**端到端**抓到（重试真的失败），而不是靠事后数数。

#### 一个"只观测、不断言"的地方

生产代码在"请求失败"路径里会 `release` 一条**从未成功请求**的线
（防御性清理）。libgpiod 对这种情况的确切行为我**无法在本机验证**（没有真实
`gpiod.h` / 库）。因此夹具只记录 `releaseUnrequestedCount()`，测试**不断言**
它等于几 —— 把猜测写成契约比不写更糟。

### 不可达槽与"平行实现"：为死代码写测试的陷阱

第 23 轮审计发现：`FingerprintManager::startEnroll(int)` 有完整实现、有挂起
守卫、有分步状态机，第 21 轮我还为它写了 **26 个单元测试并全部通过** ——
但它在整个工程里**没有任何调用者**，也不是任何 connect 的目标。

现场真正跑的指纹注册是 `Form_finger` 里那套**内联**状态机
（`startFingerRegisterStep()` + `doFingerCollect()`），它直接调
`buildPacket/sendPacket/readPacket`，完全绕过 `FingerprintManager`。

**这是"有测试 ≠ 被测到"的升级版**：不是断言没落在输出上，而是**被测对象
根本不在执行路径上**。26 个绿灯与现场行为毫无关系。

两份实现还**并不等价**（实测抽取出来的序列）：

| | 轮询探手指 | 采集前再发 GET_IMAGE | GEN_CHAR | REG_MODEL | STORE_CHAR |
| --- | --- | --- | --- | --- | --- |
| A `startEnroll`（**无调用者**） | — | 是（500ms） | 是 | 是 | 是 |
| B `Form_finger`（**现场执行**） | 是（500ms） | 是（2000ms） | 是 | 是 | 是 |

B 比 A 多一轮由定时器发出的 `GET_IMAGE` 探手指。B 与 baseline 的
`form_finger.cpp` **逐字节一致**（两处各 9 个 `append`、命令序列与
`readPacket` 超时完全相同），所以 B 才是必须守住的契约。

**为什么 B 没有单元测试**：它是 UI 类，`startFingerRegisterStep` 要经由
`parentWidget()` 上溯到 `Widget` 才能拿到 `fingerprint()`，还要真实的
`QTimer` 与提示音资源。给它造夹具比被测代码本身还复杂，而且会**再造一份
平行实现**——正是本轮要消除的问题。因此改用**静态契约检查**把它的线上字节
钉住（与"协议常量必须被钉住"同一思路）。

`check_live_fingerprint_enroll_sequence_is_pinned` 按源码顺序抽取
`append((char)…)` 的常量与 `readPacket(超时)`，与内置期望序列逐一比对：

```
0x01,500 | 0x01,2000 | 0x02,<var>,2000 | 0x05,2000 | 0x06,1,<var>,<var>,2000
 探手指      采集         生成特征           合并         存储
```

三个对照变异全部被抓：轮询超时 500→999、`GET_IMAGE` 字节 0x01→0x77、
`STORE_CHAR` 的 BufferID 1→2。

#### 全工程不可达槽清单

`check_declared_slot_is_reachable` 报出 3 个（**警告级**，因为无法排除
"刻意保留的公开 API"与跨仓库调用）：

| 槽 | 说明 |
| --- | --- |
| `FingerprintManager::startEnroll` | 现场走 `Form_finger` 内联实现，这条永不被调用 |
| `Widget::onFingerprintEnroll` | 对应 `FingerprintManager::enrollResult`，而该信号只在 `startEnroll` 里发射 → 永不触发 |
| `Widget::onFingerprintIdentify` | `identifyResult` 已被改成 lambda 连接，这个槽从未接线 |

三者**baseline 同样存在**（baseline `widget.cpp:227` 那行 connect 是被注释掉的，
`:237` 的 `enrollResult` 连接同样指向一条死路），因此属**继承死代码**，
重构并未引入。本轮不改行为，只把它们**显式标注出来**并加自动化告警。

必须排除 `on_<对象名>_<信号>()`：它们由 `connectSlotsByName` 自动接线，
否则会把全部按钮槽误报成死代码（本工程有 5 个这样的槽）。

### 指纹应答的包标识校验缺口（已知偏差，已告警未修）

同一个 `fingerprint_hailin.cpp` 里的解析路径判据**不一致**：

- `autoIdentify()` 显式校验 `(quint8)resp[6] != PKT_ACK` 后才解析；
- `stepRegister()` / `stepIdentify()` / `cancel()` 只判长度与 `resp[9]`；
- `FingerprintManager` 的识别轮询回调同样只看 `resp.size() >= 12 && resp[9] == 0x00`。

后果：串口上任何"`byte[9]==0x00` 但包标识不是 `0x07`"的字节流（噪声、
命令回显、错位后重新同步出的伪帧）都会被误判为"手指已按下"，界面直接进入
采集步骤，而实际上根本没有手指。

**本轮未修**，原因：收紧判据会改变现场行为，而项目对协议的要求是
"与 baseline 逐字节兼容"。这不是可单方面由静态检查决定的修复，需显式决策。
当前行为已由 `commandTagFrameIsNotAValidAck` 钉住，避免被无意改动；
静态检查则报 **WARNING**（`[fp-ack-tag]`，共 7 处），不报失败。
自检见 `tools/verify_fp_ack_tag.py`（3/3）。

### 密码输入窗体：第一个 Qt Widgets 测试目标，以及"密码不进日志"的行为证明

`src/ui/form_passwd.cpp` 是门口机**真实门禁的密码录入路径**，也是"密码不得进
日志"这条安全性质的唯一现场。此前该性质**只由一条静态文本规则**
（`check_no_secret_in_logs`）守着，而那条规则自己就写着"只做词法判定，不做
数据流分析"。于是换一种写法就能绕过：

```cpp
const QString leakTmp = inputBuffer;
qDebug() << "[passwd] len-ok tmp=" << leakTmp;   // 静态规则看不出
```

第 35 轮故障注入实测确认：上面这种写法确实躲得过静态规则，而当时**没有任何
行为测试**会拦它。

新增测试目标 `t113_test_form_passwd`（`tests/test_form_passwd.cpp`，24 个
用例），做法是**真的把按键点下去**，用 `qInstallMessageHandler` 捕获**实际
日志输出**，再断言日志里不含刚输入的密码。这比静态规则强：它不关心密码是
"怎么"漏出去的，只要出现在日志里就红。

这是本工程**第一个 Qt Widgets 测试目标**。写它之前先用探针
（`%TEMP%\probe35\`）量清了三件有风险的事：

1. `uic` 能处理真实的 `form_passwd.ui`（无 `customwidget`、无 `<connections>`）；
2. 无显示器时 `QT_QPA_PLATFORM=offscreen` 下 `QApplication` 能起来，
   `QTest::mouseClick` 真能触发 `clicked`；
3. `btn0..btn9` 的文本恰好等于那一位数字 —— 否则 `inputBuffer += btn->text()`
   拼出来的密码就是错的（0-9 全部核对）。

替换了什么、没替换什么：

| 用真的 | 用替身 |
|---|---|
| `form_passwd.cpp` / `form_passwd.ui`（真 uic） | `Widget`（`tests/shadows/ui/widget.h`） |
| `AppConfig`（真配置类） | `AudioManager`（`tests/shadows/managers/audio_manager.h`） |
| `QPushButton` 与真实信号槽连接、密码信号载荷 | — |

两条替身只影响**提示音**这一条支路，不影响密码路径。替身的意义在于：真
`ui/widget.h` 会拖进 `<opencv2/core.hpp>`、`<opencv2/objdetect.hpp>` 与整个
主程序门面；真 `AudioManager::playAsync()` 会 `QtConcurrent::run` 去真的开
ALSA 播放 `/tmp/pcm/beep.pcm`（慢、依赖声卡、文件通常不存在）——那正是要
消灭的"在我这儿能跑"。

安全性质之外，本目标还覆盖：按键拼装顺序、`OK` 只提交一次、提交后清空缓冲、
8 位上限、空缓冲不发信号、退格、`NoFocus`、以及**提示音路径来自 `AppConfig`**
（跟随配置而不是散落字面量）。

故障注入自检见 `tools/verify_form_passwd_test.py`：**16 个变异全部被抓到**
（含 7 种"换写法把密码弄进日志"，其中 3 种静态规则看不出），3 个对照不误报。
这是本轮最有价值的一组变异 —— 它保住的是门禁密码。

### 凭据存储：门禁口令从**明文比对**改为**加盐哈希**（第 41 轮，方案 P2）

上一节保住的是"密码不进日志"。但重构前本工程还有一个**更根本**的缺口：

```cpp
// src/managers/password_manager.cpp（重构前，第 42 行）
const bool matched = (input == password);      // 明文比对
```

`password` 直接来自 `AppConfig`，而 `config/appconfig.json` 里写的是
**明文** `"door_password": "5788"`。三件事同时成立：

1. 配置文件被谁拿到，门禁密码就被谁拿到（运维、备份、整机镜像都算）；
2. 进程内存被 dump 时，口令直接可读；
3. `QString::operator==` **短路** —— 比较在第 1 个不同的字符处停下，
   于是"前缀猜对几个字符"存在可观测的时序差异。

第 41 轮新增 `src/managers/credential_store.{h,cpp}`，并把
`PasswordManager` 的成员从 `QString password` 换成 `CredentialStore`。

**落盘格式**（是契约，变了老凭据就读不出来）：

```
sha256:<盐hex>:<迭代数>:<摘要hex>
```

| 设计点 | 取值与理由 |
|---|---|
| 盐 | 每用户**独立** 16 字节。`makeSalt()` 先读 `/dev/urandom`，取不到退回重复 `QUuid`。**刻意不用 `qrand`**（不是密码学随机源）**也不用 `QRandomGenerator`**（Qt 5.10+，越过本工程 5.9 的 API 底线） |
| 迭代 | 默认 10000，夹在 `[1, 200000]`。上限防"配置里写天文数字把开锁拖成秒级" |
| 比对 | `constantTimeEquals()` 累积异或，长度不同才提前返回（长度不是秘密） |
| 落盘 | `QSaveFile` 原子写 + `setPermissions(ReadOwner\|WriteOwner)`。门口机可能被直接断电，非原子写会留下半截 JSON → 下次启动解析失败 = **门忽然打不开** |
| 锁定 | 按 userId 记 `failed` 与 `lockedUntilMs`。**默认 `maxAttempts=0` = 从不锁定** —— "未配置时行为与重构前一致"是硬约束；启用锁定是显式行为变更 |
| 临时码 | 一次性 + 到期即失效，**只做存储层**，未接 UI |

**迁移必须是显式的**（方案 P2 第 3 条）。三条分支各自打日志：

| 情形 | 行为 | 日志 |
|---|---|---|
| `credentials.json` 已有该用户 | **以文件为准**，配置值不再参与 | `qInfo` 说明配置文件不再参与 |
| 配置值已是 `sha256:...` | 直接装入 | `qInfo`，**不报**"明文" |
| 配置值是明文 | 现场加盐哈希入内存，写进 `credentials.json` | **`qWarning`** 明说"检测到**明文**引导值"，**且绝不打印明文本身** |
| 既无文件也无配置值 | 校验**一律拒绝**，不假造默认口令 | `qWarning` 说明原因 |

明文**不写回** `appconfig.json` —— 那是运维手工编辑的文件，程序改写它会让
"配置是人的输入"这一性质消失，掉电还有损坏风险。哈希住在**独立的**
`credentials.json`（与配置同目录）。运维在首次启动后即可删掉
`security.door_password`，**删掉仍能开门**。

#### 安全边界：**不要高估**这个实现

加盐 + 迭代哈希**不是** Argon2 / scrypt / PBKDF2 —— **Qt5 不提供 PBKDF2**，
本类也没有内存硬度参数。它相对"明文比对"是实质提升（防彩虹表、防"两人同密码
一眼看出"、防时序泄漏、防配置文件被读到即失守），但**不能**用来保护高价值
口令。这条已写进 `credential_store.h` 的长注释，**不得**在别处被描述成
"用了 PBKDF2"。

#### 测试与故障注入

- `tests/test_credential_store.cpp`（新目标 `t113_credential_store`）：**39 条**
  用例，行覆盖 91.8%。
- `tests/test_password_manager.cpp`：12 → **16 条**，新增 5 条迁移路径用例。
- 静态规则 2 条：`check_credentials_are_hashed_not_plaintext`（明文成员 /
  明文比较 / 是否真加盐 / 是否真用 `QSaveFile` / 配置里是否残留明文）、
  `check_credential_store_test_is_wired`（目标是否真的建了、注册了、链接了
  真实现，以及关键断言是否还在）。
- 两个故障注入夹具各 **12/12** 变异被抓 + 对照不误报 + 还原后逐字节一致。

#### 这轮的故障注入抓出了什么（12 处，全是我自己的错）

写这轮代码时，"工具比人可靠"这条又一次成立。要点：

| 抓到的对象 | 问题 |
|---|---|
| **既有规则** `[hex-parse]` | 我直接用了 `QByteArray::fromHex()`。它**不报错**，会跳过非法字符重新配对 —— 被篡改的哈希串会"成功"解出**内容全错**的字节，比解析失败危险得多 |
| **我的新静态规则** | 6 处假绿。最典型两处：`"add_test(...)"` 子串在**注释行**里也成立（把那行注释掉，目标被摘出 ctest 而规则不响）；`"managers/credential_store.cpp"` 在上方**注释块**里就出现过（把目标里的链接删掉也不响） |
| **我的新行为夹具** | 变异写错（把"不累加"写成"加倍"）、对照项 `old == new` 被自己的守卫拦下 |
| **我的新测试** | ①用 `hashSecret(plain, salt, n)` 观测 `setSecret()` —— 盐是测试自己传的，"内部改固定盐"照样绿；②"到期自动解锁"的清理逻辑**删掉也全绿**（`isLockedOut()` 是算出来的）；③非法字符的构造让长度判据掩盖了字符判据 |
| **Release-only 门禁** | `static const int` 被 `qBound()` ODR-使用，Debug 链接失败而 Release 通过（见开发约定第 72 条） |
| **既有脚本** `check_host_compile.sh` | `-isystem $QTSP_INC` 在 `qmake` 缺失时退化成裸 `/mingw64/include`，遮蔽 libstdc++，21 个文件全误报 |

#### 刻意没做的部分（不是遗漏）

1. **锁定旋钮未接配置**：`maxAttempts` / `lockoutMs` 只能由测试设置。
   加公开旋钮就必须遵守开发约定第 10 条（"配置里公开的开关必须真的生效"），
   而"启用锁定"本身是行为变更（锁定期内正确口令也被拒），应由运维显式决策。
2. **临时码未接 UI**：只在存储层，没有窗体、按键、解锁路径接入。
   覆盖率报告会把它算作「不可达」—— 那是**如实反映**，不是缺陷。
   接线需要产品决策（谁能签发、有效期、位数、要不要与呼叫中心联动）。
3. **多用户未启用**：API 全按 userId 索引（已为多用户准备），
   但 `PasswordManager` 只用一个固定 ID `"door"`。


静态契约由 `check_form_passwd_contract_is_pinned` 钉住，
自检见 `tools/verify_form_passwd_rule.py`（9 个变异全抓到，2 个对照不误报）。

> 已知继承缺陷：点一次"退格"会删**两位**（`uic` 的 `connectSlotsByName` 与
> 显式 `connect` 叠加，槽被连了两遍）。`baseline` 亦然，属继承而来。
> 本轮**不修**（改变"退格删几位"是用户可感知的行为变化），改为用特征化测试
> 钉住现状。详见约定第 58 条。

### 通话窗体：stopsend 的**发源地**，以及"两头都有测试、中间没人守"

`src/ui/form_call.cpp` 是门口机**唯一**发出 `stopsend` 信令的地方，而
`stopsend` 在硬约束清单里（TCP 8887 信令语义必须与 baseline 逐字节兼容）。

第 36 轮之前的状态是**两头都有测试、中间没人守**：

| 环节 | 此前由谁守 |
|---|---|
| `Signal::StopSend` ↔ wire 串 `"stopsend"` | `test_protocol.cpp`（toWire / fromWire / 前缀歧义） |
| `SignalingClient::send()` 写出 `"stopsend\n"` | `test_signaling_client.cpp`、interop |
| **"点下停止按钮 → stopsend 真的发出去"** | **无人守** |

`form_call.cpp` 一直挂在"从未被任何测试目标链接的产线文件"清单上。于是
把这一行删掉：

```cpp
widget->notifyPeerStopSend();     // -> TCP 8887 "stopsend\n"
```

此前**全部 15 个测试目标都会照常通过**。现场表现是"点了挂断，但室内机不知道
本端已停发" —— 正是约定第 30 条"有测试 ≠ 被测到"的又一个实例。

新增测试目标 `t113_form_call`（`tests/test_form_call.cpp`，20 个用例）。做法是
起一个**真实** `QTcpServer` 监听，用**真实** `SignalingClient` 连上它，把真实
`Widget` 替身挂成 `Form_CALL` 的父窗口，然后 `QTest::mouseClick` 真的点下停止
按钮，最后断言**对端 socket 收到的字节恰好是 `"stopsend\n"`**。

断言落在**线上字节**上，所以任何一处接线断裂都会红：不发、发错信令（`stop`
而不是 `stopsend`）、发多次、漏掉结尾换行。

替换了什么、没替换什么：

| 用真的 | 用替身 |
|---|---|
| `form_call.cpp` / `form_call.ui`（真 uic） | `Widget`（`tests/shadows/ui/widget.h`） |
| **真实** `SignalingClient` + 真实 TCP socket | `AudioManager`（影子头，避免真 ALSA） |
| `QPushButton`、真实信号槽连接、`hangupRequested` 信号 | — |

这里有个**必须说清的诚实边界**：唯一的替身 `Widget` 的
`notifyPeerStopSend()` 是 3 行真实现的**副本**（真 `widget.cpp` 因依赖 OpenCV
与整个主程序门面而无法链接）。副本对了不代表原件对 —— 所以静态规则
`check_form_call_stopsend_is_wired` **单独去核对真 `src/ui/widget.cpp`** 里那个
函数确实发的是 `Signal::StopSend`，而不是裸字符串。没有这一步，规则就退化成
"替身和替身自己一致"，真实现漂移了照样发现不了。

本目标还覆盖：`hangupRequested` 在**没有 `Widget` 祖先**时也照发（这是真实的
失败模式：窗体被单独构造时就是这种父子关系）、点击不自己关窗口、`NoFocus`、
半透明浮层属性、点停止打断提示音、计时器归零/停止/文案。另有一条**负面断言**：
构造窗体本身不得发出任何信令 —— 缺了它，"点一下收到 stopsend"可能是构造时就
发了（约定第 28 条）。

故障注入自检见 `tools/verify_form_call_test.py`：**17 个变异全部被抓到**
（含 5 种"把 stopsend 接线弄断"，其中"删掉那一行调用"是首要变异），
1 个对照不误报，还原逐字节一致。
静态契约自检见 `tools/verify_form_call_rule.py`：**12 个变异 + 1 个文件系统
变异全部被抓到**，3 个对照不误报。

> 本轮真实踩到的假绿：`startCallTimerShowsZeroAndRestartsTheCounter` 第一版只
> 比较 `ui->label->text() == "通话中 00:00"`，而 `startCallTimer()` 末尾**无条件**
> 写了 `ui->label->setText("通话中 00:00")` —— 于是把 `callSeconds = 0;` 删掉
> 之后标签照样是 `00:00`，断言 rc=0（故障注入实测）。有判别力的判据是**下一次
> tick 的秒数**：归零了下一个 tick 必然是 `00:01`，没归零就接着上次的数走。
> 这与约定第 50 条（"没有判别力"的断言要当作没有断言）同源。

### 指纹注册窗体：一条**真实**的 use-after-free，以及"崩溃不能当守卫"

第 37 轮的目标是 `src/ui/form_finger.cpp`（347 行）—— **现场真正执行**的指纹
注册状态机。它与 `FingerprintManager::startEnroll(int)` 是两份**并行实现**，
后者有测试但零生产调用者（见约定第 30 条与不可达槽清单）。第 23 轮已用静态
规则把前者的**命令字节序列**钉住；本轮补上它**行为**上的测试，并因此抓到一个
实打实的内存错误。

#### 缺陷：窗体析构时对已销毁的管理器调用成员函数

`Form_finger` 在注册期间要挂起常驻识别（两者抢同一个串口），退出/析构时成对
恢复，并把"由我挂起的那个管理器"记在成员 `m_suspendedFingerprintManager` 里。

生产对象图（`src/ui/widget.cpp`）：

```
L250  m_fingerprintManager = new FingerprintManager(this);   // 先建
L670  m_fingerForm         = new Form_finger(this);          // 后建（懒建）
```

两者都是 `Widget` 的子对象。**Qt 按创建顺序销毁子对象（先建先死，不是逆序）**
—— 这一点很容易记反，所以本轮专门写了探针实测，并把结论钉进一条单元测试
（`managerIsDestroyedBeforeTheFormProvesTheHazardIsReal`）。于是 `delete Widget`
时管理器**先**析构；等窗体析构函数执行

```cpp
if (m_suspendedFingerprintManager)
    m_suspendedFingerprintManager->setSuspended(false);
```

时那个**裸指针仍然非空**（实测：裸指针非空、对象已死），`if` 判空**挡不住**，
构成实打实的 use-after-free。

触发条件很窄，因此很容易漏测：用户进过指纹注册（记下了句柄）且**没有**点
『退出』就关机 —— 走『退出』会在 `onAddQuitClicked()` 里把句柄清空，因而不触发。
日常手工测试几乎总会先点『退出』。

**修法**：成员类型改为 `QPointer<FingerprintManager>`（被指对象销毁后自动变空），
并 `#include <QPointer>`。与 `widget.h` 中 `m_passwdForm` 用 `QPointer` 同理。

> 与 baseline 的关系：baseline 的 `form_finger.cpp`（267 行）里 **完全没有**
> `suspend` / `setSuspended` / `FingerprintManager` / `m_pollTimer` 等字样
> （本轮逐项 grep 实测）。整套"串口互斥 + 挂起恢复"设计是**重构引入**的，
> 因此这条 UAF 是**重构回归**，不是继承缺陷。这一点决定了修它不违反"行为
> 逐字节兼容"的硬约束：baseline 根本没有这个行为。

#### 关键负结果：**"它会不会崩"不能作为回归守卫**

第一版回归测试写的就是"不点退出、直接让窗体析构，看进程崩不崩"。
故障注入实测（把 `QPointer` 改回裸指针、重编译、重复运行）显示：

- 针对**当时那份较薄的**测试文件：变异体 **5/5 退出码全为 0，一次都没崩**；
- 补强测试（加入整条线上序列断言等，见下）之后：变异体 **8/8 全部**
  `rc=139`（SIGSEGV）或 `0xC0000005`。

原因：use-after-free 属**未定义行为**。Qt 的析构只是把内存还给分配器；
在"刚释放、尚未被复用"的窗口里，那块内存往往仍保存着原来的 vtable 与字段，
`setSuspended(false)` 照常走完 —— 而且 `setSuspended()` 开头有
`if (m_suspended == suspended) return;`，多重放一次也不会产生可见差异。
崩不崩取决于**后续代码是否恰好触碰了那块内存**，所以"靠它崩"是不稳定的守卫。

结论与处理：
1. 把"崩溃"这条路**降级**，不作为唯一守卫；
2. 新增**确定性**的静态规则 `check_form_finger_suspended_manager_is_guarded`
   （`tools/verify.py`），逐字钉住"成员必须是 `QPointer`、必须包含
   `<QPointer>`、挂起/恢复必须成对且分别落在退出与析构两个函数体内"；
3. 单元测试改钉**成因前提**（Qt 确实先销毁先创建的子对象），它顺序一反就红，
   已用故障注入验证有判别力；
4. 在行为夹具的 `UNDETECTABLE` 里记录"这条行为测试抓不到裸指针变异"这一事实
   （并注明补强后已能抓到 —— 记录会随测试强度更新，而不是当成永久结论）。

#### 另一个真实发现：测试被**同一设备上的第二个生产者**污染

`quitStopsThePollTimer` / `destructionStopsThePollTimer` 第一版直接比较
`fakefp::sentCount()` 的增量，结果 rc=1 误报。原因是**真**
`FingerprintManager` 也在用同一个假串口：`startIdentify()` 每 500ms 下发一条
`CMD_GET_IMAGE`（`fingerprint_manager.cpp:118/199`）。而窗体一退出就把常驻
识别**恢复**了，于是计数照涨 —— 断言把管理器的**合法**流量错算成"窗体还在轮询"。

修法是先把常驻识别静音（`setSuspended(true)` + 等一拍 + 抽空事件队列，
使恢复路径的 `singleShot(0,...)` 变成空操作），再观察增量。
这是约定第 52 条（"要断言 X 停下了，必须先把它和同一通道上所有其它生产者的
流量分开"）的又一处实例。

#### 故障注入：24 个变异，先抓到自己的 **5 处假绿**

`tools/verify_form_finger_test.py`（行为）第一轮跑出 5 个未被抓到的变异，
**全部是测试侧的问题**，不是生产代码的问题：

| 假绿变异 | 原因 | 修法 |
|---|---|---|
| 采集前 `GET_IMAGE` 改 `0x03` | 断言只找 `0x02` | 改为整条序列精确比对 |
| 采集超时 `2000`→`500` | `contains(2000)` 被后面的轮询满足 | 改为整个超时序列精确比对 |
| `GEN_CHAR` 不下发 | 断言是"等到出现 `0x02`"，而那正是被删的东西 | 同上（把"等待"变成"断言"） |
| 析构时不停轮询定时器 | "删完再等一会儿看计数"不可判（见上：析构会恢复识别） | 析构前先挂起识别，使流量只可能来自窗体 |
| 析构后不清空句柄 | `setSuspended` 有 `if (m_suspended == suspended) return;`，重放无可见差异 | 判为**不可观测**，改由静态规则守"成对性" |

最关键的一条是**整条序列断言**（`fullEnrollSequenceMatchesThePinnedContract`）：
一次修好三个假绿。它的期望值不是读代码猜的，而是先用**探针**把真实的
`sentPayload` 与 `readTimeouts` 打出来（探针临时加进测试、跑完即删）：

```
01 | 01 02 01 | 01 02 02 | 01 02 03 | 05 | 06 01 12 34
超时 500 | 2000 2000 | 2000 2000 | 2000 2000 | 2000 | 2000
```

顺带纠正了我自己的一处误读：**每一步其实有两条 `GET_IMAGE`** —— 一条来自
"检测手指"轮询（500ms），一条来自 `doFingerCollect()` 再采集一次（2000ms）。
这是与 baseline 一致的既有行为，不是重复下发；静态规则的
`B:0x01 ... B:0x01` 正是此意。我第一版期望序列少写了三条，测试立刻报
"size 11 vs 9" —— 这正是"探针 → 断言"，而不是"读代码 → 断言"的价值。

`tools/verify_form_finger_rule.py`（静态规则）：13 个变异全部被抓到，
3 个对照不误报，还原逐字节一致。

#### 两条"行为上不可观测"的防御性调用，交给静态规则守

故障注入暴露出有两处代码**改坏了也不会有任何行为测试变红**，必须说清楚它们
靠什么守，否则就是无人看守的死角：

| 调用 | 为什么行为不可观测 | 由谁守 |
|---|---|---|
| 析构里的 `stopFingerPoll()` | 轮询定时器以 `form` 为 parent，随窗体一起销毁；析构过程不跑事件循环，所以停不停表看不出差别 | 静态规则 |
| 析构后 `m_suspendedFingerprintManager = nullptr` | 析构只执行一次，而 `setSuspended()` 开头有 `if (m_suspended == suspended) return;`，重复调用无可见差异 | 静态规则 |

它们的**真实**风险在**退出**路径（`close()` 只隐藏窗口、对象仍活着），那条
路径行为测试能抓到。静态规则额外要求"退出与析构两个函数体内都必须有停表与
恢复调用"，于是这两处防御性代码也有确定性守卫 —— 写进 `UNDETECTABLE` 并在
规则里各配一条变异证明它真的守得住。

#### 教训：两个"改写源文件"的夹具绝不能并行

本轮我在行为夹具还在后台跑时又触发了规则夹具，两个进程都在改
`form_finger.h/.cpp`，结果**交叉污染**：规则夹具报"析构时不恢复"没被抓到、
行为夹具报了两条莫名其妙的误报/假绿 —— 而**单独串行重跑时全部正常**。

这与第 36 轮"不要在故障注入扫描期间跑 `verify.py`"是同一类错误，只是这次是
两个扫描器互相踩。修法不是"下次小心"，而是让工具自己拒绝：
两个夹具**共用一把文件锁**（`%TEMP%\fi37_rewrite.lock`，带 pid、能识别陈旧锁），
拿不到锁就立刻退出并说明原因。**别把流程纪律寄托在人的记性上。**

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

配置文件查找顺序（**第 40 轮更正** —— 此前这里写了 4 级，实现只有 2 级，
第 2、3 级根本不存在，照它部署会以为"放同目录就能生效"）：

1. 命令行第一个参数：`./smartpro /path/to/appconfig.json`
2. `/etc/accessterminal/appconfig.json`（内置默认值，见 `app_config.cpp` 的
   `kDefaultConfigPath`）

找不到文件、JSON 解析失败或根节点不是对象时，一律**回退到内置默认值并告警**，
不会崩、也不会把整份配置清空。

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

以下七十九条是本工程最容易在重构中被悄悄破坏、又最难靠运行测试发现的约定。
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

### 4. `WA_DeleteOnClose` 的窗口不能用裸指针成员跟踪

设了 `Qt::WA_DeleteOnClose` 的窗口会在关闭瞬间被 Qt 自动销毁。若把它存进
裸指针成员，该成员立刻悬垂，而“密码正确后关窗”这类回调仍会访问它。

`Form_passwd` 的 `m_passwdForm` 因此改为 `QPointer<Form_passwd>`
（对象销毁后自动变空）。注意 baseline 里这段恰好不触发：原代码用了同名
**局部变量**遮蔽成员，所以不悬垂，但代价是密码正确后窗口关不掉——
重构把它改成成员时修掉了那个功能缺陷，同时引入了悬垂。

由 `check_deleted_on_close_members` 强制。

### 5. 轮询定时器必须有可停止的句柄

`X = new QTimer(...)` 之后若 `X->start(...)`，它就变成了周期定时器。
若 `X` 只是局部变量（不是成员、也没放进容器），则除回调自身外没人在外部
能停止它。此时若承载它的窗口只是被 `close()` 隐藏（未设
`WA_DeleteOnClose`，对象仍存活），这个定时器就会永远每 N 毫秒在 GUI 线程
上跑一次。

`Form_finger::startFingerRegisterStep()` 里的“检测手指”轮询正是如此：
回调中 `readPacket(500)` 是**阻塞串口读**，用户点『退出』后 `close()` 只隐藏
窗口，于是每 500ms 阻塞一次界面。现已改为成员 `m_pollTimer`，并提供
`stopFingerPoll()` 在退出与析构时调用。

与之配套的还有延时重启的作废：分步采集之间是
`QTimer::singleShot(1500, ...)`，只停周期定时器不够——用户若在 1.5 秒内退出，
已排队的 `singleShot` 仍会把轮询重新拉起来。因此 `stopFingerPoll()` 会
`++m_pollGeneration`，延时回调携带发起时的代数，不匹配即放弃
（与第 2 条的 `m_callGeneration` 同一手法）。

由 `check_unqueued_poll_timers` 强制。该检查**只覆盖 QWidget 子类**——
像 `FingerprintManager` 这种随程序存活的 QObject，其常驻轮询是设计意图，
不算“界面空转”类缺陷。

但“不算缺陷”只对界面卡顿成立：常驻轮询器**仍然必须持有句柄**，理由见第 8 条
（串口互斥）。两条规则互补，不要只看这一条。

### 6. 收 UDP 包必须检查 `readDatagram()` / `recvfrom()` 的返回值

先 `resize(pendingDatagramSize())` 再 `readDatagram(...)` 的写法里，**返回值不能丢**：

```cpp
QByteArray datagram;
datagram.resize(socket->pendingDatagramSize());
socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
```

两个后果：

1. `QByteArray::resize()` **不清零**新分配的字节。若实际读到的字节数少于
   `pendingDatagramSize()`，尾部就是未初始化内存（或上一轮循环的残留字节）。
   音频路径下它会被 append 后直接喂给声卡（噪声）；视频路径下会交给
   `loadFromData()` 解码（垃圾数据，甚至解码器崩溃）。
2. 返回 `-1` 表示读取失败，此时 `readDatagram` **不会填充** `sender`/`senderPort`
   出参。音频路径若不判就赋值，会把“回传目标地址”改成未初始化（或上一次的）
   地址，导致后续上行语音全部发错地方。

注意 `pendingDatagramSize()` 返回 `qint64` 而非 `int`。

正确写法（`camera_preview_server.cpp` 采用）：

```cpp
const qint64 bytesRead = udpSocket->readDatagram(
    datagram.data(), datagram.size(), &sender, &senderPort);
if (bytesRead < 0) {
    continue;                 // 失败：丢弃该数据报，不要污染出参
}
if (bytesRead < datagram.size()) {
    datagram.resize(static_cast<int>(bytesRead));
}
```

由 `check_datagram_read_returns` 强制（覆盖 `readDatagram` 与 `recvfrom`）。

### 7. `connect()` 的第二个实参必须是信号

Qt5 的 PMF 版 `connect` 会把第二个实参（成员函数指针）交给
`QObject::connectImpl()`，后者用 `QMetaObject::IndexOfMethod()` 反查信号索引，
而 **moc 只为 `signals:` 区声明的成员生成信号索引**（槽与普通成员索引为 `-1`）。
因此把一个槽或普通成员当作信号源去连接，编译能过、运行期必然失败：

```
QObject::connect: signal not found in Form_finger
```

`connect` 返回 `false`，**对应的槽永远不会被调用**——那段业务逻辑是死代码，
而且不会有任何崩溃或报错提示，极难发现。

本项目曾真实存在这样一处（继承自重构前的旧代码）：

```cpp
// Form_finger::addFingerRequested() 声明在 public slots: 区，
// 却被当作信号使用 —— 该 connect 从未生效。
connect(m_fingerForm, &Form_finger::addFingerRequested,
        this, &Widget::onAddFingerFromFingerForm);
```

注意区分：`connect(btn, &QPushButton::clicked, this, &Cls::someSlot)` 是**完全正常**
的写法——槽只需可调用，不必是信号。本检查因此**只看第二个实参**。

由 `check_connect_signal_is_signal` 强制：解析每个 `Q_OBJECT` 类的
`signals:` / `slots:` / 访问权限分区，若 `&Cls::name` 中的 `name` 在该类的
非 `signals` 区被显式声明，即报错。继承自 Qt 基类的信号不予判定。

### 8. 共用同一硬件的轮询器必须能被他方暂停

门口机上**常驻指纹识别**与**指纹注册**读写的是同一个串口：

```
识别轮询（FingerprintManager::startIdentify）
    send(CMD_GET_IMAGE)  →  readPacket(500)    ← 抢应答
注册流程（Form_finger::doFingerCollect）
    send(CMD_GET_IMAGE)  →  readPacket(2000)   ← 抢应答
```

两者互为对方之应答源。只要识别轮询在跑，它就会把注册流程正在等待的
`0x02/0x05/0x06` 应答从串口缓冲里取走，注册流程拿到空字节数组、
判为“采集图像失败”静默返回；反过来注册流程也会抢走识别的应答。
这不是“偶发硬件问题”，而是确定性的应答错配。

要互斥就必须能**暂停识别轮询**，而暂停的前提是拿得到定时器句柄。旧实现是

```cpp
void FingerprintManager::startIdentify() {
    static bool waitingFinger = false;      // 函数内 static：跨实例共享
    if (waitingFinger) return;
    waitingFinger = true;
    QTimer* timer = new QTimer(this);       // 局部变量：外部拿不到句柄
    connect(timer, &QTimer::timeout, this, [=]() mutable {
        fingerprint->sendPacket(pkt);
        QByteArray resp = fingerprint->readPacket(500);   // 阻塞读
        ...
    });
    timer->start(500);
}
```

三处独立问题，现已一并修复：

1. **句柄丢失** → 无法暂停（本条的核心理由）。改为成员 `m_pollTimer`，
   并新增 `setSuspended(bool)` 作为互斥入口。
2. **函数内 `static`** → 状态为所有实例共享，且不随实例销毁而复位。
   改为成员 `m_waitingFinger` / `m_enrolling`。
3. **裸堆指针** `int* currentStep = new int(1)` 只在**成功路径** `delete`，
   采集失败/合并失败等提前返回全部泄漏。改用 `QSharedPointer<int>`
   按值捕获进 lambda，任何退出路径都自动释放。

暂停/恢复必须成对发生，且覆盖“用户中途点退出”这条路径：
`Form_finger::startFingerRegisterStep()` 进入时挂起，
`onAddQuitClicked()` 与 `~Form_finger()` 都要恢复，否则门口机的
常驻指纹识别会**永久失效**。析构里也要兜底，因为窗口可能被父窗口直接销毁
而不走 `onAddQuitClicked()`。

由 `check_blocking_poll_has_handle` 强制：找出被 `start(...)` 周期化、
且回调体内含 `readPacket` / `waitForReadyRead` / `waitForBytesWritten` 等
阻塞调用的 `QTimer`，若句柄不是 `m_` 成员（也未放入容器、未设
`setSingleShot(true)`）即报错。

它与第 5 条互补：第 5 条只管「QWidget 子类、`close()` 后界面空转」，
本条管「无法与共用同一设备的流程互斥」。二者互不覆盖。

### 9. 数值配置必须在取值出口做范围兜底

配置（JSON 文件、环境变量）是**不可信输入**。环境变量经
`QString::toInt()` 解析时，非法文本会**静默返回 0**；JSON 里也可以直接
写 0 或负数。

重构前 `widget.cpp` 里是硬编码常量：

```cpp
const int detect_interval = 10;   // 每10帧检测一次
```

重构后改成从配置读取，于是这一行变成了可崩溃的代码：

```cpp
if (++detectCounter % cfg.faceDetectInterval() == 0 && ...)
```

只要把 `face_detect_interval` 配成 0，或设置
`ACCESSTERMINAL_BEHAVIOR_FACE_DETECT_INTERVAL=abc`（→ 0），
取模就会触发整数除零（Linux 上是 `SIGFPE`，进程直接崩溃）。

同类风险还有：`frame_interval_ms` 为 0 让 `QTimer` 忙循环、
LBPH 阈值为负导致所有人脸都判定通过、指纹起始 ID 超过 2 字节线格式。

修法是在**取值出口**统一兜底（`app_config.h` 的 `clampInt()` /
`clampDouble()`），这样所有调用方一次性被覆盖，且合法值（默认值与示例
配置）经兜底后不变，与重构前行为一致：

```cpp
int faceDetectInterval() const { return clampInt(m_faceDetectInterval, 1, 100000); }
```

由 `check_config_value_sanity` 强制，并且它自己也有故障注入自检
（`tools/verify_config_sanity.py`，6/6 用例）——静态规则写错会静默失效，
必须证明它真的能区分好坏代码。

### 10. 配置里公开的开关必须真的生效

第 9 条管的是「配置值不合理会崩」，本条管的是「配置值根本没人用」。

`config/appconfig.json` 提供了 `behavior.face_confidence_threshold`，
`app_config.h` 也提供了对应访问器，但 `FaceManager::recognize()` 里仍然是
重构前的硬编码：

```cpp
bool success = (predictedLabel == 1 && confidence < 90);   // 配置改了没用
```

现场把该阈值从 90 调到 60，行为**毫无变化**。这类「假旋钮」不崩溃、
不告警，只让调参永远无效，是最难发现的一类重构退化。

修法是把阈值真正接进 `FaceManager`，并把默认值定为 `90.0`，与重构前的
硬编码常量完全一致 —— 因此不配置时行为逐字节不变：

```cpp
m_faceManager->setConfidenceThreshold(cfg.faceConfidenceThreshold());
```

同时消掉了一处会导致静默错位的重复常量：`widget.cpp` 原本另有
`kFaceSampleTarget = 3`，与 `FaceManager::collectTarget = 3` 各改各的就会
出现「界面永远不触发训练」或「样本已满仍继续采集」。现改用
`FaceManager::sampleTarget()`，全工程只有一个事实来源。

由 `check_config_knobs_are_used` 强制，自检见
`tools/verify_config_knobs.py`（6/6 用例）。判据是「访问器在
`app_config.*` 之外被调用，**或**其成员在加载代码之外被读取」——
后一条是为了不误伤 `pcmDir()` 这种「访问器没人调、但 `pcmFile()` 内部
直接读了 `m_pcmDir`」的有效配置。特别地，**加载代码里的读取不算消费**：
`m_x = jsonInt(o, "x", m_x)` 只是回退实参，若把它算作消费，假旋钮就会被
漏掉（本条规则第一版正是这样漏掉了 `faceConfidenceThreshold`）。

### 11. 指纹/密码等凭据不得写入日志

门口机是**实体门禁设备**，门禁密码即物理准入凭据。重构前的密码校验路径
把用户输入和**已存储的正确密码**一并打进 `qDebug`：

```cpp
qDebug() << "[PasswordManager] checkPassword: input=" << input
         << ", password=" << password
         << ", inputHex=" << input.toUtf8().toHex()
         << ", pwdHex=" << password.toUtf8().toHex();
```

输入窗体还逐位记录按键、并打印整串输入：

```cpp
qDebug() << "[passwd] Digit clicked:" << btn->text();
qDebug() << "[passwd] OK clicked, inputBuffer=" << inputBuffer << ...;
```

后果是**任何一次密码校验（含输错）都会把门禁密码明文写进日志**。嵌入式
Linux 上 `qDebug` 落到 stderr / systemd journal，可被任何有日志读取权限的
人拿到，且跨重启留存；逐位日志与整串日志叠加还能完整还原输入过程。

修法是只保留非内容信息 —— 校验结论与输入长度：

```cpp
const bool matched = (input == password);
qDebug() << "[PasswordManager] 密码校验完成，结果:" << (matched ? "通过" : "拒绝");
emit passwordChecked(matched);          // 比较逻辑与信号语义完全不变
```

由 `check_no_secret_in_logs` 强制，自检见 `tools/verify_secret_logs.py`
（5/5 用例）。规则要求日志语句中不得把凭据标识符、**被当作密码传递的
变量**（如 `emit passwordEntered(inputBuffer)` 里的 `inputBuffer`）、或
密码输入界面的按键文本作为被输出的值；同时放行两类反例以免误伤 ——
敏感词仅作日志前缀标签（`"[passwd]"`），以及只取 `.size()` /
`.isEmpty()` 等非内容信息。注意 `passwordChecked(matched)` 里的 `matched`
是 bool 结果而非密码，故标记凭据时已排除 `checked`/`result`/`success`
这类结果型名字。

### 12. `run()` 内不得重置停止标志

`CameraPushThread` 的停止标志初值为 `false`，并在 `run()` 开头无条件
`running = true`：

```cpp
CameraPushThread::CameraPushThread(...) : ..., running(false) {}
void CameraPushThread::run() { running = true; ... while (running) { ... } }
```

而调用方是「先清理旧的、再 new + start 新的」：

```cpp
m_cameraPushThread = new CameraPushThread(...);
m_cameraPushThread->start();
```

从 `start()` 到 `run()` 真正执行之间存在时间窗口。若期间调用 `stop()`
（用户瞬时挂断），`running` 被置 `false`，随后 `run()` 又把它写回 `true`
—— **停止请求被吞掉**。线程于是继续长时间推流，导致挂断流程
`wait(3000)` 超时、`delete` 一个仍在运行的 `QThread`（Qt 打印
`QThread: Destroyed while thread is still running` 并终止进程），
且该线程继续向已关闭的 socket `sendto`。

正确做法是把停止标志初值置 `true`、`run()` 内不再重置，使 `stop()` 成为
**不可逆**操作 —— 这正是同工程 `AudioSenderThread` /
`AudioReceiverThread` 已有的写法：

```cpp
CameraPushThread::CameraPushThread(...) : ..., running(true) {}
void CameraPushThread::run() { /* 不再写 running = true */ ... }
```

由 `check_no_running_reset_in_run` 强制，自检见
`tools/verify_stop_flag.py`（6/6 用例，含「重置的是非停止标志」与
「非 QThread 类」两个反例）。

### 13. `signals:` 里声明的信号必须真的会被发射

一个只声明、从不发射的信号是**死信号**：任何 connect 到它的槽永远不会被
调用，而编译、链接、静态检查全都不会报错。

本工程的实例是 `AudioSenderThread::connectionLost()`。它被声明、被连接，
connect 的注释还明确写着用途——

```cpp
// 采集线程报告连接丢失时立即挂断，避免通话卡死。
connect(m_audioSenderThread, &AudioSenderThread::connectionLost,
        this, &Widget::hangUpCall);
```

——但全工程从未 `emit` 过它（基线的 `audiosenderthread.cpp` 同样只 emit
`errorMsg`/`finishedMsg`）。于是 `sendto` 失败后采集循环 `break`、线程结束，
再无任何帧发往室内机，通话已单向静音，界面却依旧显示“通话中”，用户只能
手动挂断。这条“避免卡死”的兜底实际是死的。修法是在四个真实的失败出口
（`inet_pton` 失败、`snd_pcm_open` 失败、非 EPIPE/EAGAIN 的读错误、
`sendto` 失败）都补上 `emit connectionLost();`。

判定一个信号“真的会触发”只有两种途径：

1. 被显式发射：`emit S(...)` 或直接调用 `S(...)`；
2. 作为 connect 的**第 4 个实参**出现（信号→信号链式连接，
   `connect(btn, &QPushButton::clicked, this, &Cls::S)`）——源信号触发时
   Qt 会自动发射 S。

第 2 条不能漏：漏掉就会把 `Form_finger::addFaceRequested` 这种链式目标
误判为死信号。反过来也要注意，`connect(this, &Cls::src, this, &Cls::dst)`
里的 `src` 是**源**，它自己必须另有 emit 出口，否则整条链依然永不触发。

由 `check_declared_signal_is_emitted` 强制，自检见
`tools/verify_dead_signal.py`（10/10 用例，含“链式连接的源信号本身是死的”
这一真阳性）。

### 14. connect 实参必须能正确切分（`->` 陷阱）

第 7 条（`connect` 第二实参必须是信号）依赖“按顶层逗号切分实参”。而本工程
最常见的写法首实参就含 `->`：

```cpp
connect(ui->addface, &QPushButton::clicked, this, &Form_finger::addFaceRequested);
```

若切分器把 `>` 也当作需要配对的括号（`if ch in "(<[" / elif ch in ")>]"`），
每遇到一个 `->` 深度就偏负一次，**顶层逗号再也分不出来**，整条实参列表被当成
1 个实参；检查随后因 `len(args) < 2` 直接 `continue`，**静默跳过这条
connect**——既不报错也不提示。

实测本工程因此漏检了 6 条本应检查的 connect（33/39），且漏掉的恰好都是
`ui->控件` 开头的常见形式：

```
form_call.cpp:27    ui->btnstop, ...
form_finger.cpp:37  ui->addface, ...
form_finger.cpp:39  ui->addfinger, ...
form_finger.cpp:41  ui->addquit, ...
form_passwd.cpp:46  ui->btnbck, ...
form_passwd.cpp:48  ui->btnok, ...
```

现在切分只让 `()`/`[]`/`{}` 参与配对，`->` 与 `>>` 不计数。由
`tools/verify_connect_signal.py`（5/5 用例，含“首实参含 `->` 且第二实参是槽”
的真阳性）锁定，确保第 7 条不再被悄悄绕过。

### 15. 异步播放任务必须全部等待，不能只留最后一个

`AudioManager::playAsync()` 每次播放都覆盖同一个成员：

```cpp
stop();                                     // 置旧标志
interruptFlag = std::make_shared<std::atomic<bool>>(false);
playFuture = QtConcurrent::run([=]() { ... 捕获 this ... });
```

`QFuture` 赋值**不会等待**上一个任务（只转移内部 d 指针的引用，`~QFuture()`
也不阻塞）。所以上一次播放若尚未结束，它就变成"被丢弃但仍在运行"的任务：

- 它捕获的 `flag` 已不再是 `interruptFlag` 指向的对象，`stop()` 再也打不断
  它 —— 会把整段提示音播完，并继续持有静态 `pcmMutex`，令后续提示音排队；
- 析构只 `waitForFinished()` **最后一个** future，旧任务完全不被等待，于是
  `~AudioManager()` 返回后它仍在 `emit` 信号、读成员 —— use-after-free。
  在门口机上表现为关机/退出时的偶发崩溃。

修法是把所有未完成任务收集起来，析构时逐个等待：

```cpp
QVector<QFuture<void>> playFutures;   // 成员
void waitForAllTasks();               // 析构中调用（先 stop() 再等）
```

`playAsync()` 回收已完成项后把新 future 一并登记；`waitForAllTasks()` 在
`stop()` 之后逐个 `waitForFinished()`。顺序不能颠倒：仅等待而不置标志会让
析构阻塞到整段提示音播完。

由 `check_abandoned_async_tasks` 强制，自检见
`tools/verify_abandoned_task.py`（5/5 用例，含"收集未完成任务"与
"即发即忘 + QPointer 守卫"两个反例）。注意**不误伤即发即忘的用法**：像
`widget.cpp` 的人脸检测那样不把结果赋给任何变量、且自带 `QPointer` 守卫的
任务不存在"被覆盖丢弃"的问题。

> 第 42 轮澄清（别把两条规则混为一谈）：上面这条只管**对象生命周期**
> （会不会在已析构对象上跑），因此它**有意**放过即发即忘的任务。
> "即发即忘"还有**另一个**独立风险 —— 任务在无界队列里**堆积**，
> 那是约定 §77 的范围，由 `check_latest_value_handoff` 守。
> 人脸检测曾同时踩两条：它当时有 `QPointer` 守卫（UAF 无碍），
> 却没有背压闸门（堆积有碍）。一条规则绿不等于另一条也绿。

### 16. 可独立构建的子工程 CMake 必须自包含

`tests/CMakeLists.txt`、`tests/interop/CMakeLists.txt` 这类**既能作为主工程的
子目录、又能被单独 `cmake -S tests`** 的文件，必须自带：

| 必须有 | 缺了会怎样 |
|---|---|
| `cmake_minimum_required()` | 配置阶段直接失败：`No cmake_minimum_required command is present` |
| `project()` | 同上，CMake 要求顶层脚本必须有 `project()` |
| `enable_testing()` | **配置成功、构建成功，`ctest` 报 `No tests were found!!!`** |
| `set(CMAKE_AUTOMOC ON)` | 测试文件里 `#include "test_xxx.moc"` 找不到 |

作为子目录时这几行是幂等的（主 CMakeLists 已提供），所以对两种用法都安全。

第三种最危险：构建退出码为 0，看起来一切正常，**实际一个用例都没执行**。
在有 ALSA 的机器上用主工程构建时 `enable_testing()` 由顶层提供，缺陷被掩盖；
只有在没有 ALSA 的开发机上走"独立构建"这条唯一可行的路时才会暴露 —— 而
那条路此前从未被真正跑通过。

用 `check_subproject_cmake_is_selfcontained` 强制，自检见
`tools/verify_subcmake.py`（8/8 用例，含"缺 enable_testing""缺 AUTOMOC 但
无 `.moc` include（真阴性）"等）。

### 17. `fromHex()` 的输入必须先逐字符校验

`QByteArray::fromHex()` **不会**因为遇到非法字符而失败 —— 它会**跳过**非法字符，
把剩下的合法字符**重新两两配对**。已用真实 Qt 5.15.19 实测确认：

| 输入（偶数长度） | fromHex 结果 | 说明 |
| --- | --- | --- |
| `FD034C12045DDF` | `fd 03 4c 12 04 5d df` | 正确 |
| `FD0G4C12045DDF` | `0f d0 4c 12 04 5d df` | **内容全错** |
| `XX034C12045DDF` | `03 4c 12 04 5d df` | **错位，少一字节** |

只要字符数是**偶数**，解码就总是"成功"，`!bytes.isEmpty()` 完全拦不住。
这个缺陷是在**室内机**（Accessterminal_desktop）的灯控指令解析里发现并修复的；
本工程当前不使用 `fromHex`，本条作为**回归防护**存在 —— 门口机同样有串口
外设（指纹模块），一旦将来引入十六进制配置解析，这种写法会立刻被拦下。

正确写法：

```cpp
if (compact.isEmpty() || compact.size() % 2 != 0) return QByteArray();
if (!isHexOnly(compact)) return QByteArray();   // 关键，否则上面那条守卫形同虚设
return QByteArray::fromHex(compact.toLatin1());
```

由 `check_hex_parse_is_validated` 强制：把**变量**传给 `fromHex` 而未先做
十六进制校验即报错（传给 `fromHex` 的**字面量**不受影响）。
自检见 `tools/verify_hex_parse.py`（5/5）。室内机侧另有可执行断言
（`tests/test_app_config.cpp`）用真实 Qt 跑出上表结果，把该语义钉死。

### 上行音频（AudioSenderThread）的线上字节断言

`AudioSenderThread` 把门口机的麦克风音频写进 UDP 12345。这条通路的
**字节格式是协议硬约束**：裸 PCM、S16_LE、16000 Hz、单声道、每包 320 帧
（**640 字节**）、**无包头**。室内机按包直接喂给 ALSA，所以包长或字节序
错一点，现场只表现为"声音不对/有杂音"，**不报任何错误**。

它此前从未被真正验证过，原因很具体：`tools/devstubs` 里的 `sendto()` 只是
"返回 len"的桩，**既不记录载荷也不校验格式**；更要命的是它的 `inet_pton()`
**永远返回 0**，于是 `run()` 在建连阶段就 `return`，**根本走不到发送代码**。
也就是说，这条通路在主机端测试里一直是"看起来测过、其实一步没跑"。

`tests/test_audio_sender.cpp` 因此改用两份可记录/可注入的假替身：

- `tests/fake_net.cpp` —— 记录每次 `sendto()` 的 fd、目标地址、目标端口与
  **完整载荷**，使断言可以针对"线上到底是什么"；
- `tests/fake_alsa.cpp` —— 已支持采集方向（`readi`）的 `-EPIPE` / `-EAGAIN` /
  不可恢复错误注入。

该目标**不能**同时链接 `devstubs/stub_impl.c`（两边都定义 `sendto`/`inet_pton`），
故 `tests/CMakeLists.txt` 里是一套独立源文件列表。

| 用例 | 钉住的行为 |
| --- | --- |
| `uplinkPacketsMatchProtocolExactly` | 每个数据报正好 640 字节裸 PCM，无包头 |
| `packetSizeIsFrozenConstant` | 16000 / 单声道 / 320 帧 / 端口 12345 冻结；ALSA 侧同为 S16_LE |
| `sendsToConfiguredAddressAndPort` | 发往参数指定的 IP 与端口，且用**外部传入**的 fd |
| `noPacketHasAHeader` | 载荷首字节必须是 PCM 本身，不是长度/魔数前缀 |
| `underrunRecoversWithoutLosingAudio` | 采集 XRUN 后 `prepare` 并继续，不丢后续音频 |
| `eagainDoesNotAbortCapture` | `-EAGAIN` 只是"暂时没数据"，必须重试而非结束 |
| `fatalReadErrorEmitsConnectionLost` | **不可恢复读错误必须发 `connectionLost`**（第 11 轮回归） |
| `openFailureEmitsConnectionLost` | 打不开设备 -> 挂断，且一个包都不发 |
| `badIpEmitsConnectionLost` | `inet_pton` 失败 -> 挂断 |
| `sendFailureEmitsConnectionLost` | `sendto` 失败 -> 挂断（这条兜底路径曾经是**死的**） |
| `finishedSignalAlwaysFires` | 正常与出错两条路径都必须发 `finishedMsg` |
| `stopEndsThreadPromptly` | `stop()` 后能及时退出，不卡在采集循环 |

对照实验（还原修复后测试必须失败）确认了区分度：

| 还原的修复 | 结果 |
| --- | --- |
| 删掉全部 `emit connectionLost();`（4 处） | 4 个 `*EmitsConnectionLost` 用例**全部失败** |
| 去掉 underrun 后的 `snd_pcm_prepare()` | `underrunRecoversWithoutLosingAudio` **失败** |
| 还原原文件 | 全部通过 |

> 关于时序：线程启动 + `snd_pcm_open` + EAGAIN 路径的休眠叠加起来有几十毫秒
> 抖动。用固定 `qWait(80)` 会写出**偶发失败**的测试——这里踩过一次：首个包
> 实测在约 80 ms 才发出，断言恰好落在边界上。因此所有"应该有包发出"的用例
> 一律用 `waitForPackets()` 等**性质**成立，而不是等固定时长。连续运行 5 次
> 均稳定（14 passed）。

### 下行音频（AudioReceiverThread）的播放行为断言

接收线程决定 UDP 12345 下行的**播放行为**：裸 PCM、S16_LE / 16000 / 单声道、
每包 320 帧 = 640 字节、**无包头**，且线程**只在 `accept` 之后才创建**。

与发送线程同样的处境：它此前也**从未被真正执行过**。`tools/devstubs` 的
`select()` 恒返回 0、`recvfrom()` 恒返回 -1，于是 `run()` 里两个循环体
**一次都没跑过**。补上可执行测试后立刻暴露了两个真实缺陷（第 18 轮修复）：

1. **`-EAGAIN` 被当成致命错误。** 设备以 `SND_PCM_NONBLOCK` 打开，所以
   `snd_pcm_writei` 返回 `-EAGAIN`（播放缓冲满）是**必然可达**的正常情况。
   原实现只特判 `-EPIPE`，其余负值一律 `emit errorMsg` 并结束线程——现场表现是
   **通话中途突然单向无声**，而 `errorMsg` 只打日志，界面毫无察觉。
2. **错误路径不再结束线程。** baseline 是 `break` 出 `while(running)`，
   语义即"线程结束"；重构后的 `goto receive_done` 只退出**内层**循环，外层
   立刻重新 `select`/`recvfrom`，把同一个持久性错误无限重复。

`tests/test_audio_receiver.cpp` 用同一套假替身（`fake_alsa` 补齐 mixer 与
写方向注入；`fake_net` 补齐 `recvfrom`/`select`/`fcntl` 与待收数据报注入）：

| 用例 | 钉住的行为 |
| --- | --- |
| `playedPacketsMatchProtocolExactly` | 每包正好 640 字节，且内容逐字节不改动 |
| `negotiatesProtocolPlaybackFormat` | 协商为 S16_LE / 16000 / 单声道 |
| `playsOnlyAfterDataArrives` | 没数据时一个字节都不写 |
| `zeroLengthDatagramIsIgnored` | 零长数据报被忽略，不产生空写 |
| `startupDiscardsStaleBacklog` | **启动前堆积的包必须被丢弃**（见下方时序说明） |
| `underrunRecoversAndKeepsPlaying` | XRUN 后 `prepare` 并继续播放 |
| `eagainOnWriteIsNotFatal` | **`-EAGAIN` 必须继续播放**（第 18 轮回归） |
| `fatalWriteErrorEmitsErrorAndStops` | 不可恢复写错误发 `errorMsg` **并结束线程** |
| `recvErrorEmitsConnectionLost` | `recvfrom` 真错误发 `connectionLost` **并结束线程** |
| `eagainOnRecvIsNotConnectionLost` | 收包 `-EAGAIN` 只是"暂时没数据"，不算断开 |
| `eintrOnRecvRetries` | 收包 `-EINTR` 重试，不丢包 |
| `openFailureEmitsErrorAndReturns` | 打不开设备 -> 报错返回 |
| `stopEndsThreadPromptly` | `stop()` 后能及时退出 |
| `volumeIsClampedAndApplied` | 音量按 [0,100] 夹取后写入 mixer |
| `volumeMissingControlIsSafe` | mixer 里找不到 "Playback" 控件时安全返回 |
| `volumeMixerOpenFailureIsSafe` | mixer 打不开时安全返回，不影响播放 |

对照实验（还原修复后测试必须失败）确认了区分度：

| 还原的修复 | 结果 |
| --- | --- |
| 去掉 `recvfrom` 错误处的 `running = false` | `recvErrorEmitsConnectionLost` **失败** |
| 去掉 `writei` 错误处的 `running = false` | `fatalWriteErrorEmitsErrorAndStops` **失败** |
| 去掉 `-EAGAIN` 分支（落进一般错误） | `eagainOnWriteIsNotFatal` **失败** |
| 还原原文件 | 全部通过 |

就"重复发 `connectionLost` 会反复挂断"这一点单独做了探针（600 ms 窗口）：

| 状态 | `connectionLost` 次数 | 线程仍在运行 |
| --- | --- | --- |
| 修复在位 | **1** | 否 |
| 还原修复 | **3410** | 是 |

> 关于时序：`run()` 开头有一段**丢弃启动前堆积包**的循环。因此测试**不能**在
> `start()` 之前投递数据——那些包会被丢弃阶段吃掉，症状是 `writeiCount` 恒为 0
> （踩过一次）。所有用例统一走 `startFeedAndWait()`：`start()` -> `qWait(80)`
> 越过丢弃阶段 -> 投递并等待性质成立。
>
> 另一个坑：QtTest 的 `QVERIFY` 失败会直接 `return`，若线程仍在运行，析构时
> 会触发 `QThread: Destroyed while thread is still running` 并 abort，把真正的
> 失败原因淹没。故用 RAII `ThreadGuard` 保证任何路径都 `stop()` + `wait()`。

### 视频推流（CameraPushThread / UDP 23456）的线上字节断言

UDP 23456 是协议硬约束的一员：**每个数据报就是一个完整 JPEG**（质量 75），
不做分片重组、不做重传，端口固定 23456。它此前**完全没有测试**，与刚修过
缺陷的两个音频线程属于同一类问题——`tools/devstubs` 的 `sendto()` 只"返回
len"、不记录载荷，其 `inet_pton()` 还永远返回 0，而真实 `V4L2Camera` 需要
`/dev/video0`，开发机上都不存在。

因此这里用两件替身把**真实网络路径**跑起来：

- `tests/fake_net.cpp` —— 记录每次 `sendto()` 的 fd/目标地址/目标端口/完整
  载荷（本轮补上了 `socket()`，因为本线程自己建 socket）；
- `tests/fake_v4l2_camera.cpp` —— `V4L2Camera` 的**链接期替身**。
  `getFrame()` 不是 virtual，无法继承覆盖，所以只能替换链接时的符号定义；
  好处是**生产代码一行都不用改**（不加任何测试钩子）。

> AUTOMOC 陷阱：替身顶掉了 `v4l2_camera.cpp`，就没有任何源文件与
> `v4l2_camera.h` 同名，AUTOMOC 不会为它生成 moc，链接期报
> `undefined reference to 'vtable for V4L2Camera'`。把真实的
> `v4l2_camera.h` 显式列进 `add_executable` 的源文件列表即可。

| 用例 | 钉住的行为 |
| --- | --- |
| `eachPacketIsAWholeJpegWithoutHeader` | 第 0 字节就是 JPEG 的 SOI（FF D8），末两字节是 EOI（FF D9），且能解回原尺寸 —— "无包头"的直接证据 |
| `wireJpegMatchesExplicitQuality75` | 线上字节与本地 `save(..., "JPEG", 75)` **逐字节相同**（质量改动会静默改变带宽/画质） |
| `sendsToConfiguredAddressAndVideoPort` | 发往指定 IP 与固定端口 23456 |
| `videoPortIsFrozenAndDistinct` | 8887 / 12345 / 23456 / 50000 四个端口互不重叠 |
| `noFrameMeansNoPacket` | 没有可用帧时**不发空包**，但仍持续取帧 |
| `everyFrameIsPushed` | 多帧都发出、都发往同一目标 |
| `socketFailureEmitsNothing` | `socket()` 失败时干净退出，一个包不发、一次帧都不取 |
| `stopEndsThreadPromptly` | `stop()` 及时生效（40ms 分片睡眠的作用） |
| `stopBeforeStartIsNotLostByRun` | 停止标志不可逆（第 10 轮真实缺陷回归） |

### 远程监看（CameraUdpServer / UDP 50000）的线上语义断言

UDP 50000 与 23456 的差异**全是线上语义**，改错只在现场表现为"看不到画面"，
不报任何错：只在收到裸 `getframe` 后才推帧、收到 `stop` 立即停止、
50 ms 定时器 = 20 fps、JPEG 用 `save(&buffer, "JPEG")` 默认质量。

本目标刻意使用**真实 `QUdpSocket`** 走 `127.0.0.1:50000`，把 bind、事件循环、
`readyRead` 派发一起验证，而不是直接调私有槽（那会绕开真正的线上路径）。
客户端发的是**裸文本**（不带 `'\n'`，与 TCP 8887 的信令不同）。

| 用例 | 钉住的行为 |
| --- | --- |
| `noPushBeforeGetFrame` | 没收到请求时一个字节都不推，**也不取帧**（"不长期占用带宽"的核心保证） |
| `getFrameStartsPushingWholeJpegs` | 裸 `getframe` 后开始推完整 JPEG（无包头、可解码） |
| `getFrameWithNewlineIsAccepted` | 带 `'\n'` 也接受（`fromWire` 会 trim），但裸文本才是线上形态 |
| `stopHaltsPushing` | `stop` 后不再推帧 |
| `getFrameAfterStopResumesPushing` | **停止后再次 `getframe` 能恢复推帧**（见下方对照实验说明） |
| `unknownDatagramIsIgnored` | 无法识别的文本被忽略，且不影响后续请求 |
| `noFrameMeansNoPacket` | 没有可用帧时不发空包 |
| `wireJpegMatchesExplicitQuality75` | 线上字节等于显式质量 75 的编码结果 |
| `previewJpegMatchesCallVideoQualityOnQt5` | 固化"Qt5 默认质量 == 75"这一实测事实 |
| `previewPortIsFrozen` | 端口固定 50000，且与其余三个端口不同；裸信令文本为 `getframe` / `stop` |

#### 一个被澄清的"差异"

预览用 `save(&buffer, "JPEG")`（默认质量），通话视频用 `save(&buffer, "JPEG", 75)`
（显式 75）。两者**看起来**质量参数不同，但实测（Qt 5.15.19，探针遍历质量
1..100 逐一比对字节）Qt 的**默认 JPEG 质量恰好就是 75**，因此二者输出逐字节
相同。我最初想当然地断言"两者必然不同"，被实测当场证伪。该事实现已固化为
断言：若将来 Qt 改变默认质量，或有人改掉显式的 75，测试会立刻失败。

#### 对照实验暴露的两个真实覆盖盲区

| 对照（还原被钉住的行为） | 首次结果 | 结论与处置 |
| --- | --- | --- |
| 生产代码质量 75 -> 1 | **测试仍全过** | 旧断言只校验 JPEG 魔数与可解码性，**不校验质量**；已新增逐字节质量断言 |
| 去掉 `sending = false;` | **测试仍全过** | 该赋值对"停止"是冗余的（`sendTimer->stop()` 已足够，且 `sendFrame()` 开头的 `if (!sending) return;` 使其线上不可见）；它真正的作用在**恢复**路径 —— 少了它，`stop` 后再次 `getframe` 时 `if (!sending)` 为假、`sendTimer->start()` 不被调用，**第二次监看永远看不到画面**。已新增 `getFrameAfterStopResumesPushing` |
| 去掉 `sendTimer->stop()` | 线上不可见 | 同上，冗余但保留（防御性） |

> 教训：**"有断言"不等于"有区分度"**。上表第二行正是我先写错了对照用例
> （选了"什么都不发"的场景，根本进不了分支），换到正确用例后才暴露出来。
> 最终 6 项对照 18/18 全部符合预期。

### 提示音播放（AudioManager）的可编程测试

`AudioManager` 是本工程出过**两个真实缺陷**的类（第 11 轮 XRUN 静默丢数据、
第 12 轮异步任务漏等待），而它的危险分支全在"设备报错 / 被打断 / 并发"上——
真实 ALSA **无法按需制造 -EPIPE**，也无法让写入变慢以观察打断时机。

因此 `tests/test_audio_manager.cpp` 链接一份**可编程的假 ALSA**
（`tests/fake_alsa.cpp`，直接 include `tools/devstubs/alsa/asoundlib.h`，
故每个函数签名都由头文件在编译期校验）。它用失败注入把那些从未被执行过的
分支真正跑了一遍（13 个用例）：

| 用例 | 钉住的行为 |
| --- | --- |
| `normalPlaybackWritesEveryChunkAndDrains` | 每块都写出、收尾用 drain、内容不错位 |
| `negotiatesProtocolAudioFormat` | 协商为 S16_LE / 16000 / 单声道（与协议一致） |
| `recoversFromXrunWithoutLosingData` | **XRUN 后 prepare 并重写同一包**（第 11 轮回归） |
| `survivesXrunBeyondRetryLimit` | 重试超限后仍继续处理后续块，不提前结束 |
| `fatalWriteErrorDoesNotAbortRemainingChunks` | 不可恢复错误只告警，不跳过剩余提示音 |
| `interruptDropsInsteadOfDraining` | 被打断时 drop（丢弃尾巴），不是 drain |
| `nextPlaybackInterruptsPreviousTask` | 上次播放**可以**被后续 `playAsync()` 打断 |
| `destructorWaitsForAllRunningTasks` | 析构返回时句柄已配平、不再有新任务开始 |
| `openFailureIsHandledGracefully` | 打不开设备：只告警，不崩溃、不多余 close |
| `missingFileIsHandledGracefully` | 文件不存在：**已打开的句柄必须关闭** |
| `playbackFinishedFiresEvenWhenInterrupted` | 被打断时 `playbackFinished` 仍会发射 |

对照实验（还原修复后测试必须失败）确认了区分度：去掉 XRUN 重试会让
`framesWritten` 立刻变少；`stop()` 不置位或忽略中断标志则 3 个打断类用例
同时失败。**唯一例外**是析构用例：把它还原成"只等最后一个 future"后**仍然
通过**，因为 `playPcmFile()` 入口的静态 `pcmMutex` 把各任务完全串行化，
"只等最后一个"往往传递性地也等到了更早的任务。该用例因此只断言确实成立的
性质，并在代码注释里写明这一局限——不留一条永远为真的"回归测试"。

> 关于本检查的实现坑：`strip_comments_and_strings()` 会清空**字符字面量**
> （`'0'` → `''`），所以判据**不能**基于 `QChar('0')` / `c >= '0'` 这类写法，
> 只能识别"名字里含 hex 的校验型函数调用"。而且提取 `fromHex` 实参必须
> **按括号配平扫描**——真实写法 `fromHex(compact.toLatin1())` 的实参含嵌套
> 括号，用 `[^()]*` 会完全匹配不到而静默漏判。这两点都是故障注入自检抓出来的。

### 18. 配置载入必须在每次调用开头复位"已载入"状态

配置类通常是**单例**且 `load()` 可被多次调用。若只在成功分支写
`m_loadedFromFile = true`、末尾 `return m_loadedFromFile;`，而
`applyDefaults()` 不复位该标志，则 `load(坏文件)` 会**返回 true**
（上一次成功的残留），`configPath()` 还指向早已失效的旧文件，调用方误以为
配置已生效。

本工程使用**函数局部** `bool fileLoaded = false;`，天然无此问题；本条作为
回归防护存在（室内机侧曾有该缺陷，并已修复）。

### 19. 配置整数转窄类型前必须先夹取

`static_cast<quint16>` 是**取模截断**而不是夹取，实测（真实 Qt）：

| 配置值 | 取模结果 | 应该 |
| --- | --- | --- |
| 65536 | **0** → 随机临时端口 | 65535 |
| 999999 | **16959** | 65535 |
| -1 | **65535** | 1 |

越界配置会变成"能用的错误值"，比直接报错更难发现。必须先 `clampInt` 再转换。
本工程当前无 `quint16` 配置项，本条为回归防护（门口机的指纹 ID 已是 2 字节
线格式，引端口类配置时最容易踩到）。

两条均由 `check_load_state_reset` / `check_narrowing_cast_of_config` 强制，
自检见 `tools/verify_config_state.py`（7/7）。

### 20. 两个仓库的测试目标名必须唯一

门口机与室内机各有 `tests/CMakeLists.txt`，目标名原本都是 `test_protocol`、
`test_app_config`。单独构建都好，但把两个仓库作为子目录放进**同一个** CMake
工程（跨仓库集成，或 IDE 一次性载入）时直接配置失败：

```
add_executable cannot create target "test_protocol" because another
target with the same name already exists.
```

实测：修复前 `configure_rc=1`，两个 `add_executable` 全部失败；给目标名与
ctest 用例名加仓库前缀（`t113_` / `desktop_`）后，超级工程 **5/5 用例通过**。
`add_test(NAME ...)` 的用例名同理——重名会让 `ctest -R` 一次选中两个。

由 `check_cross_repo_test_targets_are_unique` 强制（需传入另一仓库根目录，
见 `tools/verify_cross_target.py`，5/5）：

```bash
python3 tools/verify.py . ../Accessterminal_desktop
```



### 21. 提示音播放的控制流必须有可执行测试（假 ALSA）

`AudioManager` 的危险分支是"XRUN / 被打断 / 并发"，而真实声卡**无法按需制造
`-EPIPE`**、也无法让写入变慢来观察打断时机，所以这些分支长期只靠阅读代码
判断，正是第 11 轮 XRUN 静默丢数据能存活至今的原因。

`tests/fake_alsa.cpp` 提供一份可编程假 ALSA（`setEpipeFirst`、
`setWriteDelayMs`、`setOpenFail`、`setFatalError` …），使 `AudioManager`
**不加任何测试钩子**就能被驱动到每条出错路径。它直接 include
`tools/devstubs/alsa/asoundlib.h`，因此签名与真实 ALSA 一致——不会因为参数
类型不符而在链接期"碰巧能过"。

新增类似"设备出错才走到"的代码时，请同样用失败注入把它跑起来；只做静态
分析会重复第 11 轮的教训。
### 22. 影响线上字节格式的通路必须有针对"线上字节"的断言

`AudioSenderThread` 的教训是：**"有测试"和"测到了"是两件事**。该目标此前
配套的头文件桩让 `inet_pton()` 恒返回 0、`sendto()` 恒成功且不留痕迹，于是
测试即便"绿"着，发送代码一行都没执行过。判断一条通路是否真的被测到，要看
**断言是否落在它的输出上**（这里是 UDP 载荷的字节），而不是看它有没有被链接、
有没有被调用、有没有跑过而不报错。

同理，凡是决定"线上/设备上到底是什么字节"的参数（采样率、声道、每包帧数、
端口、有无包头），都应当有一条**冻结常量断言**，这样将来有人"顺手"改成加个
包头或改小每包帧数时会立刻失败，而不是等到现场听出杂音。

自检由 `tools/verify.py` 的 `check_wire_format_constants_are_pinned` 强制：
`protocol.h` 里决定线上字节格式的常量（采样率、声道、每样本字节数、每包帧数、
四个端口）必须出现在 `tests/` 的源码中。故障注入见
`tools/verify_wire_format.py`（**7/7**），覆盖"测试里完全没有断言""只在注释里
提及""新增线上常量却无人断言"三种真阳性，以及"新增常量且已断言""清单里列了
但 `protocol.h` 中并不存在"两种真阴性。

> 该自检本身也踩过一次坑：第 4 个用例原本通过**修改临时仓库里的 verify.py**
> 来注入探针常量，但 `load()` 导入的是**原始** `VERIFY` 路径，探针根本没进
> 清单，于是这一条"真阳性"实际上什么都没验证却报了 PASS。改成直接 patch
> **已加载的模块**后才真正生效。这正是必须做故障注入的理由——**自检脚本自己
> 也会骗人**。

用 `.clang-format` 统一格式，但**格式不是正确性**——上述五十一条请以
`verify.py` 的结果为准。

---

### 23. 非阻塞打开的 PCM 句柄必须区分 `-EAGAIN` 与真错误

`snd_pcm_open(..., SND_PCM_NONBLOCK)` 打开的句柄，在播放缓冲满（写方向）或
采集缓冲空（读方向）时**必然**返回 `-EAGAIN`。这是**正常且可恢复**的状态，
不是设备故障。

把它当成一般错误处理的后果是**静默**的：`emit errorMsg` 只进日志，界面毫无
反应，现场表现就是"通话中途突然单向无声"（下行）或"声音断断续续"（上行）。
第 18 轮在 `AudioReceiverThread` 上就是这个问题：只特判了 `-EPIPE`，其余负值
一律结束线程。

正确做法：`-EAGAIN`/`-EWOULDBLOCK` 重试或丢弃本包后继续；`-EINTR` 有界重试；
`-EPIPE` 用 `snd_pcm_prepare()` 恢复；**只有其它负值才是真错误**。

`verify.py` 的 `check_nonblock_pcm_handles_eagain` 会挑出"以非阻塞方式打开，
却在同一函数体内完全没有提及 `EAGAIN`/`EWOULDBLOCK`"的读写调用点。它按
**函数体**判定（不是整个文件），自检见 `tools/verify_nonblock_pcm.py`（6/6）。
桌面端当前没有 ALSA 代码，规则同为空转，但已同步过去以防将来加入播放逻辑。

### 24. 图像编码质量属于线上字节，必须被逐字节断言

`QImage::save(&buffer, "JPEG", quality)` 的质量参数**不改变可解码性**，只改变
字节。因此"质量被改动"是一类**静默的线上差异**：带宽与画质变了，接收端照样
能解码，谁也不报错。

只校验"是 JPEG 魔数 + 能解码"是**不够的**。对照实验证实：把通话视频的质量
从 75 改成 1，原有测试**全部仍然通过**。

正确做法是把收到的数据报与本地同一张图、同一个显式质量参数编码出的字节做
**逐字节比较**，并补一条"换一个质量必须产生不同字节"的反向断言，证明该比较
有区分度。注意期望值必须**显式写出质量数**（如 75），不能也用默认质量——
那样比较会退化成"默认 == 默认"，完全不校验编码参数。

### 25. 只写"停止"是不够的，必须断言"停止之后还能重新开始"

状态机类代码里，`stop` 分支常有一行把状态置回初始值的赋值（如
`CameraUdpServer::onReadyRead` 里 `stop` 分支的 `sending = false;`）。
这行赋值对"停止"本身往往是**冗余**的 —— 实测去掉它，停止行为完全不变，
线上字节一模一样，因此**任何只测"停止后不再发"的用例都发现不了它的缺失**。

它真正的作用在**恢复**路径：若停止时没有复位，再次启动的条件判断
（`if (!sending) sendTimer->start();`）就不成立，**第二次使用永远起不来**，
而界面不报任何错。

因此对"有开始/有停止"的通路，必须覆盖完整序列：
**开始 -> 收到数据 -> 停止 -> 再次开始 -> 必须能再次收到数据**。
本轮对照实验正是靠这一条找出了 `getFrameAfterStopResumesPushing` 这个缺口。
### 26. 加了解析路径的"外层守卫"后，必须断言它**独有的**可观测效果

一个流程里同一条件常出现**两次**（入口守卫 + 回调内让路）。只断言"最终没有
产生副作用"时，**两道守卫互为遮蔽**：去掉入口守卫，回调内的守卫仍会在一轮
之内兜住，"没有副作用"照样成立 —— 入口守卫于是成了**未被验证**的代码。

正确做法是找到入口守卫**独有的**那个可观测效果。`startIdentify()` 里它在建
定时器**之前**就发出"请按下指纹"的提示信号；回调内的守卫在 500 ms 后才触发，
**无法**补救这次提示。因此要断言"挂起时连提示都不发"，而不只是"不下发帧"。

第 21 轮的对照实验正是这样发现两道守卫互相遮蔽的（首版 7 个对照项里有 5 个
"测不出来"，全部源于只断言了不下发）。

### 27. 夹具里的取值必须让断言**可能失败**

`STORE_CHAR` 的 id 高字节断言用 `id = 0x0005` 时，其高字节本来就是 `0x00`，
把源码里的 `id >> 8` 改成常量 `0x00` 测试**照样通过** —— 断言形式正确，
但取值让它没有区分度。改用 `0x0105`（两个字节都非零）后立即暴露。

规则：凡是"大端拆分"或"逐字节比较"类的断言，**取值必须让被拆分的各个字节
互不相同且非零**，否则应视为断言无效。同理，夹具里的函数名/宏名要**恰好**
匹配被测规则的匹配模式（本轮另有 `startTimer()` 不匹配 `\bstart\s*\(`
的实例，只因仓库里恰好存在真实文件才没暴露）。

### 28. 断言"最终出现了什么"是不够的，还要断言"在错误的一侧发生之前什么都没出现"

室内机的 `decodersArePerConnection` 首版只断言"最终出现了一条 `Call`"。而
"所有连接共用同一个解码器"这个 bug **恰好也能产生一条** `Call`（A 连接的
前半 + B 连接的后半拼起来），于是测试照样通过 —— 断言形式没错，却完全没有
区分度。

正确写法是在**错误的那一侧动作之前**插入一个否定断言：

```cpp
a.write("cal");  b.write("l\n");
QTest::qWait(300);
QCOMPARE(spy.count(), 0);   // ← 这一条才真正有区分度
a.write("l\n");            // 只有补上 A 自己的后半，才应出现 Call
```

推广：任何"多方各自持有状态"的逻辑（按连接/按线程/按设备的映射），
都要断言"**串味不会发生**"，而不只是"正确的那种情况最终能成功"。
第 22 轮的对照实验正是这样抓到首版测试的空转断言。

> 与本工程第 26 条的区别：第 26 条讲的是**同一个流程里两道守卫互为遮蔽**，
> 要断言"外层守卫独有的效果"；本条讲的是**多方之间不得串味**，
> 要断言"错误的一侧动作之前什么都没发生"。两者都是"否定断言"，
> 但被否定的对象不同。

### 29. 测试用 QSignalSpy 断言自定义类型前必须 `qRegisterMetaType`

`Q_DECLARE_METATYPE` 只让 `QVariant::value<T>()` **编译**通过；跨越队列投递
传递自定义类型还需要**运行时注册**，否则 Qt 打印
"Cannot queue arguments of type ..." 并把参数丢成默认构造值 ——
spy 里拿到的是 `Unknown` / `nullptr`，**断言失败的样子像被测代码的 bug**。

第 22 轮逐项变异实测出的边界（不能笼统概括成"不注册就整片变哑"）：

| 变异 | 失败用例 |
| --- | --- |
| 只去掉 `protocol::Signal` 注册 | 只有 `receivesExactWireBytes` |
| 只去掉 `QTcpSocket*` 注册 | 只有 `replyWritesExactWireBytes` |
| 两个都去掉 | 以上两条 |

本工程的 `tests/interop/test_interop.cpp` 早就注册了这两个类型（并被这条规则
复查通过），室内机侧新增的 `test_signaling_server.cpp` 一度漏掉、被此规则抓出。
现由 `check_signalspy_types_are_registered` 强制，自检见
`tools/verify_signalspy_metatype.py`（4/4）。

### 30. 有测试 ≠ 被测到：先确认被测代码在不在执行路径上

第 21 轮给 `FingerprintManager::startEnroll()` 写了 26 个用例并全部通过，
第 23 轮才发现它在整个工程里**没有任何调用者** —— 现场跑的是 `Form_finger`
里另一套内联状态机。这是"有测试 ≠ 被测到"的升级版：不是断言没落在输出上，
而是**被测对象根本不在执行路径上**。26 个绿灯与现场行为毫无关系，而且两份
实现**并不等价**（现场那套多一轮 `GET_IMAGE` 探手指）。

规则：为一个函数写测试之前，先确认它有生产调用者（或作为 connect 目标）。
`check_declared_slot_is_reachable` 现在会对"既无调用者、也不是 connect 目标"
的槽告警。

### 31. 无法用单元测试覆盖的产线路径，用静态契约检查钉住它的线上字节

`Form_finger` 的内联注册流程无法用单元测试覆盖（UI 类，需
`parentWidget()` 上溯到 `Widget` 才能拿到串口句柄，还要真实 `QTimer` 与
提示音；造夹具比被测代码还复杂，且会**再造一份平行实现**——正是第 30 条要
消除的问题）。

但它的命令字节是**指纹模块的硬契约**，不能没人守。做法是静态抽取
`append((char)…)` 的常量与 `readPacket(超时)`，与内置期望序列比对：
`check_live_fingerprint_enroll_sequence_is_pinned`。

这与"协议常量必须被测试钉住"（第 17 条）是同一思路的延伸：**能用运行时
测试就用运行时测试，不能用就用静态契约，但绝不能没人守**。

### 32. 报覆盖率必须连**分母**一起报，否则数字本身就是误导

第 24 轮第一次量化覆盖率得到 94.4%，看着很好；把从未被任何测试目标链接的
产线文件折进分母后，真实值是 **25.0%**。差别全在口径：那个 94.4% 只统计了
被链接进测试的 17 个文件，另有 19 个产线文件（约 1972 行，含 `widget.cpp`
678 行）压根不出现在 gcov 数据里。

**数字正确、口径错误，比没有数字更危险** —— 它会让人停止寻找缺口。

规则：凡声称覆盖率，必须同时给出"已测文件 / 产线文件总数"与未测清单。
`check_coverage_denominator_is_declared` 会强制 `tools/coverage_report.py`
既列出未测文件、又输出折入未测文件后的真实总覆盖率。

### 33. 故障注入的"全部仍然通过"要先怀疑**夹具**，不是被测代码

第 24 轮给 `test_signaling_client.cpp` 做故障注入自检时，前两版夹具都因
**路径**问题而静默失效（bash 把 MSYS 路径传给 Windows Python；构建目录用了
MSYS 的 `/tmp`，Windows 侧看不到），结果 6 个变异**全部**报"仍然通过" ——
看上去像"测试全是假绿"，实际一个变异都没注入成功。

把"夹具坏了"当成"产品有缺陷"写进结论，就是自己制造了一个假 bug。

规则：故障注入夹具必须**自证**三件事：
  1. 变异确实改动了文件（写入后重新读回比对）；
  2. 目标 `.obj` 的时间戳确实更新了（跑的确实是新二进制）；
  3. 锚点在源文件中**唯一**（`count == 1`）。
任一不满足即判**夹具错误**，不得计入"仍然通过"。

跨 shell/解释器传路径时，统一用**目标解释器认识的形式**。

### 34. 同一契约的**多份拷贝**之间没有契约：一致
性必须被检查，否则测试只验证了副本自己

第 25 轮发现指纹报文的构造契约存在两份副本：生产
`src/hal/fingerprint_hailin.cpp` 的 `buildPacket()`，以及链接期替身
`tests/fake_fingerprint_device.cpp` 的 `buildFrame()`（逐字节照搬）。所有指纹
单元测试跑的都是**替身**。

后果：生产的 `buildPacket()` 覆盖率为 **0**，报文字节从未被运行时测试直接
验证过；而两份拷贝可以各自漂移——改了生产那份，替身不跟着变，测试照样全绿，
**真机报文已经错了**。

这是第 23 轮"平行实现"的又一种形态：
  - 第 23 轮 = 同一**功能**有两套实现（`startEnroll` vs `Form_finger` 内联）；
  - 第 25 轮 = 同一**契约**有两份拷贝。
前者靠静态检查钉住线上字节，后者靠静态检查钉住两份拷贝的一致性。

规则：凡"契约被复制进测试替身"，必须有一条检查比较两份实现的**操作序列**，
且该检查要经**双向**故障注入验证（改生产要报、改替身也要报），并配一个
"两边都改成一样、应保持通过"的**对照实验**（证明它比较的是一致性，
而不是等于某个死写的期望序列）。

`check_fake_fingerprint_builds_same_bytes` 即此检查。

### 35. 优先链接**真实实现**，而不是复制一份进替身

第 25 轮查明 `HailinFingerprint::buildPacket()` 是 **public 且纯函数**——只拼
字节、不碰串口，构造函数也只设置参数不 `open()`。因此完全可以**直接链接真实
实现**来测。

此前的替身方案并非必要：它为了"不依赖串口硬件"而把报文逻辑复制了一份，
反而制造了第 34 条所说的漂移风险，并让真实实现覆盖率停在 0。

规则：做链接期替身之前，先问"被测的这部分到底碰不碰硬件？"。**纯逻辑就链接
真实实现**；只有真正需要隔离硬件时（`readPacket`/`sendPacket`/`open` 这些
真的走 `QSerialPort` 的路径）才用替身。

本工程现在两种取向**并存且刻意**，不要"统一"掉：
  - `t113_test_fingerprint_manager` 用替身，测**流程调度**（互斥、步进、超时）
  - `t113_test_fingerprint_wire` 链真实实现，测**报文字节**
一个链接 `fake_fingerprint_device.cpp`，另一个链接 `fingerprint_hailin.cpp`，
互不干扰。

**附带实测事实**：在**未打开的** `QSerialPort` 上调 `waitForReadyRead()` 会让
Qt5 **段错误**（探针实测退出码 139，崩溃栈在 QSerialPort 内部）。`readPacket()`
循环第一句就是它，所以"对未打开的设备调 readPacket() 看它是否优雅返回"这类
想当然的用例会让测试进程直接崩掉。这也正是"纯逻辑该与硬件访问分开"的又一
理由：纯逻辑部分不碰这条未定义行为路径。

### 36. 自检脚本必须能在**目标控制台编码**下安全输出，否则会伪装成"通过"

第 25 轮两个故障注入自检脚本在本机崩溃：

```
UnicodeEncodeError: 'gbk' codec can't encode character '\u2713'
```

它们用了 `✓`。本机控制台是 GBK。危险之处在**它伪装成通过**：报告前面的
检查结果已经正常输出，看上去一切正常，而 rc=1；调用方若忽略返回码或只看尾部
输出，就会读成"全绿"。

**代价是两轮误诊。** 第 24 轮遇到同一现象时，我因为当时用 pwsh 且设了
`PYTHONIOENCODING=utf-8` 去测（恰好掩盖了真因），把它误判为"多余参数"问题，
打了个忽略参数的补丁——治的是症状。第 25 轮在 bash、不带该环境变量时复现，
才查出真因。

规则：脚本要么把 stdout/stderr
`reconfigure(encoding="utf-8", errors="replace")`，要么不含目标控制台编不出的
字符。**逐个删符号不算修好**——第 24 轮我删了 `⚠`，第 25 轮就冒出 `✓`。
`check_tool_scripts_are_console_encoding_safe` 强制此事。

推而广之：**"在我这儿能跑"不是验证。** 验证必须在**目标环境的确切条件**下进行
（这里是"不带 PYTHONIOENCODING 的 GBK 控制台"）。用一个恰好掩盖问题的环境变量
去测试一个环境相关问题，得到的绿色是假的。

### 37. 覆盖率要再拆一维：未覆盖 = **不可达** + **可达未测**

第 25 轮量出 `fingerprint_hailin.cpp` 覆盖率 17.9%，
像个大洞。第 26 轮按可达性拆开后：

    可执行行 285
      零调用者（静态不可达，baseline 亦然）  204 行 = 71.6%
      有调用者（真的没测到）                  81 行，其中未执行 30 行

**未覆盖 234 行里只有 30 行真能靠写测试补上。** 其余是基线就没接线的死代码
（`autoIdentify`/`autoEnroll`/`cancel`/`stepRegister`/`stepIdentify` 五个入口在
生产代码里零调用者，实测 baseline 同样如此）。

只报一个百分比会**同时误导两个方向**：把死代码算成待补缺口，又把真缺口淹没。
所以：

    未覆盖 = 不可达（不是待办） + 可达未测（**待办看这一列**）

`coverage_report.py` 输出该分类并列出可达未执行的具体行；
`check_coverage_separates_dead_from_untested` 强制它这么做。

**规则的能力边界也要如实写清。** 这条规则是"能力存在性 + 分类标签存在性"检查，
**不能**发现"分类函数还在但被掏空成 `return {}`"—— 那需要真的跑一次脚本。
不写清边界就会高估它。故障注入还实测过：一致地改名（行为不变）时规则通过是
**正确**的，它检查的是能力是否存在，不是"有没有被改动"。

### 38. 变异"未触发"时，先证明变异真的改变了被测属性

第 26 轮给新规则做故障注入，前 3 个用例都报"期望报错、实得通过"。查下去是
**夹具缺陷**：关键字在文件里各出现 5 次，而我只替换了第一处，其余仍满足规则 ——
规则不报错是**正确**行为。

这是第 33 条的又一次实例（那轮是"路径错了，一个变异都没注入"）。教训一致：

**变异没触发时，第一嫌疑是变异没生效，不是规则失灵。**

具体到字符串替换：**替换次数要显式确认**。默认全替换，或断言替换后目标属性
真的消失（再读回文件、按规则同样的判据检查一遍）。同时也说明：

**我的期望也可能写错。** 我曾期望"函数一致改名后规则应失败" —— 但改名是保持
行为的重构，规则通过才是对的。这类"期望本身错了"只能靠**对照实验**发现：
凡断言"应当报错"，就配一条"改动但行为不变，应当通过"的对照，两者一起才说明
规则判的是性质而不是"有没有变"。

### 39. 锚点必须唯一，且要唯一到**正确的那个函数**

第 26 轮夹具的"锚点唯一"自证第一次真正发挥作用：变异目标
`if (resp2.size() < 12 || (quint8)resp2[9] != 0x00) {` 在
`fingerprint_manager.cpp` 里出现**两次** —— `startIdentify`(L143) 与
`startEnroll`(L269)。

若不收窄锚点，就会改错函数：测试照样会失败，而我以为验证的是识别路径，
**实际验证的是注册路径**。两处代码几乎逐字相同，肉眼看 diff 也发现不了。

规则：锚点唯一性不够，还要**唯一到目标函数**。同一工程里"平行实现"的两份
代码往往逐字相似（这正是第 23/34 条的成因），用共同的那几行做锚点必然歧义。
收窄办法是带上**只有目标处才有的上下文**（此例是紧随其后的
`emit identifyResult(...)` vs `emit enrollResult(...)`）。

### 40. 安全性质要有**运行期**断言，不能只靠静态文本规则

静态规则（"源码里敏感变量名没出现在日志语句里"）容易被绕过：**改名、拼串、
经辅助函数转一手**，正则就看不见了。第 27 轮为门禁密码补了运行期测试：真的
设哨兵密码、真的调校验、真的抓 `qDebug` 输出，断言哨兵及其 hex 一次都没出现。

故障注入实测：6 种真实泄漏形态（baseline 原句、只打明文、只打 hex、
`left(4)` 前缀脱敏、拼进 `QStringLiteral`、经局部变量转一手）全部被抓到，
2 个对照（只打长度、只打空判定）不误报。

### 41. "X 不出现"的断言必须配**阳性对照**，否则捕获管道坏了它照样通过

"日志里不含密码"这类断言有个致命失效方式：若消息处理器没装上、或日志走了别的
通道，那么**任何**"不包含"断言都会通过 —— 测试看着很严，实则零验证。

所以每条"哨兵不出现"都配一条"**非敏感日志确实被捕获**"的对照
（断言 `密码校验完成` 那行存在）。捕获管道坏了，对照先失败。

同一条道理适用于任何"不存在"型断言：**先证明检测手段能看见东西。**

### 42. 夹具跨过编码边界时，必须先断言它原样到达

第 27 轮第一版测试用**环境变量**注入**非 ASCII** 哨兵，然后断言"日志里不含该
哨兵的每个前缀"。故障注入发现"`left(4)` 前缀脱敏"变异**抓不到**。探针查明：

```
ASCII  设定 == 取回 ? true      （26 码元 -> 26 码元）
UTF8   设定 == 取回 ? false     （13 码元 -> 16 码元）
```

`AppConfig::toQString()` 是 `QString::fromLocal8Bit()`（`app_config.cpp:67`），
本机 GBK 代码页下 UTF-8 字节被按 GBK 解码。于是那条断言在比较**两个不同的串**，
**恒真** —— 断言在，实际什么都没验证。

**凡夹具跨过一次编码边界（环境变量、文件、argv），必须先断言它原样到达。**
否则围绕它的所有断言都可能是空的。

顺带：前缀断言要**逐一遍历每个前缀**，而不是只查一个固定长度 —— 只查
`left(4)` 会漏掉 `left(2)`、`mid(0,n)`、`.at(0)` 等其它脱敏写法。

### 43. 守卫条件不能引用被守卫内容里会出现的字符串

第 27 轮新规则里我写成：

```python
if "Fixture" not in text and "哨兵必须原样" not in text:
    if "doorPassword(), pwd" not in text:
        fail(...)
```

测试文件的**头部注释**里恰好写了"哨兵必须原样到达被测代码" —— 外层条件恒
False，真正的断言检查**永远不可达**。故障注入"去掉夹具自证往返断言"因此报
"规则未触发"。

这是第 22 轮"写了一条永远不会 fire 的规则"的同族问题。修法是干脆去掉逃生门，
直接检查断言本体。

**写守卫时要问：这个字符串会不会正好出现在我要检查的东西里？** 会，就别用它
当门。

另外，本轮夹具自己又踩了一次解析问题：verify.py 打印的是**大写** `  X [规则]`
而夹具按小写 `x` 判断，5 个用例被误报成"规则未触发"。夹具的解析逻辑同样要
按**真实输出**写，并靠对照用例自证（第 33/38 条）。

### 44. 写硬件替身之前，先查清真实库的**取值范围**与**内存所有权**

第 28 轮为 `GpioLine` 写可编程假 libgpiod，第一版失败时我两次怀疑方向都错了：

1. **取值范围**：假实现给每个 chip 64 条线，而工程用 132 / 133，于是所有正常
   用例失败 —— 看着像"生产代码开不了线"，实为夹具把合法线号判成越界。
2. **内存所有权**：libgpiod 1.x 里线对象**属于 chip**（`get_line` 返回 chip
   内部的数组元素），`release` 只清"已请求"状态、**不释放内存**，`close` 才
   释放。我第一版让两者各自 `new`/`delete`，于是生产代码在"请求失败"路径里
   先 `release` 再 `close` 时被误报 `release:unknown`。

**夹具与真实 ABI 不符时，会稳定地产生假阳性 —— 而假阳性比真缺陷更像真缺陷。**
写替身的第一步是把真实库的取值域与所有权查清，不是先把测试写完。

### 45. 断言"泄漏"要数**语义资源**，不是数对象

第一版用"线对象个数"判断泄漏，在错误的所有权模型下毫无意义。对齐 ABI 后真正
该数的是 **仍持有有效请求的线数**（`busyLineCount()`）：

> 泄漏的请求会让线一直 busy，下次开锁直接 EBUSY。

并且假实现让**对已 busy 的资源再次请求返回失败**，于是泄漏能被**端到端**抓到
（重试真的失败），而不是靠事后数数。

**推论：度量要选成"现场会痛的那个量"。** 数错了量，测试通过也说明不了问题。

### 46. 无法验证的行为只观测、不断言

生产代码在"请求失败"路径里会 `release` 一条**从未成功请求**的线（防御性清理）。
libgpiod 对该情况的确切行为我**无法在本机验证**（没有真实 `gpiod.h` / 库）。

所以夹具提供 `releaseUnrequestedCount()` 记录这件事，测试**不断言**它等于几，
只在注释里说明"为何不断言"。

**把猜测写成契约比不写更糟** —— 它会锁住一个可能错误的假设，后人改真代码时
反被此测试拦住。第 21 轮 `calcChecksum` 的"已知不可区分"标注是同一取向；
第 31 轮该函数被接线后，那条标注的前提消失，相应的故障注入随即转为致命变异
（见上文"实测：`calcChecksum()` 曾是死代码"）。

### 47. 规则的"登记检查"要覆盖**全部**相关文件

第 28 轮给 GPIO 写新规则时，我检查了 `fake_gpiod.cpp` 与 `hal/gpio.cpp` 是否
列进 CMakeLists，**却漏了 `test_gpio.cpp` 本身**。故障注入"测试未接进
CMakeLists"因此没触发 —— 测试文件存在但没人编译它时，规则会静默放过。

**列"必须存在的引用"时，逐个文件核对，别只挑印象最深的两个。**

### 48. 探针的一半价值在于**否掉错误假设**

第 29 轮为 `FaceManager` 写测试前先跑了三个临时探针。除了量到"未训练的
LBPH `predict()` 抛 `cv::Exception`"与"单 label 时 `predictedLabel == 1`
恒真"这两条有用事实，还**推翻了我自己的推断**：

> 我原以为"人脸框越界 -> `QImage::copy()` 返回空图 -> 存进空样本 ->
> `train()` 抛异常 -> UI 线程崩溃"。实测 `QImage::copy()` 传越界矩形返回的是
> **指定尺寸、内容未初始化**的图，`isNull() == false`，根本不是空图。

若不做探针，我会把这条**错误**的因果链写进注释与断言里 —— 而错误断言比没有
断言更糟：它会把一个不存在的行为固定成"契约"。**记录"我否掉了什么假设"和
记录"我发现了什么"同样重要。**

### 49. 安全参数如果只有一个判据，就要把它写成断言

探针证实：单 label 的 LBPH 模型里 `predictedLabel == 1` **恒为真**，因此
`confidence < threshold` 是**门禁唯一的门**。这意味着门禁安全**完全**系于
一个数字。

由此产生两条做法：
1. 专门写一条用例把"阈值是唯一判据"这一事实钉住（松阈值 -> 陌生人也能开门；
   紧阈值 -> 必须拒绝）。丢了它，就没人守这条唯一的门。
2. 阈值相关的断言**不硬编码 confidence 数值**，而用被测代码自己吐出的
   confidence 当基准（`阈值 = conf` 必须拒绝，`conf + 1` 必须接受）——
   既精确钉住比较语义（严格 `<` 而非 `<=`），又与 OpenCV 版本无关。

### 50. "没有判别力"的断言要当作没有断言

`addFaceSample` 应当把彩色图转灰度再存。我第一版用例只断言"彩色注册后
`train()` 成功"，理由是"不转换的话 LBPH 会抛异常"。**故障注入证明这条断言
毫无判别力**：

> LBPH 接受任何 `CV_8UC1` 的 Mat，即使那个 Mat 其实是 RGB32 缓冲的前 1/4
> 字节（`stride` 400 而 `width` 100），`train()` 照样成功。

改做第二个探针量出真正的判别力在**像素数据**上：

| 注册 | 用灰度同图案预测 | conf | 结果 |
| --- | --- | --- | --- |
| 真转换 | kind=0 | 0.00 | 接受 |
| 不转换 | kind=0 | 256.00 | 拒绝 |
| 不转换 | kind=1 | 153.60 | 拒绝 |
| 不转换 | kind=2 | **33.26** | **接受（无判别力）** |

于是改为"彩色注册 -> 用**灰度**同图案预测 -> 必须接受"，并**固定选 kind=0**
—— 因为 kind=2 那个图案即使错了也会通过（33.26 < 90）。

**结论：断言必须落在能区分正确/错误实现的输出上。** 一条"跑了没出错"的
断言，在故障注入下常常什么也抓不到；而且**不是所有输入都有判别力**，
要挑出真的能区分的那一个。

### 51. 规则匹配用词元边界，不要用子串

第 29 轮的 `[face-gate]` 规则用子串检查测试函数是否存在，结果故障注入抓到：
把 `thresholdIsTheOnlyGateWithSingleLabel` 改名成
`thresholdIsTheOnlyGateWithSingleLabel_removed()` 后，规则**仍然通过** ——
因为原名是改后名的**前缀**。

修法：要求严结尾，即检查 `"thresholdIsTheOnlyGateWithSingleLabel("`
（紧跟左括号）或使用**完整符号名**。这与第 43 条（守卫条件不能引用被守卫
内容里会出现的字符串）是同一类问题的两个面：**匹配一个符号时必须考虑它的
边界**。

同时删掉了我自己写的一条**不成立的用例**（"去掉隐患的说明文字"）：
注释文字并非必要条件，规则用符号名做锚点是对的 —— 是那条用例过强。

### 52. QtTest 的退出码**等于 FAIL 行数**，不是崩溃信号

第 30 轮实测（此前一直是**猜**的）：

| 变异 | 退出码 | FAIL 行数 |
|---|---|---|
| 请求输出失败时不释放线 | 1 | 1 |
| 析构不释放（RAII 完全失效） | 18 | **18** |
| `close()` 不 munmap（只关 fd） | 22 | **22** |

由此发现两件事：

1. 我此前在 `verify_gpio_test.py` 的文档里写的"多数变异是被进程崩溃抓到的，
   那些变异真的造成了 double-free / use-after-free"是**错的** —— 那些其实是
   **干净的多用例失败**。判据 `returncode not in (0, 1)` 把"多失败"当成了
   "崩溃"。
2. 更危险的是它还有**假绿**方向：真正的崩溃若恰好 `rc=1`，会被判成
   "未抓到（假绿）"，从而把一个**有效的回归测试**误删。这是最坏的一类错误 ——
   它删掉的正是保护你的东西。

正确判据：**真正的崩溃 = 非零退出码但一条 FAIL 行都没有**（如访问违例
`0xC0000005`）。退出码与 FAIL 计数是否一致还可以作为交叉校验。

新增规则 `check_fault_harnesses_classify_crashes_correctly` 钉住这一点。它
**只看可执行代码**：该模式会作为**字符串字面量**出现在故障注入夹具里（夹具
正是靠 replace 它来制造变异），只挖注释会把夹具本身误报出来。

### 53. 规则里的 `except Exception: continue` 会把规则变成**空操作**

第 30 轮写新规则时漏了 `import ast`，而循环体是：

```python
try:
    text = code_only(p)
except Exception:
    continue          # <-- NameError（ast 未导入）被静默吃掉
```

于是**每个文件都被跳过**，规则恒真。故障注入的"变异未被抓到"是唯一线索 ——
在它暴露之前，规则的输出与"全部合规"完全一样。

教训：自检脚本里**不要吞异常**。宁可让规则响亮地失败，也不要它悄悄通过。
（把 `except Exception: continue` 删掉后，`NameError` 立刻现形。）

### 54. 挖注释不够，还要挖**字符串字面量**

判断"某段代码是否还在"时，正则去 `#` 注释是不够的：

- CMakeLists 的**注释**里会反复提到源文件名（解释为什么用假替身），
  于是"把 `add_executable` 里那一行删掉"这个变异**抓不到** ——
  整文件子串仍然匹配注释里的名字。
- Python 夹具的**字符串字面量**里会出现被禁止的模式（它正是用来制造变异的），
  于是夹具自己被误报。

修法：用 `tokenize` 挖掉注释**与字符串**，只在真正的可执行代码上匹配；
CMake 则先把目标块切出来（`add_executable(...)` / `target_link_options(...)`
的括号内容），在块内查名字，而不是整文件查子串。

### 55. 影子头里的类一旦被 `qobject_cast`，就必须带 `Q_OBJECT` 并进源列表

第 35 轮为**第一个 Qt Widgets 测试目标**（`t113_test_form_passwd`）做影子
`Widget` 时，两个方向都实测了一遍：

| 做法 | 结果 |
|---|---|
| 影子 `class Widget` **不带** `Q_OBJECT` | **编译失败**：`qobject.h:522:5: static assertion failed: qobject_cast requires the type to have a Q_OBJECT macro` |
| 带 `Q_OBJECT`，但只放进 `target_include_directories` | **链接失败**：`undefined reference to Widget::staticMetaObject` |
| 带 `Q_OBJECT` **且**列进 `add_executable` 源列表 | 通过 ✓ |

两条都要记住：

1. Qt 5.15 的 `qobject_cast` 对没有 `Q_OBJECT` 的类型是**静态断言**拒绝的，
   **不会**退回 `dynamic_cast`。这不是运行期失败，是编译期硬错。
2. **AUTOMOC 不看 include 目录**。只把影子头放进 include 路径，`moc` 不会
   处理它，`staticMetaObject` 就没有定义。必须把影子头**显式写进**
   `add_executable(...)` 的源列表。

这与第 33 轮定下的"影子头不要带 `Q_OBJECT`"**并不矛盾** —— 那条规则的适用
前提是"影子类**从不**成为 `qobject_cast` 的目标"。一旦被测代码对它做
`qobject_cast`（本轮的 `form_passwd.cpp` 就沿父链
`qobject_cast<class Widget*>(w)` 找主窗口），就必须反过来。

同一个坑还有第三个面：影子头用**路径替换**（文件名叫 `ui/widget.h`）而不是
另起假文件名。第一版把 `AudioManager` 做成 `fake_audio_manager.h`，而
`form_passwd.cpp` 自己 `#include "managers/audio_manager.h"`，于是编译器看到
**两个** `class AudioManager`：`redefinition of 'class AudioManager'`。
替身必须放在**影子路径**（`tests/shadows/managers/audio_manager.h`）上，让
同一路径只有一份定义。这也正是影子头机制相对"假文件名"的优势：替换的是
**路径**，被测代码一个字都不用改。

### 56. "不含连续 N 位"对**逐位泄露**没有判别力

第一版的"逐位日志不泄露密码"断言写成"日志里不含输入串的连续 4 位"。
故障注入实测：把

```cpp
qDebug() << "[passwd] 数字键按下";                      // 安全
qDebug() << "[passwd] key" << btn->text();              // 逐位泄露密码！
```

从前者改成后者，该断言**照样通过** —— 2/2 漏报。

原因：QTest 的每条消息前缀会把相邻两位数字切开。

```
QDEBUG : TestFormPasswd::typingLogs...() [passwd] key 9
QDEBUG : TestFormPasswd::typingLogs...() [passwd] key 8
```

两位之间隔着测试名与级别前缀，输入的密码**永远不会**在日志里连续出现。断言
在，却什么都没验证（第 50 条）。

有判别力的性质是**差分**：日志内容不该取决于按了**哪几个**数字。取两个等长、
且**不共用任何数字**的密码各跑一遍，只保留被测代码自己打出的行，归一化后
**逐字节比较**：

```cpp
QVERIFY2(logA == logB, ...);        // logA 来自 "99999999"
                                    // logB 来自 "11111111"
```

这个性质与日志格式、前缀、间隔都无关，排版怎么变都不会失效。同时先断言两个
密码不共用数字（否则差分本身退化），并断言两份日志都非空（否则"相同"是因为
都是空串，又是真空成立）。

教训：**泄露的判别式是"输出是否随秘密变化"，不是"输出里有没有秘密的连续
片段"。** 后者对逐位、逐段、编码后等泄露形态一律无效。

### 57. 崩溃会掩盖后续用例 —— 越界索引前必须先查边界

`QSignalSpy::at(i)` 越界在 Release 下是**未定义行为**。第 35 轮故障注入实测：
把 `emit passwordEntered(inputBuffer)` 删掉之后，多处 `spy.at(0).at(0)` 直接让
进程以 `0xC0000005`（访问冲突）结束。

危害不是"崩了"（那也算抓到了），而是**诊断信息退化**：崩溃终止整个测试二进制，
后面所有用例都跑不到，输出里只剩一个崩溃码，没有哪条断言失败的说明；同时让
`rc` 与 `FAIL` 行数**不再一致**（实测 `rc=3221225477` 配 5 条 FAIL 行），
第 52 条那个交叉校验也跟着失效。

修法：加一个带边界检查的取值辅助函数，越界返回空串。

```cpp
QString payloadAt(const QSignalSpy& spy, int i)
{
    if (i < 0 || i >= spy.count()) return QString();
    const QList<QVariant> args = spy.at(i);
    return args.isEmpty() ? QString() : args.at(0).toString();
}
```

这样变异后每条用例都能干净地报"期望 X，实际空"，`rc` 重新等于 `FAIL` 行数。

### 58. uic 的 `connectSlotsByName` 会和显式 `connect` **叠加**，不是覆盖

第 35 轮写窗体测试时发现的**继承缺陷**：点一次"退格"会删掉**两位**。

根因（探针实测区分了两种可能）：

* `uic` 生成的 `setupUi()` 末尾调用 `QMetaObject::connectSlotsByName(this)`，
  它按**命名约定**自动连接：槽名形如 `on_<对象名>_<信号名>` 就自动接上。
* 而 `form_passwd.cpp` 的构造函数**又显式** `connect` 了一次同名槽。

于是同一个槽被连了**两遍**，一次点击执行两次。

| 探针 | `clicked` 发射次数 | 槽执行次数 |
|---|---|---|
| `btnbck->click()` | 1 | **2** |
| `btnok->click()` | 1 | **2** |
| 对照：普通按钮 | 1 | — |

信号只发一次、槽跑两次，因此确定是**双重连接**，而不是鼠标事件送了两遍。

`baseline` 同样如此（`form_passwd.cpp:42` 就是显式 connect，且 `.ui` 里没有
`<connections>` 块，uic 一定走 `connectSlotsByName`），属**继承而来**，不是
重构引入。

**本轮不修**：改变"退格删几位"是用户可感知的行为变化，属产品决策，硬约束要求
行为逐字节兼容。改为写一条**特征化测试**把现状钉住：
`backspaceRemovesTwoDigitsPerPress_documentsInheritedDefect`。它先断言
`clicked` 只发一次（确认双重连接确实存在），再断言删两位。若有人改了行为，
这条会红，提醒他这是有意变更。同时
`okIsIdempotentDespiteDoubleConnection` 钉住一个**侥幸**："确定"键也被连了
两遍，但靠 `if (!inputBuffer.isEmpty())` 恰好幂等，密码只提交一次 —— 若哪天
把这个判断去掉，门锁会被驱动两次。

一般教训：**`on_<对象名>_<信号名>` 这个名字一旦用了，就等于同时声明了"手工
接线"和"约定接线"两次。** 两者叠加且静默，不跑起来看不出来。其余界面（
`form_finger` / `form_call` / `form_passwd.ui` 里的其它按钮）都要按这个思路
复核。

### 59. 影子头要连"依赖链上游"一起替，否则真头会从别处被包含进来

第 35 轮第一版只替了 `ui/widget.h`，而 `form_passwd.cpp` 第 10 行自己
`#include "managers/audio_manager.h"`（真头）。因为 `ACCESSTERMINAL_SRC_ROOT`
在 include 路径里，真头照样被找到，于是编译器看到两个 `class AudioManager`。

教训：**只要被测源文件直接包含了某个类型，替身就必须替换那个类型的头文件
路径本身**，而不是在别处提供一个同名类。检查方法很直接：看被测 `.cpp` 的
`#include` 列表，逐个确认哪些必须替（会拖重依赖/碰硬件的），**每一个都要有
对应的影子路径**。

### 60. 用"用例定义"而不是"标识符出现过"来确认测试还在

`check_form_passwd_contract_is_pinned` 第一版查
`"logCapturePipelineWorks" not in text`。故障注入实测：把该用例的**定义改名**
之后规则**仍然通过** —— 因为这个名字还出现在声明里，以及**另一个用例的报错
文案**里（`"正对照见 logCapturePipelineWorks"`）。

修法：查**定义**形态，即 `TestFormPasswd::logCapturePipelineWorks\s*\(`。
这与第 51 条（用词元边界而不是子串）是同一条：确认一个符号**存在且可用**，
要匹配它的定义点，不是它任意一次被提及。

### 61. 改源文件的脚本要有**不依赖 `finally`** 的还原保障

第 36 轮真实事故：`tools/verify_form_call_test.py` 第一版把"原始内容"只放在
**内存**里，靠 `finally` 还原。结果脚本跑到第一个变异（删掉
`widget->notifyPeerStopSend();` 那一行）时被**从外部强杀**，`finally` 根本没
机会执行，仓库里就留下了一个**被变异过的源文件**。

它的可怕之处在于**看起来完全正常**：只是少了一行，没有语法错误、没有标记。
直到下一次运行在启动预检时报"锚点出现 0 次"才暴露出来。如果那个变异恰好是
"把信令改成 stop"这种语义错误，它会被原样提交上去。

三道防线（缺一不可）：

1. **磁盘上的原始副本**，不只在内存里 —— `<临时目录>/fi36_pristine/`；
2. **启动自愈**：若发现磁盘副本与当前文件不同，先用副本还原再干活。于是
   上一次被强杀留下的污染会被**下一次运行自动修好**；
3. **信号处理器 + `atexit`**：`SIGINT`/`SIGTERM`/`SIGBREAK` 也走还原。

`finally` 仍然保留（正常流程用），但它**不是**唯一保障。这条与第 33 轮
"不要 `job_kill` 一个正在改源文件的夹具"是同一件事的两面：前者是操作纪律，
这条是让脚本自己扛得住纪律失守。

### 62. 规则要限定在**拥有该性质的那个用例体内**，不能全文件搜

第 36 轮实例：`check_form_call_stopsend_is_wired` 要确认"断言落在线上字节上"，
第一版写成全文件搜 `QByteArray`。故障注入实测**两次假绿**：

- 第一次：把 `QCOMPARE(got, QByteArray("stopsend\n"))` 换成 `QVERIFY(true)`，
  文件**别处**仍有 `QByteArray` 用法（`QByteArray("call\n")` 等），rc=0；
- 补上字面量匹配后**仍然**假绿：同一个 `QCOMPARE(got, QByteArray("stopsend\n"))`
  在**另一个用例**里也有一份，删掉主用例那一份，全文件搜照样命中。

修法：用 `_function_body()` 先框出
`TestFormCall::clickingStopSendsExactlyStopSendOnTheWire`，**只在它内部**找。
这与第 34 条（宽松正则一旦有第二处同形代码就静默失效）是同一类；也和
`_cmake_call_containing()` 用"作用域内匹配"替换宽松正则是同一招。

### 63. 需要匹配**字符串字面量**时，用第三种剥离器（只挖注释）

第 36 轮发现前两个剥离器都不适用：

- `strip_comments_and_strings()` 会把字符串内容**清空** —— 用来查
  `"stopsend\n"` 这类线上字节字面量的检查会变成真空命中/真空不命中，两端都
  没有意义；
- `strip_comments_and_strings_cmake()` 把 `#` 当注释起点 —— 用在 C++ 上会把
  `#include`、`#error` 整条删掉（第 35 轮已踩过一次）。

于是新增 `strip_comments_only()`：挖 `//` 与 `/* */`，**保留字符串字面量**，
保留换行（行号不漂移）。选择剥离器取决于**要匹配的东西在不在字符串里**：

| 要匹配的东西 | 用哪个 |
|---|---|
| 注释里也会出现的代码标识符（`Q_OBJECT`、`#error`） | `strip_comments_and_strings` |
| 线上字节字面量（`"stopsend\n"`） | `strip_comments_only` |
| CMake 参数（几乎全是带引号的） | `strip_comments_and_strings_cmake` |

### 64. 夹具的锚点不要钉在**会漂移的文案**上

第 36 轮连带修的：`verify_form_passwd_rule.py` 里两条"去掉影子头 `#error`
安全阀"的变异，锚点是 `#error` 那行的**整句中文文案**。本轮给两个影子头的
安全阀加上了第二个测试宏（`ACC_FORM_CALL_TEST`）之后，文案一变，夹具立刻报
"锚点出现 0 次"——**规则本身是对的**，是夹具把自己测坏了。

同一个文件里还有第三条：`shadows/ui/widget.h` 那行在**两个**
`add_executable` 里都出现之后，裸行锚点命中 2 次。

修法：
1. 安全阀两条改为**正则变异**（`RX_MUT`），锚在 `#\s*error\s` 这个**指令**
   本身。被测属性是"安全阀存在"，不是那句文案 —— 锚在文案上等于让夹具去测
   一件与规则无关的东西。
2. 影子头那条锚点**带上目标名**，恢复唯一性。

这与第 39 条（锚点必须唯一，且要唯一到正确的那个函数）是同一条：**新增一个
同形的目标/文件之后，所有"裸锚点"都要重新检查唯一性**。接新测试目标时这是
最容易漏掉的一步 —— 它不会让任何测试变红，只会让**验证工具本身**静默失效。

### 65. 未定义行为不必然崩溃，所以**崩溃不能当回归守卫**

第 37 轮最贵的一课。修掉一条真实的 use-after-free（`Form_finger` 析构时对已被
Qt 销毁的 `FingerprintManager` 调 `setSuspended()`）之后，自然想写一条
"让窗体直接析构、看它崩不崩"的回归测试。故障注入证明这条路**不可靠**：

- 用当时那份**较薄的**测试文件：把 `QPointer` 改回裸指针后，**5/5 运行全部
  正常退出**（rc=0），一次都没崩；
- 把测试补强（加整条线上序列断言等）后：同一变异 **8/8 全部崩溃**
  （rc=139 / 0xC0000005）。

也就是说"崩不崩"取决于**后续代码是否恰好复用了那块已释放内存**，是偶然的、
随测试规模浮动的。用偶然现象做守卫，等于守不住。

更雪上加霜的是本例的 `setSuspended()` 开头有
`if (m_suspended == suspended) return;` —— 即使真的对已销毁对象调用了它，
也可能因为内存未被复用而"看起来什么都没发生"。

正确做法是**换成确定性的判据**：
1. **静态规则**钉住"做法"（成员必须是 `QPointer`、必须包含 `<QPointer>`、
   挂起与恢复必须成对且分别落在退出与析构两个函数体内）——
   `check_form_finger_suspended_manager_is_guarded`；
2. **单元测试**钉住"成因前提"（Qt 确实先销毁先创建的子对象；顺序一反就红）；
3. 在行为夹具里保留一条 `UNDETECTABLE` 记录，并注明"补强后已能抓到"——
   记录要与时俱进，不是免死金牌。

推论：审阅任何"靠崩溃来判定"的测试时，都要先问**它在缺陷代码上跑过没有**；
没跑过就不能算守卫（第 50 条的同一条理，只是这次的判据是"是否崩溃"）。

### 66. 判定"X 停下来了"之前，先排除同一通道上的**其它生产者**

第 37 轮的 `quitStopsThePollTimer` / `destructionStopsThePollTimer` 第一版
直接看 `fakefp::sentCount()` 的增量，结果**误报失败**。原因是真
`FingerprintManager` 也在用同一个假串口（`startIdentify()` 每 500ms 一条
`CMD_GET_IMAGE`），而窗体退出时恰好把常驻识别**恢复**了 —— 计数照涨，
断言把管理器的**合法**流量错记成"窗体还在轮询"。

这与第 52 条是同一件事的两个面：**一个通道上有多个生产者时，"它停了"必须
先把别的生产者隔离出去。** 本轮的隔离手段是 `setSuspended(true)` + 等一拍 +
抽空事件队列（把恢复路径的 `singleShot(0,...)` 变成空操作），之后再比增量。

写这类断言时的检查清单：
- 这条通道上还有谁会说话？（定时器、兄弟对象、被恢复的后台任务……）
- 我的"静音"是不是真的落到生产者的判据上？（本例落在 `m_suspended` 上，
  而 `startIdentify()` 开头就查它 —— 有效。）
- 我等的"性质"会不会被恢复路径自己满足？（会，所以要在恢复**之后**再静音。）

### 67. 两个"会改写源文件"的夹具不得并行，应由工具自己拒绝

第 37 轮真实踩到：行为夹具（`verify_form_finger_test.py`）还在后台跑时，我又
触发了规则夹具（`verify_form_finger_rule.py`）。两者改的是同一批文件
（`form_finger.h/.cpp`），于是**交叉污染**：规则夹具报"析构时不恢复"没被抓到、
行为夹具报了两条莫名其妙的误报与假绿；而**单独串行重跑时全部正常**。

这与第 61 条是同一类的两件事：第 61 条说"**自己**改的源文件必须能可靠还原"，
本条说"**两个**这样的夹具不能同时改"。也不要把责任推给"下次记得串行"——
让工具自己拒绝才是可维护的做法：两个夹具**共用一把文件锁**
（`%TEMP%\fi37_rewrite.lock`，写入 pid，能识别并接管陈旧锁），拿不到锁就立刻
退出并打印原因。**流程纪律不要寄托在人的记性上**（第 52 条的同一条理）。

排查提示：如果某条变异"上一轮能抓到、这一轮抓不到"，先确认**当时有没有别的
夹具在跑**，再怀疑被测代码（第 33 条：先怀疑夹具）。

### 68. "行为上不可观测"的防御性调用，要**看得见**地交代给谁守

第 37 轮暴露出一类容易变成死角的代码：`Form_finger` 析构里的
`stopFingerPoll()`、以及析构后把 `m_suspendedFingerprintManager` 置空 ——
把它们删掉，**不会有任何行为测试变红**：

- 轮询定时器以 `form` 为 parent，随窗体一起销毁，且析构过程不跑事件循环，
  所以"析构时停不停表"看不出差别；
- `setSuspended()` 开头有 `if (m_suspended == suspended) return;`，析构只执行
  一次，重复调用无可见差异。

但它们的**同类调用**在退出路径上是真有风险的（`close()` 只隐藏窗口、对象仍
活着），所以不能因为"测不到"就不管。处置办法三步：
1. 把"不可观测"这件事写进夹具的 `UNDETECTABLE`，**并写清原因**（而不是假装测过）；
2. 给它们配一条**静态规则**（本例：退出与析构两个函数体内都必须有停表与恢复
   调用），把防御性代码纳入确定性守卫；
3. 再给规则各配一条变异，证明它真的守得住（本轮：13 个变异全部被抓到）。

**没有守卫的死角，要看得见。**

### 69. 规则按**内容**定位，并如实交代它的**判别边界**

第 31 轮消除校验和重复实现时，配套改了 `check_fake_fingerprint_builds_same_bytes`：
原先它只比 `buildPacket()` 与替身 `buildFrame()` 的操作序列，而校验和的
**累加范围**（`i = 6` 起算）随之挪进了辅助函数里。只比那两个函数的话，
有人把范围**只改一边**，上面那轮照样全绿 —— 规则出现了一个静默的盲区。
故新增"辅助函数体"这一路比对。

写这条新比对时踩了两个坑，都由**新写的故障注入夹具**当场抓出：

1. **按函数名定位辅助函数**（`calcChecksum` / `checksumOf`）→ 替身只要
   改个名，就被误报成"报文契约漂移"；参数改个名更糟，原因是"抽取失败"，
   看起来却像真漂移。**改成按内容定位**：找出同时含"下标循环"与"累加
   形态"的函数体，名字无关。
2. 只要求"含下标循环"仍不够 —— 它误命中了 `readPacket()` 里那个
   `for (int i = 1; i < frames.size(); ++i)` 排队循环，规则立刻报出一堆
   假漂移。**判据必须同时约束循环与累加**。

更要紧的是那条**边界**：若有人把 `6` 改成 `0` 且**两边都改**，这两条静态
一致性规则**在定义上**就区分不了（两份实现仍然"一样"）。钉住它的是
**运行时**契约 —— `test_fingerprint_wire.cpp` 用独立手写的 `expectedFrame()`
逐字节比对，故障注入实测：把唯一实现的起点改成 0，18 个用例里 **10 个变红**。

所以夹具里那条"两边同时漂移"的用例期望值是 **`ok`（不报）**，并注明
"靠运行时守"。**不假装静态规则能覆盖它** —— 那正是约定第 46 条批评的
"把猜测写成契约"。同时这条新夹具刻意做成**跨平台纯 Python**（不依赖
msys64），使"规则本身有没有判别力"能在 Linux/CI 上验证。

### 70. 安全阈值的上限必须**低于度量的饱和点**，"很大的数"不等于安全上限

门禁阈值 `behavior.face_confidence_threshold` 的上限原本是 `1e9`。第 31 轮
把规则从"有没有 clamp 字样"改成了"上限是不是大到形同虚设"，但**把门槛定成
了 `1e6`** —— 于是 `1000` 被判为合格。

第 38 轮用探针把 LBPH 的距离量了一遍，才发现门槛本身就定错了：

| 对象 | 实测距离（LBPH `create()` 默认参数） |
| --- | --- |
| 同一身份（含噪声/光照抖动） | 0 ~ ~103 |
| 完全不同的图像（冒充者） | ~170 ~ ~256 |
| 随机噪声 | 饱和于 ~256 |

距离在 **~256 处饱和**，因此阈值 `>= 256` 时**任何**图像 —— 包括完全陌生的
脸 —— 都判"通过"。也就是说 `1e9`、`1000`、`256` 在安全意义上**完全等价**，
都是"门锁形同虚设"；旧门槛 `1e6` 只是挡住了"看起来离谱"的那一类。

判据于是改为**与被测对象自己的量纲挂钩**：上限必须 `< 256`（饱和点）。
本工程取 `150.0`，它落在"本人 <= ~103"与"冒充者 >= ~170"之间。配套：

- `tools/verify.py` 的 face-gate 由**告警**升级为**失败**（上限 >= 256 即红）；
- `tools/verify_face_gate_rule.py` 给出两侧边界用例：`256.0` 必须红、
  `255.0` 必须通过；该夹具同时改为从脚本位置推仓库根（原先写死 Windows
  路径），使这条规则在 Linux/CI 上也能验证；
- `test_app_config.cpp` 把 `150.0` 钉死（`900` / `1e9` 一律夹到 `150`）；
- `test_face_manager.cpp` 从**行为**侧再钉一遍：阈值推到上限 `150` 时，
  完全不同的图像仍必须被拒绝（放回 `1e9` 该用例即红）。

教训：**"很大的数"和"安全上限"是两件事**。凭感觉取的常数保护的是"看起来
离谱"，不是"真的危险"；安全相关的门槛应当来自度量本身的**饱和点/分离带**。

### 71. 给"资源不可用"加守卫时，必须同时给出**恢复路径**

第 39 轮修指纹串口的日志洪泛（2.5Hz × 2 条，永不停止）时，遇到一个很容易
做错的取舍：

```cpp
// 看起来够了 —— 其实是错的
if (!fingerprint->isOpen()) return;
```

这条守卫确实止住了刷屏，但它有**副作用**：全工程只有 `widget.cpp` 启动时调用
一次 `open()`，**没有任何代码会重新打开串口**。守卫一旦命中，指纹识别在该进程
的余生里再也不会尝试 —— 把"偶尔不可用"变成了**永久不可用**，而且没有任何日志
说明这件事。

正确的做法是"守卫 + 恢复"：不进识别循环，但按固定间隔重试 `open()`，设备出现时
自动接回：

```cpp
if (!fingerprint->isOpen()) {
    scheduleReopenRetry();   // 每 reopenRetryMs() 重试一次 open()
    return;                  // 关键：这一轮不进循环，所以不会刷屏
}
```

教训：**守卫描述的是"现在不能做"，不是"以后也不用做"**。凡是对"设备暂时不在"
加守卫的地方，都要同时回答"它回来了怎么办"；否则守卫就是一次**静默的功能删除**。
串口、USB、GPIO、网络重连都适用。

配套用例刻意做成**两条**而不是一条：一条钉"不可用时不动作"（去掉守卫即红），
一条钉"可用时必须恢复"（让重试失效即红）。只有前者的话，"永久关死"会被当成
合格实现 —— 那正是本约定要防的。

**日志速率也是缺陷的一部分**：重试每 5 秒打一条同样的话，一个月约 52 万条，
在嵌入式 flash 上只是从"刷屏"变成"慢性泄漏"。同一状态只提示一次，状态改变时
再提示（本轮的 `m_openFailureLogged` 即此意）。

### 72. 编译通过要**按构建配置**分别确认，Release 绿不等于 Debug 绿

第 41 轮做 P2（凭据改加盐哈希）时，`credential_store.cpp` 里写了：

```cpp
static const int kMinIterations = 1;      // 类内初始化
...
const int n = qBound(kMinIterations, iters, kMaxIterations);
```

**Release 下一切正常**：`ctest` 21/21、互操作 14/14、`verify.py` rc=0。
但覆盖率构建（`--coverage -O0`，即 Debug）**链接失败**：

```
undefined reference to `accessterminal::CredentialStore::kMinIterations'
undefined reference to `accessterminal::CredentialStore::kMaxIterations'
```

原因：C++11 里类内初始化的 `static const int` 只是**常量表达式，不是定义**。
一旦某处 **ODR-使用** 它，链接期就需要真正的定义 —— 而 `qBound` 是模板，
形参是 `const T&`，于是取了地址。-O2 下编译器把值直接内联、没有符号引用；
-O0 下不内联，于是符号缺失。

修法是补一行类外定义：

```cpp
// credential_store.cpp（文件作用域，不是类内）
const int CredentialStore::kMinIterations;
const int CredentialStore::kMaxIterations;
```

教训有两层：

1. **技术层**：类内初始化的 `static const` 整数成员，只要被按引用传递、
   取地址、或被模板接住，就必须另写类外定义。改用
   `enum { kX = 1 };` 或（C++17 起）`static constexpr` 可以绕开，
   但那两种写法分别有作用域/可用性的代价，本工程选**显式补定义**，
   并在原地留下注释说明"别删"。
2. **流程层（更重要）**：本项目此前的门禁**只跑 Release**。
   于是"编译通过"这个最基础的信号，实际上只覆盖了一半配置。
   补上 Debug 后立刻抓到了它。**凡是"编译/链接是否通过"的结论，
   都要说清楚是在哪种构建配置下得到的。**

配套：`tools/run_coverage.sh` 本身就是 Debug（`--coverage -O0`），
所以**跑覆盖率不只是量覆盖率，也是一次 Debug 构建门禁** ——
这轮之前它一直是红的（链接失败），却因为没人把它的退出码当门禁而没被发现。
现在它进自检清单。

### 73. 剥离注释/字符串的**顺序**错了，会让**所有**静态规则静默失效

`tools/verify.py` 里 50 条规则共用同一个底座：先把注释和字符串剥掉，
再在剩下的代码文本上做匹配。底座写错，就是**全体规则一起错**，
而且是**静默**地错。

第 41 轮加 `credential_store.cpp` 时，`[credential]` 报"盐没有参与哈希构造"，
可代码里 `toHex(salt)` 写得明明白白。查下去是顺序问题：

```python
# 出错时的顺序：块注释 -> 字符串 -> 字符字面量 -> 行注释
text = re.sub(r'"(?:\\.|[^"\\])*"', blank, text)   # 字符串
text = re.sub(r"'(?:\\.|[^'\\])*'", blank, text)   # 字符字面量  <-- 太早
text = re.sub(r"//[^\n]*", "", text)                # 行注释      <-- 太晚
```

源文件第 48 行的 `//` 注释里有个**孤立的撇号**：

```cpp
//  undefined reference to `accessterminal::CredentialStore::kMinIterations'
//                                                                        ^
```

它与几千行之后 `QLatin1Char('0')` 的开引号配成一对，字符字面量正则
**一次吃掉中间全部代码**：

```
剥之前 20184 字节  ->  剥之后 4349 字节
toHex / iterateDigest / salt 全部消失
```

于是依赖它们的规则集体误报。触发条件低得可怕：**注释里任何一个
英文撇号**（`don't`、`it's`、`` `Foo' ``）都会引爆它，与文件内容无关。

修法是把**行注释提前到字符字面量之前**：

```python
text = re.sub(r'//[^\n]*', "", text)                # 先剥行注释
text = re.sub(r"'(?:\\.|[^'\\])*'", blank, text)    # 再剥字符字面量
```

这样做是安全的，因为**字符串内容此时已经清空**，所以
`"http://example.com"` 里的 `//` 不会被误当成注释。

教训：

1. **共享的预处理步骤要有自己的测试。** 之前 50 条规则各有故障注入夹具，
   却**没有一条**去测底座 —— 底座错了，那些夹具全都在测错的东西。
   现在有 `tools/verify_stripper_robustness.py`（20 项），并且它**自带反证**：
   临时把顺序改回去，必须变红（实测 7 项 FAIL），改回来必须全绿。
2. **顺序敏感的重写，要用"换行数不变"这类硬性质兜底。** 剥离器可以改变
   内容，但绝不能改变行数（否则所有报错行号都偏）。真实仓库全量回归里
   逐文件断言了这一点。
3. 排查时先量**体积**：`20184 -> 4349` 这种断崖式缩水，一眼就能定位。

### 74. 故障注入夹具"用过期副本自愈"，会静默删掉合法改动

本项目里有一类夹具（`verify_form_finger_test.py` 等）为了做故障注入，
会**就地改写仓库源文件**，因此必须有还原保证。这段逻辑踩过三次坑，
每次都升级了判据 —— 前两次记在 §61/§62，第三次是第 41 轮：

| 版本 | 做法 | 踩的坑 |
| --- | --- | --- |
| v1 | 原文只在内存，`finally` 写回 | 被强杀就留下**污染源文件**，看起来完全正常（§61） |
| v2 | 磁盘副本 + `副本 != 当前文件` 就自愈 | 开发者两次运行之间的**合法修改被静默回滚**（第 37 轮实测丢了一轮测试补强） |
| v3 | 磁盘副本 + `.in-progress` 哨兵 | 本机强杀（`job_kill` → `TerminateProcess`）**必定**留下哨兵，于是拿一份**可能很旧**的副本来"自愈" |
| v4 | 哨兵 + **新鲜度校验** | 本轮 |

第 41 轮实测的事故：`%TEMP%\fi37_pristine\widget.h` 是 **7528 字节**的
第 37 轮**之前**版本，磁盘上是 **8421 字节**。哨兵残留 → 自愈 →
第 37 轮给影子头加的全部内容被抹掉，随后 23 个变异因"锚点找不到"而全红。

v4 的判据：标记文件里记下"上次开始时各文件的**长度**"。中断恢复时：

- 当前长度 == 记录长度 → 当前文件就是上次开始时的样子，**可以**自愈；
- 长度不符 → 无法区分"变异残留"与"之后的合法编辑" → **不自愈**，
  以当前文件为准并**明确告警**，请人工 `git diff` 核对。

这是**有意的保守**：宁可留下污染（一眼可见、重跑即可），
也绝不删掉合法改动（**静默**的数据丢失，最坏的一种失败）。

三个结构性改进：

1. **抽成公共模块** `tools/fixture_pristine.py`。原先 5 个夹具各抄一份，
   抄错一个就少一处保护 —— 实测 `verify_form_call_test.py` 就是漏改的那个，
   它的自愈判据还是最危险的 v2 形态，而且**完全没有互斥锁**。
2. **公共模块自己有单元自检** `tools/verify_fixture_pristine.py`（20 项），
   其中专门断言"**拒绝**用过期副本自愈"和"陈旧锁（持有者已死）可接管"。
   保护还原保证的代码如果自己错了，受害的是**仓库源文件**。
3. **把另外 10 个"就地改写仓库、却只有 `finally` 保护"的夹具也接上保险。**
   它们的入口被薄封装成
   `main() -> 拿锁 / prepare() / install_guards() / _main_locked() / finally restore()`，
   夹具内部一行未改（原有 `finally` 保留为第一道防线，新增的是唯一能在
   被强杀后生效的那道）。

顺带记一个自己踩的 bug，因为它正在本节的延长线上：给这 10 个夹具插保险的
脚本，第一版**先取 `def main():` 的偏移，再往文件前面插 import** ——
插入让后续偏移全部右移，用旧偏移切新字符串就把 `def run_verify():`
从中间切断，8 个文件同时语法错误。**凡是"先取偏移、再改内容、
再用旧偏移"的写法都是同一个 bug。** 现在的脚本（a）所有插入做完**之后**
才搜索锚点，（b）写出前 `compile()` 一次，不过就**拒绝写回** ——
宁可什么都不做，也不留一个坏文件。

### 75. Windows 上 `os.kill(pid, 0)` **不是**存活探测，陈旧锁会永远无法接管

本工程所有"改写源码"的夹具共用一把文件锁（`%TEMP%\fi37_rewrite.lock`），
锁里写持有者的 pid，发现持有者已死就接管。原先的存活探测是：

```python
try:
    os.kill(other, 0)
    alive = True          # 无异常就当活着
except OSError:
    alive = False
```

第 41 轮实测（`tools/` 下的探针，结论可复现）——在 **Windows** 上：

| 情形 | `os.kill(pid, 0)` | 正确结论 |
| --- | --- | --- |
| pid 从未存在（如 999999999） | `OSError [WinError 87]` | 已死 |
| pid 曾经存在、现已**正常退出** | **无异常** | 已死 |
| pid 真活着 | 无异常 | 活着 |

它只能区分"从未存在"和"存在过"，**区分不了"活着"和"已退出"**。
而且在 `os.kill` 分裂之前那次探测里还发现一个更危险的性质：
本机 `os.kill(pid, 0)` 对**仍然活着**的子进程也不终止它 ——
所以它既不是可靠的"杀"，也不是可靠的"探"。

后果是**静默停摆**：夹具被强杀（`job_kill` / 任务管理器）留下陈旧锁后，
之后**每一个**夹具都会打印"另一个夹具正在改写源码"然后拒绝干活，
看起来像是"环境里恰好有别的夹具在跑"，实际上是自己把自己锁死了。
这条 bug 与 §74 叠加时尤其难查：两者都表现为"夹具莫名其妙全红"。

正确做法是用 Win32 API 查进程退出码：

```python
handle = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
if not handle:
    return False                                  # 打不开 = 不存在
code = ctypes.c_ulong()
k32.GetExitCodeProcess(handle, ctypes.byref(code))
return code.value == 259                          # STILL_ACTIVE
```

已收敛到 `tools/fixture_pristine.py::process_is_alive()`，非 Windows 平台
退回 `os.kill(pid, 0)`（POSIX 语义本来就是对的）。
`tools/verify_fixture_pristine.py` 里钉住了这张表的**全部四行**，
并且刻意记录"本机 `os.kill` 把已退出的进程判为活着"——
如果哪天它变了，那条用例会立刻提醒复核。

教训：**平台相关的系统调用，要用"能区分目标状态"的实测来确认语义，
不能靠"看起来像"。** `os.kill(pid, 0)` 在 POSIX 教科书里是标准写法，
在 Windows 上却只是个"这个 pid 出现过吗"的弱检查。

### 76. 夹具的"锚点"和"期望值"会**随被它盯着的代码一起过期**

故障注入夹具靠两种东西活着，两者都会腐坏，而且**腐坏方式不同**：

| 腐坏对象 | 症状 | 危险度 |
| --- | --- | --- |
| 变异**锚点**（那段要改的字面文本） | 锚点找不到了，夹具通常直接报错退出 | 低（吵，看得见） |
| 断言**期望值**（"应该剩 2 条"） | 夹具**照常运行**、只是恒定报红 | **高（安静，看不出是夹具坏了）** |

第 41 轮实测两起，都**不是**这一轮引入的，且都藏了很久：

1. `verify_form_call_rule.py` 的"去掉影子头安全阀"变异，锚点写的是
   **两个宏**的版本；第 37 轮给影子头加了第三个宏
   （`ACC_FORM_FINGER_TEST`，指纹窗体也要用它），锚点**从那时起**
   就匹配不上了。夹具的报错是"锚点出现 0 次"—— 属于上表第一类。
2. `verify_dead_slot.py` 期望"给 `startEnroll` 加上调用者后
   只剩 **2** 条不可达槽"。那是第 23 轮全仓库有 **3** 个不可达槽时的算术。
   此后 18 轮里另外两个槽陆续被处理掉（一个接了线、一个删了），
   基线降到 **1** 条，于是"少 1 条"的正确结果是 **0**。
   旧期望 2 **永远不可能再满足**，夹具恒定假红 —— 属于上表第二类。

第二类能藏 18 轮，是因为**第一条用例只断言 `>= 1`**：

```python
case("真实仓库（有不可达槽）", "slot", None, 1, requires=None)   # 只下界
case("给 startEnroll 加上调用者 -> 只剩 2 条", "slot", ..., 0, 2) # 却瞄准精确值
```

下界断言无法区分"基线 3 条"和"基线 1 条"，于是**基线悄悄漂移而无人察觉**，
直到精确值那一条永远红。修法不只是改期望值，而是**把基线也钉成精确值**：

```python
case("真实仓库：不可达槽基线恰为 1 条（即 startEnroll）", "slot", None, 1, 1)
case("给 startEnroll 加上调用者 -> 归零", "slot", ..., 0, 0)
```

**两条通用规则**：

1. **凡是"变异后数量应当变化"的断言，都要先有一条把**基线**钉成精确值的
   用例。** 只写下界等于放弃对基线的监控。
2. **改动被夹具盯着的代码时，顺手跑一遍那个夹具。** 影子头加了第三个宏，
   同一轮里没有任何检查提醒 `verify_form_call_rule.py` 已过期 ——
   它的"证据"是失效的，但没人问它。

诊断提示：夹具**恒定**报同一条红、且与本次改动无关时，先怀疑锚点/期望值
过期，而不是被测代码真的坏了。判据是跑一次"对照"用例 ——
如果连真实仓库的基线用例都不过，那是夹具自己的问题。

### 77. 跨线程传实时数据：**只传容量 1 的最新值，不排队**

实时音视频（采集帧、播放包）在跨线程/跨进程传递时用一条固定语义：

> **生产者只保留最新一份，消费者跟不上就丢弃旧数据 —— 绝不入队。**

这条与"普通消息"的直觉相反，所以特别容易写错。要点是**排队在实时数据上
是纯有害的**：

| 排队会发生什么 | 为什么有害 |
| --- | --- |
| 消费者跟不上时队列无限增长 | 内存只增不减，直到 OOM |
| 延迟随积压线性增长 | 画面/声音越来越滞后，且**永远追不回来** |
| 显示的是旧数据 | 人脸框叠在**旧帧**上、画面却是最新的 → 看起来像"检测漂了" |

人眼只关心最新一帧、人耳只关心最新一包，**丢弃旧数据正是正确行为**。
本工程各处都已经自觉遵守（`AudioReceiverThread` 在 ALSA 缓冲满时明确丢弃
整包并在启动时丢弃积压；`V4L2Camera` 用容量 1 的 `lastFrame` + 互斥），
第 42 轮把这条**固化成约定并加规则守护**（`[latest-value]`）。

#### 配套要求：消费者必须能判别"新旧"

容量 1 只解决了"不排队"，还差一半：**消费者要能知道拿到的是不是新数据。**
否则它会反复处理同一份数据（重复解码/编码/发送，纯浪费），或者反过来
把旧帧当成新帧处理。

本工程的做法是给视频帧加**代际号**：

```cpp
// V4L2Camera：代际号与帧在**同一临界区**内更新（这点是关键）
if (loaded) {
    QMutexLocker locker(&frameMutex);
    lastFrame = img;
    ++lastFrameGeneration;     // 必须在同一把锁里
}
```

消费者用法：记下上次处理过的代际号，相等就跳过。

```cpp
const quint64 g = camera->frameGeneration();
if (g == m_lastSeenGeneration) { return; }   // 没有新帧，别重复干活
m_lastSeenGeneration = g;
```

**为什么必须同一临界区**：分两处写会让消费者读到"新代际号 + 旧帧"
（或反之），防旧帧机制就静默失效了 —— 它看起来还在工作，实际判反了。
`[latest-value]` 规则直接检查这一点。

#### 只想知道尺寸时，不要深拷贝整帧

`QImage::copy()` 是**深拷贝**。曾经 `Widget::onFaceDetectResult()` 写的是：

```cpp
const QImage frame = m_camera->getFrame();   // 1.2 MB 深拷贝
... frame.width() / frame.height() ...        // 只用了两个整数
```

为两个整数付一次整幅拷贝，且每次检测完成都付（约 2 Hz）。改用
`V4L2Camera::frameSize()`（同一把锁下返回缓存的 `QSize`，语义逐位一致）。
规则会拒绝 `frameSize()` 里出现 `getFrame()` 或 `QImage`。

#### 提交到线程池的异步任务必须有背压闸门

`QtConcurrent::run` 是**即发即忘**的。任务耗时一旦超过提交间隔，任务就在
`QThreadPool` 的**无界队列**里堆积。判定与两种正确形态：

| 形态 | 适用场景 | 判据 |
| --- | --- | --- |
| **丢弃式** | 结果只是提示、丢了无所谓（人脸检测框） | `std::atomic<bool>` + `compare_exchange`，忙则直接放弃本次 |
| **取消式** | 每次都要有结果、但旧的不再需要（提示音） | 提交前 `stop()` 打断上一次，并登记未完成的 future |

第 42 轮实测：人脸检测原本两者皆无 —— Haar 检测在 T113 上可达数百毫秒，
而提交间隔约 500 ms，于是任务持续堆积（内存只增 + 框来自越来越旧的帧）。
现改为丢弃式闸门。

两条实现细节，都踩过：

1. **释放闸门必须用 RAII**，不能在每个 `return` 前手写 `store(false)`。
   `cv::Exception` 在本工程是现实存在的（测试里就有捕获它的用例），
   漏一条退出路径就会让闸门**永久为 true** —— 表现为"人脸检测从此
   再也不执行"，且没有任何日志，极难定位。
2. **闸门标志要用 `std::shared_ptr<std::atomic<bool>>`，不能是裸成员。**
   任务可能比 `Widget` 活得久（`QPointer` 守卫正为此存在），
   若任务持 `this` 去清标志，对象先析构就是一次 use-after-free
   （写已释放的成员）。`shared_ptr` 按值捕获后，原子量活到最后一个任务结束。
   `AudioManager::interruptFlag` 用的是同一手法。

#### 规则是**按提交点所在函数**判定的，不是按文件

`[latest-value]` 对每个 `QtConcurrent::run` 定位它所属的函数体，只看那个函数
有没有闸门。原因：按文件判定会让"在已有闸门的文件里新增一个无闸门提交点"
**假绿** —— 而真实项目就是这样一点点长出无界任务堆积的。
`tools/verify_latest_value_rule.py` 里专门有一个变异钉住这点，并且实测过
把规则改成文件级后它确实漏报（否则那条变异证明不了什么）。

### 78. 检查者本身也要被检查：**安静地少跑一条规则**是最难发现的失效

`verify.py` 是全部 52 条规则的载体。它出问题时，症状往往**不是报错**，
而是"安静地少跑一条"—— `rc=0`、"全部检查通过"，一切看起来正常。
第 42 轮实测到这一类里的三种形态：

| 形态 | 症状 | 为什么危险 |
| --- | --- | --- |
| 定义了 `check_*` 但忘了在 `main()` 里注册 | 该规则**从不执行**，rc 照样 0 | 新写的规则从未生效，而作者以为已经守住了 |
| 错误处理路径从不触发（`% LOCK` 的 `NameError`） | 只在锁忙时炸，平时全绿 | 发作时恰是最需要清晰提示的时刻 |
| 文档里手写的"50 项检查" | 数字随代码漂移且**毫无症状** | 与约定 §76 同类：人工维护的数字会过期 |

**三条对策，都在这次落地：**

1. **注册表自检**：`verify.py` 打印的"N 个检查"是**从 `main()` 源码里数出来的**，
   同时检测"已定义但未注册"的 `check_*` 函数并 `fail("[harness-crash] ...")`。
   两个实现细节都是踩出来的：

   - 必须数 **`main()` 函数体源码里实际出现的调用**。只数"模块里定义了几个
     `check_`"是不够的 —— 那恰好会把"定义了却忘了注册"算进去，
     而那正是最该被发现的情况。
   - 必须用 **`globals()`，不能用 `dir()`**。在函数内部 `dir()` 返回的是
     **局部**名字，于是待检测集合恒为空、检测恒不报 —— 一条永远为真的检查。
     这个 bug 是故障注入探针抓出来的（加一个未注册的 `check_` 函数，
     规则居然不红）。

2. **交叉验证那个数字**：`tools/verify_harness_integrity.py` 用**一段独立实现**
   重新数一遍注册数，再与 `verify.py` 打印的值比对。
   注意**谁在抓它**：是夹具，不是 `verify.py` —— `verify.py` 打印的就是它自己
   list 的长度，无法察觉自己被写死，它没有第二份判据。
   对照用例是这条判据的阳性证据：加一个**已注册**的空规则后，打印值与独立计数
   **同步**从 52 变 53，证明这个计数是**数出来的**、不是碰巧相等。

3. **摆脱手写数字**：引用检查条数时，抄 `verify.py` 的输出
   （`检查函数: N 个（已注册）`），不要抄文档里的旧数字 —— 本工程此前
   在文档里先后写过 48、50，而真实值早已是 51、52。

**通用判据**（比这三条更重要）：**凡是"守卫/检查"性质的新代码，必须先证明它会红。**
一条永远为真的检查比没有检查更糟 —— 它会让人停止寻找缺口（同 §19 的
"数字正确、口径错误，比没有数字更危险"）。

> 第 42 轮用这种方法**当场抓到两个自己的 bug**：`dir()` 那个恒不报的检测，
> 以及探针插在 `sys.exit(main())` 之后导致的白跑一次。
> 两次都不是靠"再读一遍代码"发现的，而是靠"让它必须红一次"。

### 79. 静态规则守**形态**，单元测试守**语义**；这条分工要写进规则里

第 43 轮做 P4（人脸质量门与防假阳性）时踩到一个认识上的坑，值得单独记下来。

**事情经过**：我为"连续命中的统计窗口必须从**第一次**命中起算"写了一条静态
判据 —— 把 `if (!s.hasFirst || expired) { ... } else { ... }` 两个分支切出来，
断言 else 分支里不得出现计时调用。我原以为这条足以守住"窗口语义"。

**夹具把它证伪了**：把 `expired` 写死成 `false`（窗口彻底失效）之后，
`hasFirst` 的**名字**仍然出现在分支条件与赋值里，判据全绿。
**文本存在性 ≠ 机制存在** —— 这是第 42 轮 `dir()` 那个 bug 的同一类错误，
只是换了个地方。

**结论（比那个具体判据更重要）**：

| 手段 | 能证明 | 不能证明 |
| --- | --- | --- |
| 静态规则 | 结构**形态**：函数存在、被调用、调用顺序、分支划分、默认值 | 任何"算得对不对"的**语义** |
| 单元测试 | 运行期**语义**：给定输入的行为 | 有没有人在别处绕过它 |

所以正确做法是**两者都做，并把分工写在规则注释里**：

- `[face-anti-fp]` 判据 D 明确写着它只管形态，并注明"把 `expired` 写死为
  false 这类变异无法靠文本判定可靠区分"；
- 窗口语义交给 `tests/test_access_controller.cpp` 的
  `streakWindowStartsAtFirstHitNotLast`；
- 而且**再证明一次这个分工成立**：`tools/verify_face_anti_fp_rule.py` 除了
  10 个静态变异，还做一次**运行期变异** —— 把实现改成"从最近一次起算"，
  重新编译并运行测试，断言那个用例**真的变红**。

**没有最后这一步，"语义已被守住"就只是一句没有证据的话。**
这和第 78 条的判据是同一条：**凡是守卫性质的东西，必须先证明它会红。**

顺带两条本阶段的小教训：

- **变异必须模拟"真实的退化形态"**。我最初把"质量门没接线"写成
  `if (false)`，结果静态规则判绿 —— 因为文本还在。真实的退化是**整段代码
  不存在**（类写了、测试全绿，但没人调用）。写成 `if (false)` 是在测试
  检查器的实现细节，不是在测试它要防的东西。
- **锚点必须唯一，且改动必须能编译**。我写过一个"只删掉
  `bool hasFirst = false;` 声明"的变异 —— 那会让 `.cpp` 里所有
  `s.hasFirst` 编不过。那是语法错误，不是退化，真实项目不会以这种形态存在。
  夹具的预检（锚点出现次数）会抓到这类问题。

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

# 2b. 校验器自检：对 check_config_value_sanity / check_config_knobs_are_used /
#     check_no_running_reset_in_run / check_no_secret_in_logs /
#     check_declared_signal_is_emitted / check_connect_signal_is_signal /
#     check_abandoned_async_tasks / check_subproject_cmake_is_selfcontained /
#     check_hex_parse_is_validated / check_load_state_reset / check_wire_format_constants_are_pinned /
#     check_nonblock_pcm_handles_eagain /
#     check_gated_wire_paths_have_resume_test /
#     check_fingerprint_ack_tag_is_validated /
#     check_signalspy_types_are_registered /
#     check_declared_slot_is_reachable /
#     check_live_fingerprint_enroll_sequence_is_pinned /
#     check_coverage_denominator_is_declared / check_narrowing_cast_of_config
#     做故障注入。静态规则写错会静默失效，必须证明它真能区分好坏代码。
python3 tools/verify_config_sanity.py .
python3 tools/verify_config_knobs.py .
python3 tools/verify_stop_flag.py .
python3 tools/verify_secret_logs.py .
python3 tools/verify_dead_signal.py .
python3 tools/verify_connect_signal.py .
python3 tools/verify_abandoned_task.py .
python3 tools/verify_subcmake.py .
python3 tools/verify_hex_parse.py .
python3 tools/verify_config_state.py .
python3 tools/verify_cross_target.py .
python3 tools/verify_wire_format.py .
python3 tools/verify_nonblock_pcm.py .
python3 tools/verify_gated_resume.py .
python3 tools/verify_fp_ack_tag.py .
python3 tools/verify_signalspy_metatype.py .
python3 tools/verify_dead_slot.py .
python3 tools/verify_signaling_client_test.py .
python3 tools/verify_fingerprint_wire_test.py .
python3 tools/verify_identify_failure_paths_test.py .
python3 tools/verify_password_log_test.py .
python3 tools/verify_password_log_rule.py .
python3 tools/verify_gpio_test.py .
python3 tools/verify_gpio_rule.py .
python3 tools/verify_face_manager_test.py .
python3 tools/verify_face_gate_rule.py .
python3 tools/verify_v4l2_test.py .
python3 tools/verify_v4l2_rule.py .
python3 tools/verify_form_passwd_test.py .
python3 tools/verify_form_passwd_rule.py .
# 自检脚本必须在**不带 PYTHONIOENCODING** 的 GBK 控制台下也安全
# （第 25 轮的教训：带 utf-8 测会掩盖真因，绿色是假的）
env -u PYTHONIOENCODING python3 tools/verify_signaling_client_test.py .
env -u PYTHONIOENCODING python3 tools/verify_fingerprint_wire_test.py .
env -u PYTHONIOENCODING python3 tools/verify_identify_failure_paths_test.py .
env -u PYTHONIOENCODING python3 tools/verify_password_log_test.py .
env -u PYTHONIOENCODING python3 tools/verify_password_log_rule.py .
env -u PYTHONIOENCODING python3 tools/verify_gpio_test.py .
env -u PYTHONIOENCODING python3 tools/verify_gpio_rule.py .
env -u PYTHONIOENCODING python3 tools/verify_face_manager_test.py .
env -u PYTHONIOENCODING python3 tools/verify_face_gate_rule.py .
env -u PYTHONIOENCODING python3 tools/verify_v4l2_test.py .
env -u PYTHONIOENCODING python3 tools/verify_v4l2_rule.py .
env -u PYTHONIOENCODING python3 tools/verify_form_passwd_test.py .
env -u PYTHONIOENCODING python3 tools/verify_form_passwd_rule.py .
bash tools/run_coverage.sh

# 2c. 指纹串口帧解析算法（把 header 里的算法照搬为 Python 独立执行，
#     含「原实现对照」与随机模糊测试；无需编译即可验证定帧逻辑）
python3 tools/verify_fingerprint_parser_algorithm.py

# 3. 跨仓库协议一致性
python3 tools/check_protocol_sync.py

# 3b. cppcheck 静态分析（不需要编译器、不需要装 Qt，可跑在任何机器上）
#     覆盖数据流、越界、空指针解引用、初始化顺序等 verify.py 查不到的问题。
#     退出码只受 warning/error 影响，style 类仅打印。
bash tools/run_cppcheck.sh

# 4. 真实编译验证（本机类型检查；静态检查无法发现类型不匹配等问题）
bash tools/check_host_compile.sh

# 5. 链接完整性检查（发现 undefined reference / 重复定义）
bash tools/check_host_link.sh

# 6. 跨工程信令互通（需同级目录存在 Accessterminal_desktop）
#    注意：该脚本目前是 MSYS2 专用（/mingw64 + cygpath），Linux 上见 P8
bash tests/interop/run_interop.sh

# 7. 单元测试（独立构建，不需要 ALSA）
cmake -S tests -B /tmp/t113-tests -G Ninja && cmake --build /tmp/t113-tests \
  && ctest --test-dir /tmp/t113-tests --output-on-failure

# 8. 覆盖率（**同时是一次 Debug 门禁**）
#    Release 内联掉的东西在 -O0 下会成为真实符号引用。第 41 轮就是靠它
#    抓到 `static const int` 缺类外定义（Release 全绿、Debug 链接失败）。
#    见开发约定第 72 条。
bash tools/run_coverage.sh

# 9. 凭据相关的故障注入自检（改 P2 相关代码后必跑）
python3 tools/verify_credential_rule.py        # 静态规则：12/12 变异应变红
python3 tools/verify_credential_store_test.py  # 行为测试：12/12 变异应变红

# 10. 夹具底座自检（**改 verify.py 的剥离器，或改任何夹具的还原/锁机制后必跑**）
#     这两条守的是"所有其它检查赖以成立的前提"：
#       - verify.py 的注释/字符串剥离器错了 -> 50 条规则一起静默失效（约定 §73）
#       - fixture_pristine 的备份/锁错了     -> 仓库源文件被静默回滚（约定 §74/§75）
python3 tools/verify_stripper_robustness.py    # 20 项；含"顺序改回去必须变红"的反证
python3 tools/verify_fixture_pristine.py       # 26 项；含"拒绝过期副本自愈"与存活探测

# 11. 并发/实时数据约定的故障注入自检（改 V4L2Camera 的帧交接、或改任何
#     QtConcurrent::run 提交点后必跑；约定 §77）
python3 tools/verify_latest_value_rule.py      # 静态规则：8/8 变异应变红，2/2 对照不误报

# 12. 校验器**自身**的完整性自检（改 verify.py 的 main() 注册表后必跑）
#     它守的是"verify.py 安静地少跑一条规则"这种最难发现的失效：
#       - 定义了 check_* 却忘了在 main() 里注册 -> 该规则从不执行，rc 照样 0
#       - 打印的"检查函数 N 个"被写死     -> 文档里的数字随之漂移且无症状
python3 tools/verify_harness_integrity.py     # 2 个变异 + 2 个对照

# 13. 人脸防假阳性（质量门 + 连续命中平滑）的规则自检（约定 §79）
#     改 FaceQuality / AccessController 的连续命中 / 人脸识别路径后必跑。
#     它除了 10 个静态变异，还做一次**运行期**变异：把窗口改成"从最近一次
#     起算"，重编译并跑单元测试，断言 streakWindowStartsAtFirstHitNotLast
#     真的变红 —— 静态规则只能证明"形态"，语义必须靠测试（§79）。
#     运行期那一步需要一个已配置的构建目录，默认找 %TEMP%\p4antifp_rt，
#     可用环境变量 P4_RT_BUILD 指定；找不到会打印"(跳过)"而不是假绿。
python3 tools/verify_face_anti_fp_rule.py     # 10 个变异 + 3 个对照 + 1 条运行期证明
```

以上脚本在 CI 中也可直接使用，退出码非 0 即表示失败。

第 1~3 步是静态检查，速度快但覆盖有限；**第 4、5 步不能省**——
像“前置声明了不存在的类名”“把 ALSA 枚举存成 int 再传回枚举形参”
这类缺陷只有编译器能发现，“漏定义某个符号”只有链接器能发现。
第 7 步同样不能省：**只有构建退出码、却没有任何用例被执行**是最容易被
误判为“测试通过”的失败模式（本轮就修掉了一例）。

第 3b 步（cppcheck）介于两者之间：它不需要编译器和 Qt 就能做数据流分析，
在没有工具链的机器上也能跑，是本工程唯一可用的“半编译级”检查手段。
安装：`pacman -S mingw-w64-x86_64-cppcheck`（MSYS2）。

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
- **密码窗体"退格"一次删两位（继承缺陷，未修）**：`uic` 生成的
  `setupUi()` 会调 `QMetaObject::connectSlotsByName(this)`，按命名约定把
  `on_btnbck_clicked` / `on_btnok_clicked` 自动接上；而 `form_passwd.cpp`
  又显式 `connect` 了一次，槽被连两遍。信号只发一次、槽执行两次（探针实测）。
  `baseline` 同样如此，属继承而来。**本轮不修**：改变"退格删几位"是用户可
  感知的行为变化，硬约束要求行为逐字节兼容，属产品决策。现状已由特征化测试
  `backspaceRemovesTwoDigitsPerPress_documentsInheritedDefect` 钉住，
  `okIsIdempotentDespiteDoubleConnection` 另钉住"确定"键靠
  `!inputBuffer.isEmpty()` 恰好幂等（若去掉该判断，门锁会被驱动两次）。
  详见约定第 58 条。若要修，正确做法是**删掉显式 `connect`**（保留 uic 的
  约定接线），并同步更新那两条测试与本节。
- `Widget::on_btnpsswd_clicked` 等其余 `form_*` 窗体存在**同类风险**：凡槽名
  符合 `on_<对象名>_<信号名>` 且又被显式 `connect`，都会叠加。本轮只对
  `form_passwd.cpp` 做了行为验证（它有了 Widgets 测试目标）；其余窗体尚未
  逐个实测。按第 58 条的思路排查即可。
