# 借鉴 Accessterminal 方案 —— 把相邻仓库的优点搬进本项目

> 本文是一份**设计文档**，不是待办清单。它回答一件事：隔壁
> `/home/firefly/Desktop/Accessterminal`（下文简称 **A 仓库**）里有若干
> 本仓库（下文简称 **B 仓库**，即本项目）还不具备的优点，**怎么搬、按什么
> 顺序搬、每步怎么验收、哪些坚决不搬**。
>
> 与本文配套的仍是两份老文档：`README.md`（参考手册 + 71 条开发约定）与
> `docs/交接与继续开发.md`（交接说明）。本文不重复它们的内容，只补一件事：
> **跨仓库的架构借鉴路线**。

---

## 零、前提与红线

### 0.1 为什么以本仓库为基座

A 仓库的架构构思确实更好（见第一节），但**它没有 git**：无历史、无回滚、
无协作；仓库里还堆着 88.5MB 构建产物，`ui-shots.pro` 已被重写成
`/home/firefly/Desktop/...` 绝对路径。本项目有 57 个提交、可执行的 48 项静态
检查与 18 个测试目标、"证明过什么/没证明过什么"写得很清楚的交接文档 ——
**这些都是 A 仓库没有、且无法用一次重构补齐的资产**。

因此本方案的取向是：**以 B 为基座，把 A 的架构与安全模型搬过来**，
而不是反过来。

### 0.2 全程不可动的红线

1. `src/core/net/protocol.h` **逐字节冻结**（与 `Accessterminal_desktop`
   两侧 SHA256 一致，由 `tools/check_protocol_sync.py` 守护）。
   本方案 P0~P6 **没有任何一个阶段需要改它**。
2. 端口、信令字符串、TCP 分包语义、UDP 帧格式、提示音时序与音效顺序不变。
3. C++11；只使用 Qt 5.9 就有的 API（目标板 Qt 5.9~5.12）。
4. 不破坏 README 的 71 条开发约定；新增机制必须配静态规则或行为测试。

### 0.3 落地纪律

- **一个阶段一个提交**，每个提交可单独 `git revert`。
- 搬一个模块，**同时搬它的测试与规则**；没有守卫的迁移不算完成。
- 改变现场行为（提示音时序、补光/开锁、密码校验方式）的改动，
  必须在 `docs/交接与继续开发.md` 里**显式记录为行为变更**。

---

## 一、两个仓库的对照结论

### 1.1 一句话

两者是**同一台 T113 门口机的两次独立重构**（同一套协议、同一个前期原型
`smartpronew`），构思取向相反：

|   | A 仓库 `Accessterminal` | B 仓库 `Accessterminal_T113`（本项目） |
|---|---|---|
| 构思 | **架构驱动**：把"谁能进门"收敛成单一出口，硬件全赶出 GUI 线程 | **验证驱动**：冻结契约 + 故障注入自证，用规则替代板端验证 |
| 构建 | qmake | CMake + 交叉工具链文件（并保留 `smartpro.pro`） |
| 版本控制 | 无 | 57 提交，旧代码作为 baseline 先入库再重构 |
| `src/` 规模 | 13,730 行 / 63 文件 | 6,873 行 / 37 文件 |
| 测试规模 | 3 个目标 / 3,470 行 | 18 个 ctest 目标 / 1,270 条断言 + 41 个 Python 校验脚本 |
| 业务中枢 | `AccessController`（唯一放行出口） | 无，编排在 `Widget`（923 行 / 42 成员） |
| 凭据 | SHA-256 + 16 字节盐、多用户、临时码、锁定到期 | 明文比对、单一门级密码 |

### 1.2 本轮实测（本机 Ubuntu 22.04 / Qt 5.15.3 / OpenCV 4.5.4）

