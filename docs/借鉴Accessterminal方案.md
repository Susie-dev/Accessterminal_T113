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

## 进度总览（截至第 42 轮）

图例：✅ 完成 · 🟡 部分完成 · ⬜ 未开始。**每完成一步都要在这里与对应小节的
"实施记录"里同时更新**，否则下一个人只能靠翻 git log 猜进度。

| 阶段 | 状态 | 证据（提交 / 可执行验证） |
|---|---|---|
| **P0** 门锁语义与执行器分离 | ✅ 代码完成，**PE5 待板端裁决** | `214fd79` · 新测试 `t113_door_actuator`（3 条硬约束各配故障注入，3/3 变红）· 规则 `[actuator]` 夹具 4/4 抓到 |
| **P1** 单一放行出口 `AccessController` | 🟡 **P1-a 完成**；P1-b/P1-c（提示音策略与人脸调度位移）未做 | `ae2b52f` · 新测试 `t113_access_controller` · `dead-slot` 告警 3 → 1 |
| **P2** 凭据与安全模型 | 🟡 **存储层完成**；锁定旋钮与临时码**未接 UI/未接解锁路径**（见该节实施记录） | `src/managers/credential_store.*` 新增 · `password_manager.*` 改哈希校验 · 新测试 `t113_credential_store` 39/39 · `t113_test_password_manager` 16/16（含 5 条迁移路径）· 规则 `[credential]` 夹具 **12/12** 变红 · 行为夹具 **12/12** 变红 · `credential_store.cpp` 行覆盖 91.8% |
| **P3** 并发与"最新值"约定 | ✅ 完成 | 开发约定 **第 77 条** · 规则 `[latest-value]` · 夹具 **8/8** 变红、2/2 对照不误报 · `t113_test_v4l2_camera` 29 → **34/34** · 修真实缺陷：人脸检测**无背压闸门**导致任务无界堆积（`compare_exchange` 丢弃式闸门）· 消除 `onFaceDetectResult` 的整帧深拷贝（1.2 MB × 2 Hz） |
| **P4** 人脸质量门与防假阳性 | ✅ 完成（ONNX 明确不引入） | 开发约定 **第 79 条** · 规则 `[face-anti-fp]` · 夹具 **10/10** 变红、3/3 对照不误报，**外加运行期语义证明** · 新增 `src/managers/face_quality.{h,cpp}`（**不依赖 OpenCV**）· `t113_test_face_quality` **18/18** · `t113_test_access_controller` 6 → **20/20** · `t113_test_app_config` 13 → **18/18** · 修真实缺陷见下 |
| **P5** 工程卫生与文档对齐 | 🟡 5 项已完成，2 项未做（见该节实施记录） | `8308121`（Qt 5.9 重载）· `df22048`（devstubs 跨平台）· `ae2b52f`（`check_host_compile.sh` 支持 Linux、README 两处失真）· **分类器路径不一致已修**（第 44 轮，规则 `[resource-path]`，夹具 10/10） |
| **P6** 交叉编译预检 | ✅ 完成 | `18092db` · 首次交叉编译成功：`ELF 32-bit ARM`，516,644 字节，依赖闭包 **13/13** |
| 五、建议的前三个提交 | ✅ 全部完成 | `7259117`（方案文档）· `8308121`（Qt5.9）· `214fd79`（P0） |

**当前门禁水位（第 44 轮，本机 Windows/MSYS2）**：`ctest` **23/23**（第 43 轮
新增 `t113_face_quality`、第 44 轮新增 `t113_resource_paths`）· 主机端类型检查
**22/22** · `verify.py` rc=0（**54 项检查**、10 项告警、0 失败）·
`check_signatures.py` rc=0 · `check_protocol_sync.py` rc=0 ·
cppcheck 无 warning/error · **Debug/`--coverage` 构建也通过**（第 41 轮新加）·
桌面仓库 `ctest` 7/7 · 跨仓库互操作 14/14 ·
就地改写源码的夹具 **18 个**（统一一把锁）·
近似总覆盖率 **56.1%**（每轮按函数重算，**不要抄旧数字**）。

> **第 41 轮补的一条门禁：Debug 配置也要过。**
> 本项目的门禁此前**只用 Release**。而 `credential_store.cpp` 里
> `qBound(kMinIterations, ...)` 会把类内初始化的 `static const int`
> **ODR-使用**掉 —— C++11 要求它另有类外定义。Release（-O2）把值内联，
> 链接照样过；`--coverage -O0` 不内联，两个测试目标立刻
> `undefined reference`。也就是说
> **"Release 编译通过"并不等于"所有构建配置都能通过"**，
> 而覆盖率构建恰好是 Debug，这才把它暴露出来。
> 修复：在 `credential_store.cpp` 补齐四个类外定义（带注释说明别删）。
> 教训已写进 README 开发约定第 72 条。

**下一件该做的事**：P2 的**接线决策**（锁定旋钮与临时码要不要接到
UI/解锁路径 —— 目前刻意只做存储层，见该节"未做的部分"），或 P1-b/P1-c
（纯位移、风险低）；都不依赖板端裁决。板端裁决（PE5）与上板运行是另一条线，
见 `docs/交接与继续开发.md` 的【决策】与【P0】。

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

