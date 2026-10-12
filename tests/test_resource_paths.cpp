/**
 * @file test_resource_paths.cpp
 * @brief `resource_paths` 的单元测试（第 44 轮，方案 P5）。
 *
 * 背景：人脸分类器路径在三处不一致，默认部署下人脸识别**静默失效**。
 * 本测试守两件事：
 *   1. **候选顺序**（纯函数部分）—— 谁会先被选中；
 *   2. **回退行为**（磁盘部分）—— 前一个不存在时是否会继续找。
 *
 * 特别守住的**兼容性**性质：配置里写明的路径永远是第一候选。
 * 因此"配置正确"的现场行为与重构前逐字节一致（不做任何回退）。
 */
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "config/resource_paths.h"

using accessterminal::cascadeFileName;
using accessterminal::cascadeSearchPaths;
using accessterminal::resolveCascadePath;

class TestResourcePaths : public QObject
{
    Q_OBJECT

private slots:
    void cascadeFileNameMatchesDeployScripts();
    void configuredPathIsAlwaysFirst();
    void emptyConfigStillYieldsCandidates();
    void candidatesCoverAllThreeDeploymentLayouts();
    void candidatesAreDeduplicated();
    void legacyRootPathIsKeptButLast();
    void configDirCandidateIsDerivedFromConfigPath();
    void rootLevelConfigHasNoJoinCandidate();
    void resolvePrefersConfiguredWhenItExists();
    void resolveFallsBackWhenConfiguredIsMissing();
    void resolveReturnsEmptyWhenNothingExists();
    void resolveIgnoresDirectories();
    void fallbackNeverChangesASuccessfulLoad();
};

/// 文件名必须与 CMake / install.sh / 配置默认值里的完全一致。
/// 改这里而没改那三处，就会重现"路径不一致"的老毛病。
void TestResourcePaths::cascadeFileNameMatchesDeployScripts()
{
    QCOMPARE(cascadeFileName(),
             QStringLiteral("haarcascade_frontalface_default.xml"));
}

/// 配置里写明的路径必须**永远**是第一候选 —— 这条兼容性性质是核心。
void TestResourcePaths::configuredPathIsAlwaysFirst()
{
    const QString cfg = QStringLiteral("/opt/cascade.xml");
    const QStringList cands = cascadeSearchPaths(cfg, QString());
    QVERIFY(!cands.isEmpty());
    QCOMPARE(cands.first(), cfg);

    // 即使配置路径与某个回退位置"看起来相同"，也不得被换掉。
    const QStringList c2 = cascadeSearchPaths(
        QStringLiteral("/root/haarcascade_frontalface_default.xml"), QString());
    QCOMPARE(c2.first(),
             QStringLiteral("/root/haarcascade_frontalface_default.xml"));
}

/// 没配路径时也要给出候选（不能是空清单 —— 那等于放弃寻找）。
void TestResourcePaths::emptyConfigStillYieldsCandidates()
{
    const QStringList cands = cascadeSearchPaths(QString(), QString());
    QVERIFY2(cands.size() >= 3,
             "没配置路径时也必须给出部署脚本/install() 的落点作为候选");

    // 空/纯空白配置项不得变成候选（否则会去 isFile("") 并打印空路径）。
    for (const QString& c : cands) {
        QVERIFY(!c.trimmed().isEmpty());
        QVERIFY(c.endsWith(cascadeFileName()));
    }

    const QStringList blank = cascadeSearchPaths(QStringLiteral("   "),
                                                 QString());
    QCOMPARE(blank, cands);
}

/// 候选必须覆盖三处真实落点 —— 这正是"路径不一致"缺陷的三个当事者。
void TestResourcePaths::candidatesCoverAllThreeDeploymentLayouts()
{
    const QStringList cands = cascadeSearchPaths(QString(), QString());
    const QString n = cascadeFileName();

    // deploy/install.sh: $PREFIX/share/ （PREFIX 默认 /opt/smartpro）
    QVERIFY2(cands.contains(QStringLiteral("/opt/smartpro/share/") + n),
             "缺少 deploy/install.sh 的落点");

    // 根 CMakeLists.txt: ${CMAKE_INSTALL_DATADIR}/accessterminal
    // （GNUInstallDirs 在无前缀时给 /usr/local/share）
    QVERIFY2(cands.contains(
                 QStringLiteral("/usr/local/share/accessterminal/") + n),
             "缺少 CMake install() 的落点");
}