| 项 | 结果 | 证据级别 |
|---|---|---|
| A 的单元测试 | **`检查 500 项，失败 0 项`**（把 A 的 `src/`+`test/` 拷到 `/tmp` 后 `qmake test.pro && make && ./accessterminal_test`） | 【实测/本轮】 |
| B 的静态校验 `python3 tools/verify.py .` | **rc=0**，11 项告警（7×`fp-ack-tag`、3×`dead-slot`、1×`config-knobs`） | 【实测/本轮】 |
| B 的跨仓库校验 `tools/check_protocol_sync.py` | **rc=0** | 【实测/本轮】 |
| B 的 18 个 ctest 目标 | **本轮未复跑**。`cmake -S tests` 在本机因缺 `Qt5SerialPort` dev 包而配置失败 —— 这是 `docs/交接与继续开发.md` 第五节**已经记录**的 P8 环境缺口，本次为复现，不是新问题 | 【实测/本轮】 |
| A 侧数据竞争、抽象未兑现等结论 | 由本轮只读勘察给出，**未逐行复核** | 【勘察/未复核】 |

### 1.3 A 仓库里真正值得搬的五样东西

1. **`AccessController`：单一放行出口**。五条认证路径全部经过
   `unlock()`，统一维护"放行静默期"，UI 只收语义化信号。
   （A：`src/core/accesscontroller.h` 头部注释、`accesscontroller.cpp:677/684`）
2. **执行器与指示灯分离**。A 明确区分"真实开锁"与"模拟开锁"
   （`door.relayLine = -1` + 日志里标记 `模拟开锁（板端未接门锁执行器）`
   + `realActuator` 信号），并写明"PE5 是补光灯，不是门锁"
   （A：`README.md` 第 6 节、`src/core/configmanager.h:112/125`）。
3. **凭据安全模型**。加盐 SHA-256、落盘无明文、错误计数按 userId 隔离、
   锁定到期自动解锁、临时访客码一次性消费
   （A：`src/core/passwordmanager.cpp:12/38/103-104/153`）。
4. **有界流水线与新鲜度纪律**。"跨线程只传容量 1 的最新值，不排队"，
   加上帧代际号防旧图覆盖新图（A：`src/utils/latestvalue.h`、
   `v4l2camera.h:171`）。**只借鉴语义，不搬代码**（见第二节）。
5. **构建预检纪律**。SDK 预检不过就绝不开始编译、`-dumpmachine` 断言
   拿到的不是宿主 `g++`、产物 `file` 断言 `ELF 32-bit ARM`、
   在纯 ASCII 路径下构建以规避 Qt 5.12 moc 的非 ASCII 路径缺陷
   （A：`deployment/build-t113.sh`）。

---

## 二、借鉴清单 / 不借鉴清单

### 2.1 借鉴

| # | 借鉴内容 | 来源 | 落到本项目哪里 |
|---|---|---|---|
| 1 | 唯一放行出口 + 放行静默期 | A `AccessController` | 新增 `src/managers/access_controller.*`（P1） |
| 2 | 执行器/指示灯分离 + 真实/模拟开锁可观测 | A `GPIO::unlockDoor` + `realActuator` | 新增 `src/managers/door_actuator.*`（P0） |
| 3 | 非阻塞脉冲开锁（不 `msleep` 冻 GUI） | A `accesscontroller.cpp:715-718` | 同上（P0） |
| 4 | 加盐 SHA-256 + 按用户计数 + 锁定到期 + 临时码 | A `PasswordManager`/`UserDatabase` | 新增 `src/managers/credential_store.*`（P2） |
| 5 | 人脸质量门（清晰度/亮度/对比度/占比） | A `FaceQuality` | 新增 `src/managers/face_quality.*`（P4） |
| 6 | 连续命中平滑（防单帧假阳性） | A `FaceMatcher` 的 streak 机制 | `AccessController` 内状态机（P4） |
| 7 | "容量 1 最新值 + 代际号"作为**约定** | A `LatestValue`/`m_streamGeneration` | 写成开发约定第 72 条 + 规则（P3） |
| 8 | 构建预检纪律（4 条） | A `deployment/build-t113.sh` | `deploy/build-t113.sh`（P6） |
| 9 | "缺陷台账"形式（旧行为/新行为逐条对照） | A `README.md` 第 7 节（40 条） | `docs/借鉴核对表.md`（P5） |

### 2.2 **坚决不借鉴**（同样重要，避免走弯路）

