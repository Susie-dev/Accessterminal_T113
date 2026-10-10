/*
 * fake_gpiod.cpp — 可编程假 libgpiod 的实现。
 *
 * 内存所有权严格对齐 libgpiod 1.x（这是第一版的错误所在，务必先读）
 * ------------------------------------------------------------------
 *   - `gpiod_chip_get_line(chip, off)` 返回 **chip 内部的**线对象
 *     （真实实现是 `&chip->lines[off]`），**不新分配**内存。
 *   - `gpiod_line_release(line)` 只清除该线的"已请求"状态，
 *     **不释放**内存。
 *   - `gpiod_chip_close(chip)` 释放 chip 及其全部线对象。
 *
 * 因此本实现里线对象随 chip 一起分配、随 chip 一起释放。第一版让 get_line
 * 各自 new、release 各自 delete，与真实 ABI 不符，于是生产代码在"请求失败"
 * 路径里先 release 再 close 时被误报成 `release:unknown` —— 是夹具错了。
 *
 * 由此得到的现场度量是 busyLineCount()（仍持有有效请求的线数）：泄漏一条请求
 * 会让线一直 busy，下次开锁直接 EBUSY。
 *
 * 另外：对**已 busy** 的线再次请求会返回失败（真实模块 EBUSY 行为），
 * 使得"泄漏请求"能被端到端抓到，而不是靠事后数数。
 */
#include "fake_gpiod.h"

#include <QHash>

#include <gpiod.h>

// ---------------------------------------------------------------------------
// 结构性定义放在最前：线对象**属于** chip（对齐 libgpiod 1.x 的内存模型）。
// 这些必须是文件作用域，因为 libgpiod 的 ABI 函数在 namespace 之外定义。
// ---------------------------------------------------------------------------
namespace {
/// 每个 chip 支持的线号上限。
///
/// 必须**大于**本工程实际使用的线号（PE4=132、PE5=133），否则假实现会把
/// 合法线号判成越界。第一版设成 64，于是所有 132/133 的用例全部失败 ——
/// 那是夹具自造的假象，不是生产代码的问题。
/// 取 1024 留足余量：T113 每个 gpiochip 实际线数在几百量级。
const int kMaxLines = 1024;
} // namespace

struct gpiod_line {
    int requested;   ///< 0 = 空闲，1 = 已请求
};

struct gpiod_chip {
    struct gpiod_line lines[kMaxLines];
    int lineCount;
};

namespace fakegpiod {
namespace {

bool g_chipOpenFail = false;
bool g_getLineFail = false;
bool g_reqOutFail = false;
bool g_reqInFail = false;
bool g_setValueFail = false;
int g_readValue = 0;

int g_chipOpen = 0;
int g_chipClose = 0;
int g_getLine = 0;
int g_reqOut = 0;
int g_reqIn = 0;
int g_release = 0;
int g_releaseUnrequested = 0;
int g_setValue = 0;
int g_getValue = 0;

QStringList g_log;
QString g_lastConsumer;
int g_lastInitial = 0;
int g_lastSet = 0;
QString g_lastChip;
unsigned int g_lastOffset = 0;

QHash<const void*, bool> g_liveChips;
/// 仍持有有效请求的线 -> true。
QHash<const void*, bool> g_busy;
/// (chip 名, 线号) -> 请求成功次数。
QHash<QString, int> g_reqCount;

QString keyFor(const QString& chip, unsigned int off)
{
    return chip + QLatin1Char('#') + QString::number(off);
}

} // namespace

void reset()
{
    g_chipOpenFail = g_getLineFail = g_reqOutFail = g_reqInFail = false;
    g_setValueFail = false;
    g_readValue = 0;

    g_chipOpen = g_chipClose = g_getLine = 0;
    g_reqOut = g_reqIn = g_release = g_releaseUnrequested = 0;
    g_setValue = g_getValue = 0;

    g_log.clear();
    g_lastConsumer.clear();
    g_lastInitial = 0;
    g_lastSet = 0;
    g_lastChip.clear();
    g_lastOffset = 0;

    g_liveChips.clear();
    g_busy.clear();
    g_reqCount.clear();
}

void setChipOpenFail(bool fail) { g_chipOpenFail = fail; }
void setGetLineFail(bool fail) { g_getLineFail = fail; }
void setRequestOutputFail(bool fail) { g_reqOutFail = fail; }
void setRequestInputFail(bool fail) { g_reqInFail = fail; }
void setSetValueFail(bool fail) { g_setValueFail = fail; }
void setReadValue(int v) { g_readValue = v; }

int chipOpenCount() { return g_chipOpen; }
int chipCloseCount() { return g_chipClose; }
int getLineCount() { return g_getLine; }
int requestOutputCount() { return g_reqOut; }
int requestInputCount() { return g_reqIn; }
int releaseCount() { return g_release; }
int setValueCount() { return g_setValue; }
int getValueCount() { return g_getValue; }

int liveChipCount() { return g_liveChips.size(); }
int busyLineCount() { return g_busy.size(); }
int releaseUnrequestedCount() { return g_releaseUnrequested; }

QStringList callLog() { return g_log; }
QString lastConsumer() { return g_lastConsumer; }
int lastInitialValue() { return g_lastInitial; }
int lastSetValue() { return g_lastSet; }
QString lastChipName() { return g_lastChip; }
unsigned int lastLineOffset() { return g_lastOffset; }

int requestCountFor(const QString& chipName, unsigned int offset)
{
    return g_reqCount.value(keyFor(chipName, offset), 0);
}

} // namespace fakegpiod