> **实施记录（第 40 轮，已完成；提交 `214fd79`）** —— 与上面的计划有三处偏差，按实际落地为准：
> 1. 静态规则不是独立脚本，而是 `tools/verify.py` 里的
>    `check_door_actuator_is_the_only_unlock_path`（本工程的规则一律住在
>    verify.py，`tools/verify_*.py` 是**夹具**），标签 `[actuator]`；
> 2. 脉冲宽度暂用 `widget.cpp` 里的 `kUnlockPulseMs = 1000`（重构前的现场
>    语义），**没有**加 AppConfig 旋钮 —— 加旋钮要连带遵守开发约定第 10 条
>    （公开的开关必须真的生效），留给显式决策；
> 3. `setLine(nullptr)` 是刻意的：在 PE5 事实裁决之前**不接任何线**，
>    因此 unlock() 走模拟开锁，物理行为零变化（只多了日志与信号）。
>
> 实测（**当时水位**；P1 之后目标数变为 20 个，当前水位见开头「进度总览」）：
> `ctest` 19/19 通过（109.51s）· 全部 19 个 `.cpp` 主机端语法检查通过 ·
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

> **实施记录（第 40 轮，部分完成；提交 `ae2b52f`）** —— 实际落地与计划的差异：
> 1. 本轮做的是 P1-a：新增 `AccessController`（放行记账 + 静默期 + 语义化
>    信号），四条认证路径收敛到它；**P1-b/P1-c（把提示音策略与人脸检测调度
>    搬进来）尚未做** —— 那两步是纯位移、风险集中，单独一轮做更稳。
> 2. 静默期默认 **0 = 保持现状**，取非 0 属行为变更，已记为待裁决事项
>    （见 `docs/交接与继续开发.md` 的【决策】）。
> 3. `dead-slot` 从 3 条降到 **1 条**（不是 0）：两个 `Widget` 死槽已删，
>    剩下的 `FingerprintManager::startEnroll()` 是公开 API + 有测试，
>    接不接线需要拍板，已记为【决策】第 3 条。
> 4. 顺手修掉了 P0 的一个疏漏：`requestUnlock` 原先被写在**不可达**的
>    `Widget::onFingerprintIdentify` 里，等于指纹成功并不开锁。本轮把开锁
>    接到**现场真正执行**的 `identifyResult` lambda 上，并删掉那两个死槽。
> 5. 出口链变成三级（widget → AccessController → DoorActuator），规则
>    `[actuator]` 的第 ③④ 项随之改成"widget 必须调中枢"+"`->unlock(` 只能
>    出现在 `access_controller.cpp`"。**这条规则的第一版被夹具抓到假绿**：
>    它只匹配 `m_actuator->unlock(`，改成 `m_doorActuator->unlock(s)` 就漏报，
>    现已按调用形式匹配（4/4 变异抓到、2/2 对照不误报）。
>
> 实测：`ctest` 20/20（110.64s）· `verify.py` rc=0（9 项告警，其中 8 项是既有的
> `fp-ack-tag`，1 项是上面那条待裁决的 `dead-slot`）· 20/20 个 `.cpp` 主机端
> 语法检查通过。

---

### P2 · 凭据与安全模型（1.5 天）

> **状态（第 40 轮末）：⬜ 未开始。** 下面写的是设计，一行代码都还没落地。

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

#### 实施记录（第 41 轮）

> **状态：🟡 存储层完成并已上测试；接线未做。**
> 上面"要做的事"1~4 全部落地；但**锁定旋钮与临时码刻意没有接到 UI 与解锁
> 路径**（用户显式选择"只做存储层，不接 UI"）。见下方"未做的部分"。

| 设计项 | 落地形态 |
|---|---|
| 加盐 SHA-256 | `CredentialStore::hashSecret(plain, salt, iterations)`，格式 `sha256:<盐hex>:<迭代数>:<摘要hex>`。每用户**独立**盐；`makeSalt()` 优先读 `/dev/urandom`，取不到退回重复 `QUuid`（**刻意不用 `qrand`/`QRandomGenerator`** —— 前者不是密码学随机源，后者是 Qt 5.10+，越过 5.9 底线） |
| 原子写 | `QSaveFile`（临时文件 + `commit()` 改名），写完 `setPermissions(ReadOwner\|WriteOwner)`。理由：门口机可能被直接断电，非原子写会留下半截 JSON → 下次启动解析失败 = "门忽然打不开" |
| 按用户计数 / 锁定到期 | `failed` 与 `lockedUntilMs` 按 userId 存。**默认 `maxAttempts=0` = 从不锁定**，即"未配置时行为与重构前一致"；启用锁定是**显式**行为变更。`setLockoutMs` 上限 24h（防配置失误把门永久锁死） |
| 临时码 | `issueTempCode` / `consumeTempCode` / `purgeTempCodes`：一次性 + 到期即失效 + 去重重试（上限 100 次，防码空间占满时挂死）。**只做存储层**，未接 UI |
| `PasswordManager` 改哈希 | 成员从 `QString password` 改为 `CredentialStore m_credentials`；比对走**定长比较**（`constantTimeEquals` 累积异或）。对外信号契约**逐条不变** |
| 非静默迁移 | 三条分支各打日志：①凭据文件已有效 → `qInfo` 说明配置文件不再参与；②配置值**已是哈希串** → 直接装入，**不报**"明文"（否则运维把哈希拷进配置后会被当明文再哈希一遍，谁也开不了门）；③明文 → `qWarning` 明说"检测到**明文**引导值，已现场加盐哈希"，**且绝不打印明文本身** |
| 明文不写回配置 | 哈希写进**独立的** `credentials.json`（与 `appconfig.json` 同目录），那是运维手工编辑的文件、程序不改写它。运维首次启动后即可删掉 `security.door_password`（删掉仍能开门） |
| 配置项不得明文 | 规则 `[credential]` 第 ③ 条：`config/appconfig.json` 里若 `security.door_password` 不是哈希串则**告警**（不失败 —— 删字段是运维动作，不该卡构建） |