| 不搬 | 原因 |
|---|---|
| A 的 `MemPool` / `BytePool` / `ThreadPool` | A 生产代码**零使用**（只有它的测试引用），且勘察发现对象生命周期错误与真实内存缺陷；搬过来等于搬债 |
| A 的 ONNX 人脸后端 | A 自己标注"实测不同输入余弦相似度普遍 0.99，必须先现场标定才可用" |
| A 的 qmake 工程 | 本项目的 CMake + 可独立构建的 `tests/` 更好，且已有 `verify_subcmake.py` 在守 |
| A 的"依赖注入"写法 | A 的 HAL 没有接口基类，setter 只能传具体类型指针，实际只能靠 `nullptr` 降级 —— 承诺未兑现。本项目用影子头 + 链接期替身是**真替换**，更实在 |
| A 的文档组织方式 | A 的 `DEVELOPMENT_PLAN.md` 引用的 5 个头文件全不存在；`项目工程化清单.md` 通篇过期。本项目的"约定 + 交接"更可信 |
| A 的仓库形态 | 无 git、88.5MB 产物入库、绝对路径写进 `.pro` —— 作为反面教材写进 P5 的核对表 |

---

## 三、分阶段方案

> 排序原则：**先把功能正确性问题止血，再装修架构**。P0 之所以排最前，
> 是因为它不是设计问题，而是"门到底开不开"的问题。

### P0 · 门锁语义裁决与执行器分离（0.5 天）

#### 为什么最紧急

两个仓库对**同一个引脚**给出相反的事实判断，而本项目代码里**找不到开锁执行路径**：

- A 仓库（自称板端实测）：`PE5(133) 是人脸识别补光灯，不是门锁`；
  `door.relayLine` 默认 `-1` 表示门锁执行器未接线，此时开锁只播放提示音，
  并在日志中标记为模拟开锁。
- B 仓库（本项目）：`src/config/app_config.h:81` 写的是
  "开锁继电器与补光灯共用的 GPIO 线号（硬件上为同一引脚 PE5）"。

而本项目全仓 `m_auxLine` 只有 5 处使用，**全部是补光**：

| 位置 | 语义 |
|---|---|
| `src/ui/widget.cpp:306` | 人脸采样第 1 张 → 补光开 |
| `src/ui/widget.cpp:311` | 采样第 3 张 → 补光关 |
| `src/ui/widget.cpp:323` | 人脸识别结果回来 → 关 |
| `src/ui/widget.cpp:715` | 挂断/退出 → 关 |
| `src/ui/widget.cpp:896` | 有人但未检出人脸 → 开 |

认证成功侧：指纹成功只弹 `QMessageBox`（`widget.cpp:723`）、密码成功只播音、
收到室内机 `unlock` 只播 `opendoor.pcm`（`widget.cpp:582-588`）。

**两种可能，都是问题**：

- 若 PE5 确是继电器 → 现在是"采集人脸时给锁上电、识别成功时把锁关掉"，正好反了；
- 若 PE5 只是灯 → 认证成功根本不驱动门锁。

而 71 条约定与 48 项静态检查里，**没有任何一条守住"补光"与"开锁"的语义区分**。

#### 要做的事

1. **板端裁决（必须由人做，代码改不了这个）**：确认 PE5 是灯还是继电器、
   门锁执行器实际接在哪条线。结论写进 `docs/交接与继续开发.md`。
2. **新增 `src/managers/door_actuator.h/.cpp`**（借鉴点 2、3）：

   ```cpp
   // 语义：只管"开锁"这一件事，不掺补光；非阻塞脉冲；可观测真实/模拟。
   class DoorActuator : public QObject {
       Q_OBJECT
   public:
       explicit DoorActuator(QObject* parent = nullptr);
       void setLine(GpioLine* line);      // nullptr = 未接执行器（模拟开锁）
       void setDurationMs(int ms);        // 出口 clamp，沿用本项目配置风格
       bool isReal() const;
       void unlock(const QString& source); // 立即返回，单次 QTimer 拉低
   signals:
       void pulsed(const QString& source, int durationMs, bool realActuator);
       void released(const QString& source);
   };
   ```