/// 去重是必须的：配置里常常就写着 /root/xxx.xml，它与最后一个回退候选
/// 完全相同。重复项会让"找不到"时的告警清单长度失真、误导排查。
void TestResourcePaths::candidatesAreDeduplicated()
{
    const QString legacy =
        QStringLiteral("/root/") + cascadeFileName();
    const QStringList cands = cascadeSearchPaths(legacy, QString());

    int hits = 0;
    for (const QString& c : cands) {
        if (c == legacy) ++hits;
    }
    QCOMPARE(hits, 1);   // 只出现一次（作为第一候选）

    // 整体上不得有任何重复项
    QCOMPARE(cands.size(), QSet<QString>(cands.begin(), cands.end()).size());
}

/// 旧文档写的 /root/... 必须**保留**（可能真有机器是那样装的），
/// 但排在最后 —— 这样新部署不受影响，老机器继续能用。
void TestResourcePaths::legacyRootPathIsKeptButLast()
{
    const QStringList cands = cascadeSearchPaths(QString(), QString());
    const QString legacy = QStringLiteral("/root/") + cascadeFileName();

    QVERIFY2(cands.contains(legacy),
             "不得删除历史路径：那会让按旧文档部署的现场直接失效");
    QCOMPARE(cands.last(), legacy);
}

/// 与配置文件同目录的候选：手工部署常把两者放一起。
///
/// 这条同时守住一个**平台无关**性质。第一版实现用了
/// `QFileInfo::absolutePath()`，在 Windows 上会把
/// "/etc/accessterminal/appconfig.json" 解析成
/// "C:/etc/accessterminal/appconfig.json"（把开头的 / 当成当前盘根），
/// 于是**主机上算出来的候选清单与目标机不同** —— 测试在主机上过、
/// 上板却不一样，是最难发现的一类假绿。改用纯字符串处理后与平台无关。
void TestResourcePaths::configDirCandidateIsDerivedFromConfigPath()
{
    const QStringList cands = cascadeSearchPaths(
        QString(), QStringLiteral("/etc/accessterminal/appconfig.json"));
    QVERIFY(cands.contains(
        QStringLiteral("/etc/accessterminal/") + cascadeFileName()));

    // 主机上不得被改写成带盘符的形式（否则这条断言就白写了）。
    for (const QString& c : cands) {
        QVERIFY2(!c.contains(QLatin1Char(':')),
                 qPrintable(QStringLiteral("候选里出现了盘符，说明做了平台相关"
                                           "的路径解析: ") + c));
    }

    // 相对路径的目录部分同样要保留。
    const QStringList rel = cascadeSearchPaths(
        QString(), QStringLiteral("conf/appconfig.json"));
    QVERIFY(rel.contains(QStringLiteral("conf/") + cascadeFileName()));
}

/// 根目录下的配置文件：没有"目录部分"可拼，不得产出 "//xxx" 这种路径。
void TestResourcePaths::rootLevelConfigHasNoJoinCandidate()
{
    const QStringList cands = cascadeSearchPaths(
        QString(), QStringLiteral("/appconfig.json"));
    const QString bad = QStringLiteral("//") + cascadeFileName();
    QVERIFY2(!cands.contains(bad), "不得把根目录拼成 '//' 形式");
    for (const QString& c : cands) {
        QVERIFY(!c.startsWith(QStringLiteral("//")));
    }
}

/// 配置路径存在时必须直接用它，且与"不解析"的行为一致。
void TestResourcePaths::resolvePrefersConfiguredWhenItExists()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString good = dir.filePath(QStringLiteral("custom_cascade.xml"));
    QFile f(good);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("<opencv_storage/>");
    f.close();

    // 注意：即使后面还有 5 个候选，也必须返回配置里的那个。
    QCOMPARE(resolveCascadePath(good, QString()), good);

    // 一条更强的兼容性断言：把"配置的路径"和"configPath"都给全，
    // 结果仍然只由配置决定。
    QCOMPARE(resolveCascadePath(good,
                                dir.filePath(QStringLiteral("appconfig.json"))),
             good);
}

