# 更新日志 — Accessterminal_T113（门口机）

本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。
协议不兼容变更必须递增 `protocol.h` 中的 `kProtocolVersion`。

## [1.0.0] — 重构版

### 架构

- 将单文件 1381 行的 `widget.cpp` 按职责拆分为分层结构：
  `core/{net,media,vision}`、`hal`、`managers`、`ui`、`config`。
- 引入 `core/net/protocol.h` 作为协议唯一事实来源，端口、音视频参数与
  信令字符串集中定义；该文件与室内机仓库逐字节一致。
- 引入 `AppConfig` 三级配置（内置默认值 → JSON → 环境变量），
  消除散落的硬编码 IP、设备节点与路径。
- 引入 `GpioLine` 统一 libgpiod 资源生命周期。
- 引入 `SignalingClient` 统一 TCP 信令（3 秒自动重连、按 `\n` 分包、
  未知信令忽略而非报错）。
- 新增 CMake 构建与 T113 交叉编译工具链文件；移除 qmake 脚本中
  `/home/meetyoo/...` 等开发机绝对路径。

### 修复

- **音频接收线程阻塞**：`recvfrom` 原为阻塞调用，线程无法响应 `stop()`，
  挂断时 `close(fd)` 与阻塞读竞争。改为非阻塞轮询并处理 `EINTR`。
- **音量设置崩溃风险**：`setVolume()` 未校验 ALSA 返回码且未用
  `alloca` 分配 selem id，可能空句柄解引用。
- **GPIO 重复请求**：开锁继电器与补光灯在硬件上共用 PE5（线号 133），
  原实现创建两个 `GpioLine` 各请求同一条线，第二个必然失败。
  改为共用单一句柄，与重构前 `line5` 语义一致。
- **通话卡死**：补回音频线程 `connectionLost` → `hangUpCall` 的连接。
- **密码重复硬编码**：`PasswordManager` 与 `form_passwd` 各写一份 `"5788"`，
  统一收敛到 `AppConfig.security.door_password`。
- **缺失资源引用**：移除 `res.qrc` 中 3 个不存在的图片
  （`mybg002.png`、`smartpg.png`、`deviceback.png`）。

### 兼容性

- 端口、信令字符串、TCP 分包方式、音频参数、视频帧格式**均未改动**。
- 音视频线程仍在收到 `accept` 后才创建，空闲时零资源占用。
- 界面布局与 `.ui` 文件未做任何修改。

### 已知限制

- 交叉编译需在 Linux 主机配合 Tina SDK 完成，开发机无法验证。