/* ========================================================================
 * libgpiod ABI 实现
 *
 * 必须是**全局**符号（gpio.cpp 直接调用 C 接口）。签名与
 * tools/devstubs/gpiod.h（按 libgpiod 1.x 复刻）一致，由该头文件编译期校验。
 * ===================================================================== */

using namespace fakegpiod;

namespace {
/// chip 名 -> 指向该 chip 的指针，用于 requestCountFor 之类的按名统计。
QHash<const struct gpiod_chip*, QString> g_chipNames;

/// 找出这条线对象属于哪个已打开的 chip（真实实现里线属于 chip）。
const struct gpiod_chip* ownerOf(const struct gpiod_line* line)
{
    if (!line) return nullptr;
    for (QHash<const void*, bool>::const_iterator it = g_liveChips.constBegin();
         it != g_liveChips.constEnd(); ++it) {
        const struct gpiod_chip* c =
            static_cast<const struct gpiod_chip*>(it.key());
        if (line >= &c->lines[0] && line <= &c->lines[c->lineCount - 1]) {
            return c;
        }
    }
    return nullptr;
}

/// 线号（相对 chip 的偏移）。
int offsetOf(const struct gpiod_chip* c, const struct gpiod_line* line)
{
    if (!c || !line) return -1;
    return static_cast<int>(line - &c->lines[0]);
}
} // namespace

struct gpiod_chip* gpiod_chip_open_by_name(const char* name)
{
    ++g_chipOpen;
    g_lastChip = QString::fromUtf8(name ? name : "");
    g_log << QStringLiteral("open:%1").arg(g_lastChip);

    if (g_chipOpenFail) {
        g_log << QStringLiteral("open:FAIL");
        return nullptr;
    }
    // 真实 libgpiod 按 chip 实际线数分配；这里固定 kMaxLines，够用。
    struct gpiod_chip* c = new gpiod_chip();
    c->lineCount = kMaxLines;
    for (int i = 0; i < kMaxLines; ++i) {
        c->lines[i].requested = 0;
    }
    g_liveChips.insert(c, true);
    g_chipNames.insert(c, g_lastChip);
    return c;
}

struct gpiod_chip* gpiod_chip_open(const char* path)
{
    return gpiod_chip_open_by_name(path);
}