/// 这是本模块存在的理由：配置路径不存在时，必须回退到**真实存在**的落点。
/// 重构前这里直接失败 -> 人脸识别静默关闭。
void TestResourcePaths::resolveFallsBackWhenConfiguredIsMissing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 造一个"分类器与配置文件同目录"的手工部署。
    const QString cfgPath = dir.filePath(QStringLiteral("appconfig.json"));
    QFile c(cfgPath);
    QVERIFY(c.open(QIODevice::WriteOnly));
    c.close();

    const QString real = dir.filePath(cascadeFileName());
    QFile f(real);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("<opencv_storage/>");
    f.close();

    // 配置指向一个不存在的位置（模拟默认值 /root/... 在非 root 部署下的情形）
    const QString missing =
        dir.filePath(QStringLiteral("nowhere/cascade.xml"));
    QVERIFY(!QFile::exists(missing));

    const QString got = resolveCascadePath(missing, cfgPath);
    QCOMPARE(got, real);
    QVERIFY2(!got.isEmpty(), "配置路径不存在时必须继续寻找，而不是直接失败");
}

/// 全都不存在时必须返回**空串**（调用方据此报错并打印清单），
/// 而不是返回某个"看起来对"的路径 —— 那会让人以为已经找对了。
void TestResourcePaths::resolveReturnsEmptyWhenNothingExists()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString missing = dir.filePath(QStringLiteral("no/such/cascade.xml"));

    // 用一个不可能存在的 configPath，避免误命中本机真实的 /usr/share 等位置。
    const QString got = resolveCascadePath(
        missing, dir.filePath(QStringLiteral("no/such/appconfig.json")));

    // 若本机真的存在 /usr/share/accessterminal/... 则这条会失败 —— 那是
    // 环境问题而非实现问题，故只在"确实都没有"时断言。
    const QStringList cands = cascadeSearchPaths(
        missing, dir.filePath(QStringLiteral("no/such/appconfig.json")));
    bool anyExists = false;
    for (const QString& p : cands) {
        if (QFileInfo(p).isFile()) { anyExists = true; break; }
    }
    if (!anyExists) {
        QVERIFY2(got.isEmpty(),
                 "一个候选都不存在时必须返回空串，让调用方明确报错");
    }
}

/// 同名**目录**不能被当成命中：exists() 会为真，但 cascade.load() 必然失败，
/// 结果是"以为路径找对了却仍然不工作" —— 比返回空串更难查。
void TestResourcePaths::resolveIgnoresDirectories()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString dirAsFile = dir.filePath(QStringLiteral("cascade_dir"));
    QVERIFY(QDir().mkpath(dirAsFile + QLatin1Char('/')));

    const QStringList cands = cascadeSearchPaths(dirAsFile, QString());
    QCOMPARE(cands.first(), dirAsFile);

    // 目录存在，但它不是文件 -> 必须继续找；若别处也没有则返回空。
    const QString got = resolveCascadePath(
        dirAsFile, dir.filePath(QStringLiteral("no/appconfig.json")));
    QVERIFY2(got != dirAsFile,
             "同名目录不得被当作分类器文件（isFile 而非 exists）");
}

/// 兼容性的核心断言：**配置正确时，解析不得改变结果**。
/// 也就是说本模块只可能把"原本失败"变成"成功"，不会把成功变失败。
void TestResourcePaths::fallbackNeverChangesASuccessfulLoad()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString good = dir.filePath(cascadeFileName());
    QFile f(good);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("<opencv_storage/>");
    f.close();

    for (int i = 0; i < 3; ++i) {
        // 无论有没有 configPath、无论磁盘上还有多少其它候选，
        // 只要配置指向的文件存在，答案就永远是它。
        const QString got = (i == 0)
            ? resolveCascadePath(good, QString())
            : resolveCascadePath(good,
                                 dir.filePath(QStringLiteral("appconfig.json")));
        QCOMPARE(got, good);
    }
}

QTEST_MAIN(TestResourcePaths)
#include "test_resource_paths.moc"