**新增测试与规则**

- `tests/test_credential_store.cpp`，新目标 `t113_credential_store`：**39 条**用例，
  行覆盖 **91.8%**。
- `tests/test_password_manager.cpp` 由 12 条扩到 **16 条**，新增 **5 条迁移路径**
  用例（明文迁移告警、哈希引导值直接装入且**不**报明文、无引导值时一律拒绝、
  `setPassword` 覆盖迁移值、以及"任何分支都不泄漏明文"）。
- `tools/verify.py` 新增两条规则：`check_credentials_are_hashed_not_plaintext`
  与 `check_credential_store_test_is_wired`。
- 两个故障注入夹具：`tools/verify_credential_rule.py`（静态规则）**12/12** 变异被抓；
  `tools/verify_credential_store_test.py`（真编译真运行）**12/12** 变异被抓。
  两者都带"对照必须保持绿"与"还原后逐字节一致"的守卫，并共用仓库级文件锁。

**故障注入抓出的问题（本轮最有价值的部分）**

| # | 抓到的对象 | 问题 | 处置 |
|---|---|---|---|
| 1 | **既有规则** `[hex-parse]` | 我在新代码里直接用了 `QByteArray::fromHex()`。它**不报错**，会跳过非法字符重新配对 —— 篡改过的哈希串会"成功"解出**内容全错**的字节，比解析失败更危险 | 新增 `isHexOnly()` 逐字符校验；包装函数**刻意不叫 `fromHex`**（否则其调用方会被该规则误判为未校验 —— 规则不做跨函数分析） |
| 2 | **我的静态规则** | 6 处**假绿**：`"add_test(...)"` 子串在**注释行**里也成立；`"managers/credential_store.cpp"` 在上方注释块里就出现过；用例改名成 `..._REMOVED` 后裸名字子串仍在；只查 `"salt" in text` 时把函数签名里的 salt 也算进去了 | 改为**跳过注释行** + **按括号配平取那一个 `add_executable` 调用块** + 匹配**函数定义**而非裸名字 + 查 `toHex(salt)` 与 `iterateDigest([^)]*salt` |
| 3 | **我的行为夹具** | "失败时不累加计数"被写成 `++r->failed; ++r->failed;`（**加倍**），根本不是它声称的退化，且加倍后更容易触发锁定 | 改为**整段删除** `++r->failed;` |
| 4 | **我的行为夹具** | 对照项 `old == new`，被自己的"变异未落到磁盘"守卫拦下、报成夹具失败 | 对照改为真的改动字节 |
| 5 | **我的测试** | 用 `hashSecret(plain, salt, n)` 观测 `setSecret()` 的结果 —— 盐是**测试自己**传的，所以把 `setSecret()` 内部改成固定盐，断言**照样绿** | 改用 `toJson()` 观测（顺带验证"盐确实进了落盘内容"），新增 `setSecretUsesAFreshSaltEachTime` |
| 6 | **我的测试** | "到期自动解锁"的清理逻辑**删掉也全绿** —— `isLockedOut()` 是算出来的，时间一过自然为 false，看着像摆设 | 新增 `failureCountRestartsAfterLockoutExpires`：期满后再错**一次**，计数必须从 1 起；否则"刚解禁就被一次手误重新锁死" |
| 7 | **我的测试** | 非法字符的构造让 `fromHex` 跳过非法字符后**长度也对不上**，长度判据掩盖了字符判据 | 构造改为"32 个合法字符 **+** 2 个非法字符"，跳过非法字符后长度**恰好正确** —— 唯一能拒绝它的理由只剩逐字符校验 |
| 8 | **我的测试** | 断言"锁定期满后 `failedAttempts()==0`"失败 —— 清零是**惰性**的（发生在下一次 `verify()`），我把"惰性复位"当成"即时复位"。**测试写错**，不是实现错 | 保留 `isLockedOut()` 断言，把"计数清零"的观测点挪到下一次 `verify()` 之后 |
| 9 | **Release-only 门禁** | 类内初始化的 `static const int` 被 `qBound()` **ODR-使用**，C++11 需要类外定义。Release（-O2）内联所以链接过；`--coverage -O0` 不内联 → `undefined reference`，两个目标链接失败 | 补齐四个类外定义（原地留注释说明别删）；门禁补上 **Debug 配置**；教训写进 README 第 72 条 |
| 10 | **既有脚本** `check_host_compile.sh` | `-isystem $QTSP_INC` 在 `qmake` 缺失时退化成裸 `/mingw64/include`，**遮蔽 libstdc++**（`#include_next <stdlib.h>` 找不到），21 个文件全误报 | `QTSP_INC` 默认回到 `QT_INC`，并从 flags 去掉那个裸 `-isystem`；修完 **21/21** 通过 |
| 11 | **既有测试写法** | 该脚本用环境变量无法表达"空引导值"（`envOverride()` 是 `if (!raw.isEmpty())`，设空串等于没设，会退回内置默认 `"5788"`） | 改用**真实配置文件**表达空串（`writeConfigWithDoorPassword()`），并把这一事实写成注释 |
| 12 | **`verify.py` 的剥离器**（影响**全部 50 条规则**） | 预处理顺序是"块注释 → 字符串 → **字符字面量** → 行注释"。行注释里一个**孤立撇号**（`` ...kMinIterations' ``）与几千行之后的真字符字面量配对，**一次吃掉整段代码**：某文件 20184 → 4349 字节，`toHex`/`iterateDigest`/`salt` 全部消失，依赖它们的规则集体误报 | 把**行注释提前到字符字面量之前**（此时字符串已清空，`"http://..."` 不会被误当注释）。新增 `tools/verify_stripper_robustness.py`（20 项，**自带反证**：把顺序改回去必须变红，实测 7 项 FAIL）。教训写进 README 第 73 条 |
| 13 | **夹具的"过期副本自愈"**（第 38 轮事故重现） | 强杀（Windows `TerminateProcess`）**必定**留下 `.in-progress` 哨兵，下次启动拿**旧副本**"自愈"。实测把 `tests/shadows/ui/widget.h` 从 8421 字节回滚成 **7528 字节**的第 37 轮之前版本，抹掉影子头全部新增内容，随后 23 个变异因"锚点找不到"全红 | 抽出 `tools/fixture_pristine.py`：磁盘副本 + 哨兵 + **新鲜度校验**（标记里记长度，长度不符则**拒绝自愈**并告警，宁可留污染也不删合法改动）+ 锁接管 + atexit/信号。5 个夹具迁移；新增 `tools/verify_fixture_pristine.py`（26 项）。教训写进 README 第 74 条 |
| 14 | **10 个夹具只有 `finally` 保护** | `verify_form_call_test.py` 的自愈判据还是最危险的"副本 != 当前文件"，而且**完全没有锁**；另有 9 个就地改写真实仓库文件却只靠 `finally`（§61 的老坑） | 全部接上 `PristineGuard` 的薄封装（内部一行未改）；`verify_form_call_test.py` 补上原先缺失的互斥锁 |
| 15 | **`os.kill(pid, 0)` 不是存活探测**（Windows） | 实测：pid **曾经存在、现已正常退出**时它**不抛异常**，被判为"活着"。于是被强杀留下的陈旧锁**永远无法接管**，之后**每个**夹具都打印"另一个夹具正在改写源码"然后拒绝干活 —— 整个自检体系静默停摆 | 改用 `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `GetExitCodeProcess`（非 Windows 退回 `os.kill`，POSIX 语义本来正确）。收敛到 `process_is_alive()`；自检里钉住四种情形。教训写进 README 第 75 条 |
| 16 | **双重锁把自己锁在门外** | `verify_door_actuator_test.py` 自带一套锁（早于 Guard），与 Guard 用的是**同一把** `fi37_rewrite.lock`。套上 Guard 后 Guard 先持锁，内层再拿就只看到"已存在 + 持有者活着" → **自己把自己锁在门外**，rc=2 | 锁本身改为同一进程内**可重入**（持有者 pid == 本进程则视为已持有，且 release 时**不删**别人的锁）；同时**删掉**该夹具冗余的内层锁，消除两份实现 |
| 17 | **两个最后的假绿**（`verify_password_log_test.py`） | P2 删掉明文成员后，该夹具的 8 个变异里 `<< input.toUtf8().toHex()` 与 `<< input.left(4)+"***"` **全绿通过** —— 因为断言只覆盖了"已存储口令"的 hex 与前缀，而产线代码里**已经拿不到明文口令**，真正剩下的泄漏口是**用户输入**本身，那两条恰恰没断言 | 变异改指 `input`（如实反映"现在只有这一个泄漏口"）；测试补齐**用户输入**的逐前缀与 hex 断言；输入串刻意选 `zz-wrong-77` / `zz-88`（与日志里唯一拉丁文本 `"[PasswordManager]"` 无公共前缀，避免前缀断言假红）。修后 **8/8** 全被抓 |
| 18 | **插入脚本用旧偏移切新字符串** | 给 10 个夹具插保险的脚本**先取 `def main():` 偏移、再往文件前面插 import** —— 插入让后续偏移右移，用旧偏移切新串就把 `def run_verify():` 从中间切断，**8 个文件同时语法错误** | 所有插入做完**之后**才搜索锚点；写出前 `compile()` 一次，不过就**拒绝写回**。教训写进 README 第 74 条末尾 |
| 19 | **夹具锚点会随被它盯着的代码一起过期**（2 处） | ① `verify_form_call_rule.py` 的"去掉影子头安全阀"锚点写的是**两宏**版本，而第 37 轮给影子头加了第三个宏（`ACC_FORM_FINGER_TEST`）→ 锚点**从第 37 轮起**就匹配不到，夹具报的是"锚点 0 次"而非"没抓到"；② `verify_dead_slot.py` 的期望值写死"变异后剩 **2** 条"，那是第 23 轮有 **3** 个不可达槽时的算术，此后 18 轮另两个槽被处理掉、基线降到 1，正确期望变 **0**，旧值**永远不可能满足** → 恒定假红 | 两处都按当前真实字节/真实基线更新。**并各加一条"精确基线"用例**：原来那条只断言 `>=1`，无法区分"基线 3 条"和"基线 1 条"—— 正是这个空档让过期期望藏了 18 轮。教训写进 README 第 76 条 |
| 20 | **夹具"锁忙"路径抛 `NameError`**（4 个夹具，**第 42 轮发现**） | 第 41 轮把还原/锁的实现换成 `PristineGuard` 时，提示文案里的 `% LOCK` 忘了删（该变量在新实现里不存在）。它只在**锁被别的活进程占用**时执行，平时夹具独占运行，这条路径**从不触发** —— 所以 43/43 全绿照样带着这个 bug。发作时恰是用户最需要清晰提示的时刻，却只看到 Python 回溯（**实测复现**：起一个真实存活的 sleeper 占锁，`NameError: name 'LOCK' is not defined`） | 4 处改为 `_GUARD.lock_path()`（公共实现新增该方法，只有一份真相）；清理 `verify_door_actuator_test.py` 里已无引用的 `LOCK`/`_acquire_lock`/`_release_lock` 三个死符号 |
| 21 | **同一批源文件有两个不同的锁名**（**第 42 轮发现**，真实互斥失效） | 审计所有 `PristineGuard` 夹具的 (锁名, 改写文件集) 组合时发现：`verify_credential_rule.py` 与 `verify_credential_store_test.py` **都改写** `credential_store.{h,cpp}`，却分别用 `fi37_rewrite.lock` 与 `fi37_write.lock`。**实测**：在 `fi37_rewrite.lock` 被占的情况下跑后者，它 **rc=0 正常跑完**并改写了同一批文件 —— 文档里写的"共用一把文件锁"并未真正成立 | 统一为 `fi37_rewrite.lock`。**并新增一条审计**（把 (锁名, 文件集) 交叉比对，任何"同文件不同锁"都是冲突），本轮的 2 处冲突已归零 |
| 22 | **"锁忙"退出码不统一**（2 vs 3，**第 42 轮发现**） | 4 个夹具返回 2、7 个返回 3，而 **2 已被这些夹具用来表示"夹具自身错误/锚点失效/基线红"**。调用方（批量清扫脚本、CI 门禁）无法区分"这次因并发没跑"和"这次跑了且真失败"，只能靠匹配输出文字 —— 而把并发跳过误当假绿，正是最危险的失败模式 | 统一"锁忙"为 **3**（2 保留给夹具自身错误），只改 `if not ...acquire_lock():` 块内的那个 return。实测 4 个夹具现在都是 `rc=3` + 可操作中文提示 + 含锁文件路径 |

#### 未做的部分（**刻意**，不是遗漏）

1. **锁定旋钮未接配置**：`maxAttempts` / `lockoutMs` 目前只能由测试设置，
   `AppConfig` 不读它们。理由：加公开旋钮就必须遵守开发约定第 10 条
   （"配置里公开的开关必须真的生效"），而"启用锁定"本身是**行为变更**
   （正确口令在锁定期内也会被拒），应由运维显式决策，而不是随本次重构
   悄悄生效。默认 `maxAttempts=0` 保证现场行为不变。
2. **临时码未接 UI**：`issueTempCode` / `consumeTempCode` 只在存储层，
   没有窗体、没有按键、没有解锁路径接入。**这属于"已实现但生产代码零调用"**，
   覆盖率报告里会算作「不可达」—— 这是**如实反映**，不是缺陷。
   接线需要产品决策（谁能签发、有效期多久、几位数、要不要与呼叫中心联动）。
3. **多用户未启用**：`CredentialStore` 的 API 全按 userId 索引（为多用户准备），
   但 `PasswordManager` 只用一个固定 ID `"door"`。多用户需要 UI 决策。
4. **迭代哈希不是 PBKDF2/Argon2**：Qt5 没有 PBKDF2，本类也没有内存硬度参数。
   相对明文比对是实质提升，但**不能**用来保护高价值口令 —— 这条已写进
   `credential_store.h` 与 README，**不得**在别处被说成"用了 PBKDF2"。

---

### P3 · 并发与"最新值"约定（0.5 天）

> **状态（第 42 轮）：✅ 完成。** 约定写进 README **第 77 条**（原计划写"第 72 条"，
> 实际编号由当时已积累的约定数决定），规则 `[latest-value]` 已加并注册，
> 故障注入夹具 `tools/verify_latest_value_rule.py` **8/8 变异变红、2/2 对照不误报**。

**只借鉴语义，不搬代码**（理由见 2.2）。把 A 的"跨线程只传容量 1 的最新值，
不排队"写成**开发约定**，并配静态规则；`src/core/vision/v4l2_camera.*` 原先
缺"代际号防旧帧"，已补 `frameGeneration()` / `frameSize()`。

**实际落地内容**（比原计划多，因为调查发现了计划里没写到的三处缺陷）：

| # | 事项 | 证据 |
|---|---|---|
| 1 | `V4L2Camera::frameGeneration()`：代际号与 `lastFrame` **在同一临界区**内更新 | 新增测试 `frameGenerationAdvancesOnlyOnNewFrames`（含"解码失败/ DQBUF 失败时代际号**不得**变"）等 5 条；`tests/test_v4l2_camera.cpp` 29 → **34 条全过** |
| 2 | `V4L2Camera::frameSize()`：只返回 `QSize`，**不深拷贝整帧** | `frameSizeMatchesTheCachedFrame` / `sizeAndGenerationAreSafeWithoutAFrame`；由 `[latest-value]` 静态判据禁止其函数体出现 `getFrame()`/`QImage` |
| 3 | `Widget::onFaceDetectResult()` 原先为读 `width()/height()` 深拷贝整帧（640×480 RGB32 ≈ 1.2 MB，约 2 Hz），改用 `frameSize()` | 同一处代码；语义逐位一致 |
| 4 | **真实缺陷**：人脸检测 `QtConcurrent::run` 无背压闸门 → 任务在 `QThreadPool` 无界队列堆积（T113 单次 Haar 可达数百 ms，提交间隔约 500 ms），内存只增且检测框来自越来越旧的帧 | 新增 `m_faceDetectRunning`（`shared_ptr<atomic<bool>>` + `compare_exchange`）；RAII guard 释放（`cv::Exception` 是本工程现实存在的异常路径） |
| 5 | 规则**按提交点所在函数**判定，而非按文件 | 夹具 M8 专门钉住；实测把规则改成文件级即**漏报**（见下方"判别力证明"） |

**为什么闸门标志用 `shared_ptr<std::atomic<bool>>` 而不是裸成员**：任务可能比
`Widget` 活得久（`QPointer` 守卫正为此存在）。若任务持 `this` 去清标志，
对象先析构就是写已释放成员的 use-after-free。`AudioManager::interruptFlag`
用的是同一手法。**第一版我写成了裸成员 + 捕获 `this`**，自查时发现并改正 ——
这条记在这里，因为它是最容易"看起来没问题"的写法。

**两种都正确的闸门形态**（规则同时接受，避免误报）：
- **丢弃式**（人脸检测）：忙则直接放弃本次 —— 结果只是提示，丢了无所谓；
- **取消式**（`AudioManager::playAsync`）：提交前 `stop()` 打断上一次 +
  登记未完成的 future —— 每次都要有结果，但旧的不再需要。

**判别力证明**（`[latest-value]` 里唯一无法靠"改坏代码"证明的一点）：
规则若按**文件**判定，则"在已有闸门的文件里新增一个无闸门提交点"会**假绿**。
实测把规则临时改成文件级后，M8 确实从"红"变"绿"；改回函数级又变红。
没有这一步，M8 可能只是因为别的原因变红，证明不了什么。

---

### P4 · 人脸质量门与防假阳性（1 天）

> **状态（第 43 轮）：✅ 完成。** 规则 `[face-anti-fp]` · 夹具 **10/10**
> 变异变红、**3/3** 对照不误报 · 外加一条**运行期语义证明**（见下）·
> `t113_test_face_quality` 新增 **18/18**（且**不依赖 OpenCV**）·
> `t113_test_access_controller` 6 → **20/20** · `t113_test_app_config`
> 13 → **18/18**。开发约定拟写第 79 条（见 README）。

**为什么这一阶段是安全问题而不是体验问题**
本工程已实测确认（`tests/test_face_manager.cpp` 的结论）：LBPH 在单 label 下
`predictedLabel == 1` **恒为真**，于是 `face_confidence_threshold` 是**唯一的
门** —— 一张模糊、过曝或过暗的图只要其距离凑巧落进阈值，**门就开了**，
而门外站的可能不是注册者。质量门是识别之前的第二道判断。

**落地内容**

| 项 | 实现 | 关键决定 |
|---|---|---|
| 质量门 | 新增 `src/managers/face_quality.{h,cpp}`：清晰度（Laplacian 方差）/ 亮度 / 对比度 / 面积占比 | **不用 OpenCV**（见下），五项阈值默认全部"不限制" |
| 接入点 | `widget.cpp` 的人脸识别路径，在 `copy(faceRect).scaled()` **之前** | 先判质量可省掉不可用帧的缩放与 `predict()`（判据 B） |
| 启用语义 | `AppConfig::faceQualityEnabled()` 由五项阈值**推导** | **不做独立开关**（判据 C，见下） |
| 连续命中 | `AccessController::setRequiredStreak(method, required, windowMs)` | **按认证方式**独立（判据 F），窗口**从第一次命中起算**（判据 D） |
| ONNX | **明确不引入**，结论写进 `config/appconfig.json` 的注释 | 防止后来者"以为接了就能用" |

**四处与原计划的偏离，都有实测理由**

1. **质量门改为不依赖 OpenCV**（原计划"按本项目惯例缺 OpenCV 时跳过"）。
   实现时发现清晰度用四邻域 Laplacian 卷积只需十几行纯 C++。
   于是 `t113_test_face_quality` 在**任何**环境都真的跑。
   理由正是 §78：`t113_face_manager` 在本机跑得起来，但换一台没装 OpenCV
   face 模块的机器就会**安静消失**，而没人会注意到。规则里专门有一条断言
   "它不得被放进 `find_package(OpenCV)` 的可选分支"。

2. **"是否启用"由阈值推导，不做独立布尔开关。**
   独立开关会多出一份可以与阈值**互相矛盾**的状态：开关开而阈值全默认时
   门形同虚设（静默失效）；开关关而阈值已收紧时配置看起来生效了却没有。
   这类"两份状态"是误配的温床，单一事实来源消除整类问题。

3. **连续命中按认证方式独立，不是全局开关。**
   若做成全局，运维为了让人脸更严而设 `required_streak=2`，会**顺带**让
   "密码输一次不开门" —— 一个静默改变其它三条认证路径的误配。

4. **窗口从"第一次"命中起算，不是"最近一次"。**
   从最近一次起算的话，只要命中间隔小于窗口就能无限累积：一个人干站着
   （或一次持续误检）迟早会攒够 N 次，**门等于没锁**。这条是本阶段
   判别力最核心的语义。

**判别力证明：静态规则守形态，单元测试守语义**（本阶段最重要的方法论收获）
`[face-anti-fp]` 判据 D 只能判**结构形态** —— "有没有把首次/后续两个分支
分开"、"后续命中分支里有没有偷偷重置计时"。它**证明不了** `expired` 是不是
真的按窗口算出来的：把 `expired` 写死为 `false` 时，`hasFirst` 的名字仍在
分支条件与赋值里，文本判据区分不了。这一点是**实测发现**的（我原本以为
静态规则能守住窗口语义，夹具把它证伪了）。

因此 `tools/verify_face_anti_fp_rule.py` 除了 10 个静态变异之外，还做一次
**运行期变异**：把实现改成"从最近一次起算"，重新编译并运行
`t113_test_access_controller`，断言 `streakWindowStartsAtFirstHitNotLast`
**真的变红**。实测通过。

> 没有这一步，"窗口语义已守住"就只是一句没有证据的话。
> **分工写进规则注释里**，以免后人误以为静态规则已经覆盖了语义。

**附带的真实缺陷修复（不在计划内）**
`bool expected = false;` 那类"闸门标志"的释放若手写 `store(false)`，
`cv::Exception` 异常路径会漏掉一条 —— 表现是"人脸检测从此不执行"且无日志。
P4 起质量门也走同一条识别路径，故沿用 P3 的 RAII 释放写法（见 §77）。

**实施记录（第 43 轮）**：上述内容全部落地并验证；提交见《交接与继续开发》。

---

### P4 原计划（保留以便对照）

> **状态（第 41 轮）：⬜ 未开始。**（ONNX 那一半已决定**不做**，见第 3 条。）
> **P3 已完成为本阶段留好了基础**：`V4L2Camera::frameGeneration()` 让
> "同一帧不重复检测"可判定；`m_faceDetectRunning` 的丢弃式闸门保证
> 检测请求不会堆积。质量门（清晰度/亮度）接进来时**不要**再引入队列 ——
> 沿用 §77 的"最新值"语义。

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
| README 三处失真 | ①配置查找顺序写成 4 级、实现只有 2 级；②人脸分类器路径在三处不一致（**第 44 轮已修**，见实施记录）；③`run_interop.sh` 实为 MSYS2 专用（README 却作为本机入口推荐） |
| 噪声注释 | 5 个遗留模块里 275 行机器生成的 `// 逐行注释：…`，其中部分已失真（如 `video_push_thread.h:12` 写的头文件路径已过时） |
| 跨平台缺口 | 交接文档第五节记录的 `tools/devstubs/sys/socket.h` 里 `#include <_mingw.h>`（属 P8），建议与本阶段一并修掉 |
| 缺陷台账 | 把 A 的 **40 条缺陷台账**逐条对照本项目，产出 `docs/借鉴核对表.md`：哪些本项目也存在、哪些已修、哪些不适用 |