void gpiod_chip_close(struct gpiod_chip* chip)
{
    if (!chip) {
        // 传 nullptr 不记账：真实 libgpiod 的 close 对 nullptr 也会崩，
        // 生产代码的守卫避免了这一点；这里不算一次"有效关闭"。
        return;
    }
    ++g_chipClose;
    g_log << QStringLiteral("close");

    // chip 关闭会连带清掉它上面所有线的请求状态（内存随 chip 一起释放）。
    for (int i = 0; i < chip->lineCount; ++i) {
        if (chip->lines[i].requested) {
            g_busy.remove(&chip->lines[i]);
            chip->lines[i].requested = 0;
        }
    }
    g_liveChips.remove(chip);
    g_chipNames.remove(chip);
    delete chip;
}

struct gpiod_line* gpiod_chip_get_line(struct gpiod_chip* chip,
                                       unsigned int offset)
{
    ++g_getLine;
    g_lastOffset = offset;
    g_log << QStringLiteral("getline:%1").arg(offset);

    if (g_getLineFail) {
        g_log << QStringLiteral("getline:FAIL");
        return nullptr;
    }
    // 线号越界按拿不到线处理（真实 libgpiod 亦然）。
    if (!chip || static_cast<int>(offset) >= chip->lineCount) {
        g_log << QStringLiteral("getline:OUTOFRANGE");
        return nullptr;
    }
    // 关键：返回 chip **内部**的线对象，不新分配。
    return &chip->lines[offset];
}

namespace {
/// 请求的公共逻辑：已 busy 则失败（真实 EBUSY 行为）。
int doRequest(struct gpiod_line* line, bool isOutput)
{
    // 找出所属 chip，以便记账与按名统计。
    const struct gpiod_chip* c = ownerOf(line);
    if (!c) {
        g_log << QStringLiteral("req:NOOWNER");
        return -1;
    }
    const int off = offsetOf(c, line);
    const QString chipName = g_chipNames.value(c);

    if (line->requested) {
        g_log << QStringLiteral("req:BUSY");
        return -1;
    }
    line->requested = 1;
    g_busy.insert(line, true);
    g_reqCount[keyFor(chipName, static_cast<unsigned int>(off))] += 1;
    (void)isOutput;
    return 0;
}
} // namespace

int gpiod_line_request_input(struct gpiod_line* line, const char* consumer)
{
    ++g_reqIn;
    g_lastConsumer = QString::fromUtf8(consumer ? consumer : "");
    g_log << QStringLiteral("reqin:%1").arg(g_lastConsumer);

    if (g_reqInFail) {
        g_log << QStringLiteral("reqin:FAIL");
        return -1;
    }
    return doRequest(line, false);
}

int gpiod_line_request_output(struct gpiod_line* line, const char* consumer,
                              int default_val)
{
    ++g_reqOut;
    g_lastConsumer = QString::fromUtf8(consumer ? consumer : "");
    g_lastInitial = default_val;
    g_log << QStringLiteral("reqout:%1:%2").arg(g_lastConsumer)
                 .arg(default_val);

    if (g_reqOutFail) {
        g_log << QStringLiteral("reqout:FAIL");
        return -1;
    }
    return doRequest(line, true);
}

int gpiod_line_get_value(struct gpiod_line* line)
{
    (void)line;
    ++g_getValue;
    g_log << QStringLiteral("get");
    return g_readValue;
}

int gpiod_line_set_value(struct gpiod_line* line, int value)
{
    (void)line;
    ++g_setValue;
    g_lastSet = value;
    g_log << QStringLiteral("set:%1").arg(value);
    return g_setValueFail ? -1 : 0;
}

void gpiod_line_release(struct gpiod_line* line)
{
    if (!line) {
        return;
    }
    if (!line->requested) {
        // 对未被请求的线 release：记录但不算错误。
        //
        // 生产代码在"请求失败"路径里正好会走到这里（先 release 再 close），
        // 而 libgpiod 对这种情况的确切行为我无法在本机验证（没有真实
        // gpiod.h / 库）。因此只观测、不断言，避免把猜测写成契约。
        ++g_releaseUnrequested;
        g_log << QStringLiteral("release:unrequested");
        return;
    }
    ++g_release;
    g_log << QStringLiteral("release");
    line->requested = 0;
    g_busy.remove(line);
    // 不释放内存：线对象属于 chip。
}