3. **`Widget` 侧改名拆义**：`m_auxLine` → `m_fillLight`（保持 P0 前那 5 处
   行为**完全不变**），新增 `m_lockLine` 装配给 `DoorActuator`。
   若裁决结果是"PE5 就是继电器"，则补光与开锁必须二选一 —— 这是一次
   **现场行为变更**，按本项目既有口径需**显式决策**并记录。
4. **新增静态规则** `tools/verify_actuator_separation.py`：
   失败判据 = 开锁路径中出现裸 `setValue(`；或 `fillLight`/`m_auxLine`
   标识符出现在任何 unlock/door 函数体内。
5. **新增故障注入夹具** `tools/verify_actuator_separation_rule.py`：
   向仓库副本注入"把开锁写成 `setValue(1)`"，断言规则**必须变红**。
6. **新增 ctest `t113_door_actuator`**（复用现成的 `tests/fake_gpiod.cpp`）：
   断言脉冲宽度正确、`unlock()` 返回耗时 < 1ms（防"冻 GUI 一秒"重演）、
   脉冲期间重复 `unlock()` 只产生一次物理脉冲、`line=nullptr` 时
   `realActuator=false` 且不崩。

**验收门**：`verify.py` rc=0 且新规则 0 告警 · `ctest -R door_actuator` 绿 ·
新规则夹具能变红。
**回滚**：单个提交，`git revert` 回到现状。

> **实施记录（第 40 轮，已完成）** —— 与上面的计划有三处偏差，按实际落地为准：
> 1. 静态规则不是独立脚本，而是 `tools/verify.py` 里的
>    `check_door_actuator_is_the_only_unlock_path`（本工程的规则一律住在
>    verify.py，`tools/verify_*.py` 是**夹具**），标签 `[actuator]`；
> 2. 脉冲宽度暂用 `widget.cpp` 里的 `kUnlockPulseMs = 1000`（重构前的现场
>    语义），**没有**加 AppConfig 旋钮 —— 加旋钮要连带遵守开发约定第 10 条
>    （公开的开关必须真的生效），留给显式决策；
> 3. `setLine(nullptr)` 是刻意的：在 PE5 事实裁决之前**不接任何线**，
>    因此 unlock() 走模拟开锁，物理行为零变化（只多了日志与信号）。
>
> 实测：`ctest` 19/19 通过（109.51s）· 全部 19 个 `.cpp` 主机端语法检查通过 ·
> `verify.py` rc=0（48 项检查、11 项告警）· 三条硬约束的故障注入
> 3/3 变红（`tools/verify_door_actuator_test.py`）· 规则夹具 3 变异抓到、
> 2 对照不误报（`tools/verify_actuator_separation_rule.py`）。

---

### P1 · 单一放行出口：引入 `AccessController`（2–3 天）

#### 搬什么

A 的 `AccessController` 是它最有价值的部分：`Method{Face, Fingerprint,
Password, TempCode, Remote}`、唯一的 `unlock(userId, method, note)`、
放行静默期、以及给 UI 的语义化信号。

#### 怎么搬（**先提取、不改顺序**）

本项目的红线是现场行为逐字节（含音效时序）一致，所以**不重写**，用三个等价提交：

| 提交 | 内容 | 对应现状位置 |
|---|---|---|
| P1-a | 建 `src/managers/access_controller.{h,cpp}` 骨架（`unlock()` + 静默期 + 信号）；`Widget` 把"密码成功 / 指纹成功 / 人脸成功 / 收到 `unlock`"四条路径改为**只调用**它，音效仍原地播放 | `widget.cpp:330-360`、`:582-588`、`:723` |
| P1-b | 把 `setupModules()` 里的提示音策略整段搬进 `AccessController`（指纹 `identifyResult` 的 500ms 重排、`enrollResult` 的三段音顺序照抄） | `widget.cpp:234-386` |
| P1-c | 把 `updateFrame()` 的检测调度、识别冷却、补光时机搬进 `AccessController::onFrame()`，`Widget` 只负责 `setPixmap` | `widget.cpp:776-899` |

预期结果：`Widget` 成员从 42 降到 ~25，`widget.cpp` 从 923 行降到 ~550。