> **实施记录（第 40 轮，部分完成；提交 `8308121`/`df22048`/`ae2b52f`）**：
> - ✅ Qt 5.9 API 违规：已修（提交 `修复 Qt 5.9 兼容性…`），并补了规则与夹具。
> - ✅ 跨平台缺口（devstubs）：已修（提交 `devstubs: 让主机端检查在 Linux 上可用`），
>   18/18 → 20/20 ctest 可跑。
> - ✅ `tools/check_host_compile.sh` 支持 Linux：本轮补了 Qt/OpenCV/mkspecs 的
>   自动定位、QtSerialPort 头目录与 `-fPIC`（Linux 分支；MinGW 分支逐条保持
>   原样），现在 `bash tools/check_host_compile.sh` 在 Linux 上**开箱即用**，
>   实测 20/20 通过。唯一仍需外部变量的是 `QTSP_INC`（QtSerialPort dev 包
>   解出来的位置）。
> - ✅ README 两处失真：配置查找顺序（4 级 → 实际的 2 级）与
>   `run_interop.sh` 的平台限制，都已改正并标注"第 40 轮更正"。
> - ⬜ 未做：275 行机器生成噪声注释的清理；A 的 40 条缺陷对照台账
>   （其中 A 的第 39/40 条是真实的指纹解析缺陷，值得单独核对本项目是否同病）。
>
> **实施记录（第 44 轮：人脸分类器路径不一致，提交见《交接与继续开发》）**
>
> 这一项原本标注为"需要先决定哪个是规范路径，属决策类工作"。实际做下来的
> 结论是：**不要试图定"唯一规范路径"** —— 现场可能已有机器按旧文档装到了
> `/root`，改到别处就是行为倒退。正确的做法是承认多个位置都可能对，
> 然后给出**有序候选 + 明示回退 + 可查的失败**。
>
> 真实缺陷（重构前就存在，一直静默）：分类器文件名/位置在**四处**各写一遍，
> 互不一致 —— `app_config.cpp` 默认值（`/root/...`）、根 `CMakeLists.txt`
> 的 `install()`、`deploy/install.sh`、旧文档。于是按文档全新部署的机器上
> **默认配置指向的文件根本不存在**，`CascadeClassifier::load()` 静默返回
> false，**人脸识别一直是关的**，而日志只有一行"分类器加载 失败" ——
> 现场会误判成摄像头坏了。
>
> 落地：
> - 新增 `src/config/resource_paths.{h,cpp}`：`cascadeFileName()` 单点真相、
>   `cascadeSearchPaths()`（纯函数，去重的有序候选）、
>   `resolveCascadePath()`（返回第一个真实存在的文件，全无则空串）；
> - `widget.cpp` 改用它，并在失败时打印**完整候选清单**；
> - **配置路径永远是第一候选**，因此"配置本来就正确"的现场行为逐字节不变
>   —— 这个修复只可能把"原本失败"变成"成功"（有用例钉住这条性质）。
>
> 两条坑（都属"看起来对了其实没有"）：
> - **`QFileInfo::absolutePath()` 是平台相关的**：Windows 上把 `/etc/x.json`
>   变成 `C:/etc/x.json`，于是**主机上算出的候选与目标机不同**（测试在主机过、
>   上板却不一样）。改用纯字符串去尾段，与平台无关；有用例断言候选里不得出现盘符。
> - **规则判据不能只搜 `configured` 这个词** —— 它本来就在函数签名里，
>   夹具 A3（整段"把配置加进候选"删掉）精确揭出了这个假绿。
>
> 校验：规则 `[resource-path]`（`verify.py` 53 → **54 项检查`）·
> 夹具 **10/10** 变异变红、3/3 对照不误报 · 新测试
> `t113_test_resource_paths` **15/15** · `resource_paths.cpp` 行覆盖 **100%**。
> 另外 5 个手工变异（配置优先/去重/isFile/历史路径/平台无关）
> 全部被测试抓到，还原后逐字节一致。

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

> **实施记录（第 40 轮，已完成；提交 `18092db`）** —— 四条纪律全部落地在 `deploy/build-t113.sh`
> 与 `cmake/toolchain-t113.cmake` 里，并在本机 SDK 上端到端验证：
> - SDK 预检：sysroot 存在且含 `lib/libc.so`、编译器 `-dumpmachine` 必须是
>   `arm*musl*`、sysroot 内能找到 Qt5 的 CMake 配置、OpenCV 与
>   libgpiod/ALSA 的头和库齐全 —— **任一不过就不开始编译**；
> - 产物核验：`file` 断言 `ELF 32-bit ... ARM`、解释器必须是 musl、
>   `readelf -d` 的每个 DT_NEEDED 都要能在 sysroot 中解析；
> - 纯 ASCII 路径提示：SDK 路径含非 ASCII 时给出警告与软链建议
>   （本机 SDK 路径含中文，CMake 这条路能过，但 qmake/moc 未必）。
>
> 实测：`source environment.sh && ./deploy/build-t113.sh` → 25 步编译链接全过，
> 产物 `ELF 32-bit ARM / ld-musl-armhf.so.1 / stripped`，516,644 字节，
> **依赖闭包 13/13 可解析**（`--as-needed` 正确丢弃了未使用的 Qt5Concurrent
> 与 opencv_imgcodecs/videoio；libgpiod 是静态库故不在 DT_NEEDED 中）。
> 顺带证伪一条旧担忧：本 SDK 是 Qt **5.12.9**，因此"5.10+ API 编不过"不成立；
> 但 `[qt5]` 规则仍按 5.9 下限守着（换 SDK 时不至于翻车）。
>
> 与计划的偏差：第 4 条"在纯 ASCII 的 /tmp 路径下构建"改成了**警告 + 软链建议**，
> 因为这次 SDK 的真实路径含中文（`Desktop/AccessterminalUI/交叉编译工具链`），
> 而 CMake 这条路带非 ASCII 路径也能编过（实测）；与其强改路径，不如在预检里
> 提示，真出 moc/uic 怪错时再软链到 `$HOME/t113-sdk`。
> 另外新增了 A 计划里没有的两条：**产物依赖闭包核验**（每个 DT_NEEDED 都要能在
> sysroot 中解析）与 **STAGING_DIR 自动设置**（消掉工具链包装脚本的警告）。

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

## 五、建议的前三个提交（**第 40 轮已全部完成**）

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
