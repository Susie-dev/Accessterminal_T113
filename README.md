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

### 单元测试（无 ALSA 的开发机也能跑）

主工程的根 `CMakeLists.txt` 需要 ALSA，而 ALSA 在 Windows/macOS 上装不了，
因此上面那条命令在开发机上通常走不通。单元测试只依赖 `Qt5::Core`/`Qt5::Test`，
**独立构建**即可：

```sh
cmake -S tests -B /tmp/t113-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/t113-tests
ctest --test-dir /tmp/t113-tests --output-on-failure
```

当前 11 个目标：`t113_protocol`（协议编解码）、`t113_fingerprint_parser`
（指纹帧解析）、`t113_app_config`（配置读取与夹取）、`t113_audio_manager`
（提示音播放，用假 ALSA 注入设备错误）、`t113_audio_sender`（UDP 12345
上行音频）、`t113_audio_receiver`（UDP 12345 下行播放）、`t113_video_push`
（UDP 23456 视频推流）、`t113_camera_preview`（UDP 50000 远程监看）、
`t113_fingerprint_manager`（指纹串口互斥调度，用**链接期替身**）、
`t113_signaling_client`（TCP 8887 发送失败与断线重连）、
`t113_fingerprint_wire`（指纹报文构造，链接**真实**实现）。

注意最后两个目标的取向**相反**，这是刻意的：
`t113_fingerprint_manager` 用替身替换 `fingerprint_hailin.cpp`，
`t113_fingerprint_wire` 则链接真实实现。前者测流程调度，后者测报文字节。

除前三个是纯逻辑外，其余都用**可注入的假替身**在真实接口边界上跑：
假 ALSA（设备错误按需注入）、假套接字（记录/投递数据报）、假 V4L2 相机
（链接期替身）。因此它们不碰任何真实硬件，却对**线上字节**做逐字节断言——
详见后面各节。

> 至此，**四类线上通路（TCP 8887 信令、UDP 12345 音频、UDP 23456 视频、
> UDP 50000 监看）全部有可执行的线上字节断言**，不再存在"看起来测过、
> 其实一步没跑"的通路。

跨工程信令互通测试同理，见 `tests/interop/run_interop.sh`（同样不依赖 ALSA）。

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

原因在于**分母**：那个 94.4% 只统计了**被链接进测试目标的 17 个文件**。
另有 **19 个产线源文件（约 1972 行）从未被任何测试目标链接**，压根不出现在
gcov 数据里：

| 从未被任何测试链接的产线文件 | 近似可执行行 |
| --- | --- |
| `src/ui/widget.cpp` | 678 |
| `src/hal/fingerprint_hailin.cpp` | 332 |
| `src/ui/form_finger.cpp` | 162 |
| `src/core/vision/v4l2_camera.cpp` | 131 |
| `src/hal/gpio.cpp` | 105 |
| `src/managers/face_manager.cpp` | 52 |
| `src/ui/form_call.cpp` | 46 |
| `src/ui/form_passwd.cpp` | 85 |
| `src/managers/password_manager.cpp` | 19 |
| 其余头文件与 `main.cpp` | — |

**数字正确、口径错误，比没有数字更危险** —— 它会让人停止寻找缺口。

`tools/run_coverage.sh` 一键产出：

```
已测文件 969 行（覆盖 714）+ 未测文件约 1640 行 = 分母 2609
近似总覆盖率 = 27.4%   （上表那个 73.7% 仅描述已测文件）
```

（第 25 轮新增 `t113_test_fingerprint_wire` 后，把 332 行的
`fingerprint_hailin.cpp` 从"从未链接"移进了"已测"一栏，因此：分子从 671 升到
714，**已测文件的百分比反而从 94.9% 降到 73.7%**——因为刚接进来的那个文件
只覆盖了纯逻辑部分，硬件路径仍为空。分母从 2679 降到 2609，真实总覆盖率
从 25.0% 升到 **27.4%**。

这正是"分母必须一起报"的价值：只报已测文件百分比的话，**做对了事数字反而
变难看**，会诱导人不去补齐未测文件。）

#### 已测文件的明细与未覆盖行

被测到的文件覆盖度其实相当高（707 行中 671 行，94.9%）：

| 文件 | 可执行行 | 覆盖 | 覆盖率 |
| --- | --- | --- | --- |
| `src/config/app_config.cpp` | 89 | 89 | 100% |
| `src/core/media/audio_sender_thread.cpp` | 51 | 51 | 100% |
| `src/managers/audio_manager.cpp` | 69 | 68 | 98.6% |
| `src/core/net/protocol.h` | 58 | 56 | 96.6% |
| `src/hal/fingerprint_frame_parser.h` | 50 | 48 | 96.0% |
| `src/core/media/audio_receiver_thread.cpp` | 93 | 89 | 95.7% |
| `src/core/media/video_push_thread.cpp` | 31 | 29 | 93.5% |
| `src/core/media/camera_preview_server.cpp` | 52 | 45 | 86.5% |
| `src/core/net/signaling_client.cpp` | 61 | 45 | 73.8% |
| `src/managers/fingerprint_manager.cpp` | 128 | 110 | 85.9% |

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

##### 实测：`calcChecksum()` 是**死代码**，校验和逻辑在同一文件里有两份

```
src/hal/fingerprint_hailin.cpp:484  quint16 HailinFingerprint::calcChecksum(...)
                                    调用者：0
buildPacket() 体内：不含 calcChecksum，自己内联了同一个累加循环
文件里 `for (int i = 6; ...) sum += ...` 出现次数：2
```

实测 baseline（`terminal/smartpronew/HailinFingerprint.cpp`）**同样零调用者**——
是**继承来的**，不是重构引入的，故本轮不改行为。

但它是个真实隐患，故障注入给出了确切后果：**只把 `calcChecksum` 的起始下标
从 6 改成 0，18 个指纹用例全部照常通过**（因为没人调用它）。若有人"顺手修好"
这个死函数并开始调用，报文校验和就会与 `buildPacket()` 不一致。

`check_fingerprint_checksum_logic_is_not_duplicated` 以**告警**级别记录此事
（改它是否属于"修缺陷"需要产品决策）；实际生效的那一份契约由报文单元测试钉住。

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
| `calcChecksum` | 4 | 无 |

五个入口在**生产代码里零调用者**，实测 baseline 同样如此 —— **继承而来**，
既不是重构缺陷，也不该被算作待办。为它们写运行时测试只会测到"平行实现"
（第 23/30 条）。

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

以下四十三条是本工程最容易在重构中被悄悄破坏、又最难靠运行测试发现的约定。
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

用 `.clang-format` 统一格式，但**格式不是正确性**——上述四十三条请以
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
# 自检脚本必须在**不带 PYTHONIOENCODING** 的 GBK 控制台下也安全
# （第 25 轮的教训：带 utf-8 测会掩盖真因，绿色是假的）
env -u PYTHONIOENCODING python3 tools/verify_signaling_client_test.py .
env -u PYTHONIOENCODING python3 tools/verify_fingerprint_wire_test.py .
env -u PYTHONIOENCODING python3 tools/verify_identify_failure_paths_test.py .
env -u PYTHONIOENCODING python3 tools/verify_password_log_test.py .
env -u PYTHONIOENCODING python3 tools/verify_password_log_rule.py .
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
bash tests/interop/run_interop.sh

# 7. 单元测试（独立构建，不需要 ALSA）
cmake -S tests -B /tmp/t113-tests -G Ninja && cmake --build /tmp/t113-tests \
  && ctest --test-dir /tmp/t113-tests --output-on-failure
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