#### 顺手清掉自己查出来的病

`verify.py` 现在报 3 个 `dead-slot`（`FingerprintManager::startEnroll`、
`Widget::onFingerprintEnroll`、`Widget::onFingerprintIdentify`），本质是
**平行实现** —— 现场跑的是 `form_finger` 的内联状态机。P1 必须二选一：
接上，或删除并说明理由。**规则从 3 条告警变 0 条**才算做完。

#### 测试与规则

- 新增 `t113_access_controller`：5 条路径 × {允许 / 拒绝 / 静默期内重复 /
  无执行器降级}，用本项目现成的影子头机制驱动（`tests/shadows/`）。
- 新增 `tools/verify_widget_is_thin.py`：`widget.cpp` 行数上限 +
  禁止 `ui/` include `hal/`、`core/media/`、`core/vision/`。

**验收门**：分层在 UI 层重新成立 · `dead-slot` 归零 · 音效时序测试全绿。

---

### P2 · 凭据与安全模型（1.5 天）

#### 现状缺口

`src/managers/password_manager.cpp:42` 是 `const bool matched = (input == password);`
—— **明文比对**；密码来自 `AppConfig`，配置文件里是明文 `"5788"`
（`src/config/app_config.cpp:88/96`）。

#### 要做的事

1. 新增 `src/managers/credential_store.{h,cpp}`：加盐 SHA-256
   （`QCryptographicHash`，Qt 5.9 即有）+ `QSaveFile` 原子写 JSON +
   按 userId 计数 + 锁定到期 + 临时码（有效期配置化）。
2. `PasswordManager` 保留"门级密码"语义（现场就是这么用的），改为存哈希；
   `AppConfig` 里的默认值**只用于首次初始化**。
3. **一次性迁移**：首次启动检测到明文 → 写回哈希 + 打一条明确日志（不得静默）。
4. 规则双保险：保留 `verify_password_log_rule.py`（静态），新增运行期断言，
   再加"配置文件里不得出现明文密码"的检查。

**验收门**：`t113_password_manager` 扩充 + 新增 `t113_credential_store` ·
明文扫描规则绿 · 密码不进日志的静态 + 行为双证。

---

### P3 · 并发与"最新值"约定（0.5 天）

**只借鉴语义，不搬代码**（理由见 2.2）。把 A 的"跨线程只传容量 1 的最新值，
不排队"写成**开发约定第 72 条**，并配静态规则；若 `src/core/vision/v4l2_camera.*`
缺"代际号防旧帧"，补一个 `m_frameGeneration`。

---

### P4 · 人脸质量门与防假阳性（1 天）

1. 新增 `src/managers/face_quality.{h,cpp}`：清晰度（Laplacian 方差）/ 亮度 /
   对比度 / 占比，纯逻辑；测试目标按本项目惯例**缺 OpenCV 时自动跳过**
   （`t113_face_manager` 已有先例）。
2. `AccessController` 里加"连续命中平滑"（`requiredStreak` / `windowMs`），
   与 P1 的放行静默期配合。
3. **ONNX 后端明确不引入**，只把结论写进配置注释与文档，防止后来者
   "以为接了就能用"。

---

### P5 · 工程卫生与文档对齐（1 天）

| 事项 | 具体内容 |
|---|---|
| Qt 5.9 API 违规 | `src/ui/widget.cpp:865` 的 `QMetaObject::invokeMethod(obj, functor, type)` 是 **Qt 5.10+** 才有的重载，与"只用 Qt 5.9 API"的硬约束冲突 → 改为字符串形式的 `invokeMethod`；并给 `tools/verify.py` 的 `check_qt5_portability` 补这条重载的判据 |
| README 三处失真 | ①配置查找顺序写成 4 级、实现只有 2 级；②人脸分类器路径在三处不一致；③`run_interop.sh` 实为 MSYS2 专用（README 却作为本机入口推荐） |
| 噪声注释 | 5 个遗留模块里 275 行机器生成的 `// 逐行注释：…`，其中部分已失真（如 `video_push_thread.h:12` 写的头文件路径已过时） |
| 跨平台缺口 | 交接文档第五节记录的 `tools/devstubs/sys/socket.h` 里 `#include <_mingw.h>`（属 P8），建议与本阶段一并修掉 |
| 缺陷台账 | 把 A 的 **40 条缺陷台账**逐条对照本项目，产出 `docs/借鉴核对表.md`：哪些本项目也存在、哪些已修、哪些不适用 |

---

### P6 · 交叉编译预检（1 天 + 上板 0.5 天）

**直接照搬 A 的纪律**（这是 A 最实打实的资产）：

1. SDK 预检：qmake / musl 头 / `videodev2.h` / OpenCV（含 `opencv2/face.hpp`
   与 `libopencv_face`）/ libgpiod / ALSA，**预检不过绝不开始编译** ——
   让"工具链没装全"和"代码编译不过"是两种不同的报错。
2. `-dumpmachine` 断言拿到的是 `arm*-musl*` 而不是宿主 `g++`。
3. 产物 `file` 断言 `ELF 32-bit ARM`。
4. 在**纯 ASCII 的 /tmp 路径**下构建，规避 Qt 5.12 moc 的非 ASCII 路径缺陷。

这一阶段同时是交接文档里【P0：真正的交叉编译与目标板验证】。

---

## 四、每阶段统一门禁

```sh
# 0) 本机一次性环境准备（见 docs/交接与继续开发.md 第五节，两处 dev 包缺口）
python3 tools/verify.py .                     # 必须 rc=0
python3 tools/check_protocol_sync.py          # 必须 rc=0（protocol.h 全程不动）
python3 tools/check_signatures.py .
bash tools/check_host_compile.sh
cmake -S tests -B /tmp/t113-tests -G Ninja && cmake --build /tmp/t113-tests
ctest --test-dir /tmp/t113-tests --output-on-failure
# 新增的每条规则都必须自带故障注入夹具，并且夹具能把它变红
```

沿用 README 的六条"做完了"标准，其中与本方案最相关的两条：

- **新测试必须配故障注入**，在缺陷代码上不失败的测试没有价值；
- **机制必须用真实实现**，别复制一份进替身。

---

## 五、建议的前三个提交

```
1. feat(door): 拆开补光灯与开锁执行器，新增 DoorActuator（非阻塞脉冲）
   + tools/verify_actuator_separation.py + 自检夹具 + t113_door_actuator
   顺带补上"补光与开锁语义无区分、无规则守护"这个已知缺口

2. docs: 记录 PE5 与门锁执行器的板端裁决结论、以及由此产生的现场行为变更

3. refactor(access): 引入 AccessController，四条认证路径收敛到唯一 unlock()
   只做等价提取，音效时序不变；verify.py 的 dead-slot 归零
```

---

## 六、需要显式决策的事项

| # | 决策点 | 为什么必须由人决定 |
|---|---|---|
| 1 | PE5 是补光灯还是门锁继电器？门锁执行器接在哪 | 代码与文档给出相反事实，只能上板确认；结论直接改变现场行为 |
| 2 | 若 PE5 是继电器：补光与开锁二选一，保留哪个 | 涉及硬件接线与产品体验，不是纯代码决策 |
| 3 | 3 个 `dead-slot`（`FingerprintManager::startEnroll` 等）接上还是删除 | 涉及是否保留公开 API，README 第 23 轮条目要求"刻意保留需在注释里说明" |
| 4 | 人脸质量门与命中平滑的阈值 | 必须现场用真实人脸标定，本机合成数据不可作为依据（沿用交接文档 P9 的口径） |
| 5 | 是否值得引入临时访客码（需要新增 UI 入口） | 功能取舍 |

---

## 七、本方案的边界

- 本方案**不修改** `protocol.h`、端口、信令、UDP 帧格式与音效时序。
- 本方案**不替代**交接文档第三节的待办清单（P0~P9）；两者关系是：
  交接文档说"还差什么"，本文说"借鉴谁、按什么顺序、怎么验收"。
- 本文中标注【勘察/未复核】的 A 仓库结论（如并发竞争的具体位置），
  在真正动手搬运前应逐条复核，不要直接引用为事实。
