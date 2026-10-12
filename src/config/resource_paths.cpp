/**
 * @file resource_paths.cpp
 * @brief 运行时资源路径解析的实现。
 */
#include "config/resource_paths.h"

#include <QFileInfo>
#include <QStringList>

namespace accessterminal {

QString cascadeFileName()
{
    return QStringLiteral("haarcascade_frontalface_default.xml");
}

QStringList cascadeSearchPaths(const QString& configured,
                               const QString& configPath)
{
    QStringList out;

    // 候选 1（永远第一）：配置里显式写明的路径。
    // 现场部署是"配置说什么就是什么"，绝不因为别处存在同名文件而改用它。
    const QString cfg = configured.trimmed();
    if (!cfg.isEmpty()) {
        out << cfg;
    }

    // 候选 2：与配置文件**同目录**。install.sh 把两者分别装到
    // /etc/accessterminal/ 与 $PREFIX/share/，但手工"拷到同一个目录再改
    // 配置"是最常见的现场做法，值得覆盖。
    //
    // 注意这里**不用 QFileInfo::absolutePath()**：它会做"绝对路径解析"，
    // 在 Windows 上把 "/etc/accessterminal/appconfig.json" 变成
    // "C:/etc/accessterminal/appconfig.json"（把开头的 / 当成当前盘的根）。
    // 目标机是 Linux 不受影响，但本模块要能在主机上测 —— 而"主机上算出来的
    // 候选与目标机不同"正是最难发现的假绿（测试过了，上板却不一样）。
    // 这里只需要"去掉最后一段"，纯字符串处理就够，且与平台无关。
    const QString name = cascadeFileName();
    const QString cp = configPath.trimmed();
    if (!cp.isEmpty()) {
        const int slash = cp.lastIndexOf(QLatin1Char('/'));
        if (slash > 0) {                       // >0：排除 "/appconfig.json" 这种
            out << cp.left(slash + 1) + name;  // 根目录下的情形（无目录可拼）
        }
    }

    // 候选 3~5：按 install.sh / CMake install() 的两种落点补齐。
    //   deploy/install.sh     -> $PREFIX/share/            （PREFIX 默认 /opt/smartpro）
    //   根 CMakeLists.txt     -> ${CMAKE_INSTALL_DATADIR}/accessterminal
    // 两者都列出来，因为"本机手工 make install"与"用部署脚本"是两条真实路径。
    const QString prefix = QStringLiteral("/opt/smartpro");
    out << prefix + QStringLiteral("/share/") + name;
    out << QStringLiteral("/usr/local/share/accessterminal/") + name;
    out << QStringLiteral("/usr/share/accessterminal/") + name;

    // 候选（最后）：重构前的历史默认值。**保留**是有意的 —— 现场可能真的有
    // 一台机器是按旧文档手工拷到 /root 的；把它排在最后既不影响新部署，
    // 又让那台机器能继续工作。删掉它才是真正的行为倒退。
    out << QStringLiteral("/root/") + name;

    // 去重（保持首次出现顺序）。
    // 为什么必须去重：配置里很可能就写着 /root/xxx.xml，于是候选 1 与
    // 最后一项完全相同。重复项会让"找不到时的告警清单"看起来像有 6 个
    // 位置可选，误导排查；也会让调用方按数量做判断时出错。
    QStringList uniq;
    for (const QString& p : out) {
        if (!uniq.contains(p)) {
            uniq << p;
        }
    }
    return uniq;
}

QString resolveCascadePath(const QString& configured,
                           const QString& configPath)
{
    const QStringList cands = cascadeSearchPaths(configured, configPath);
    for (const QString& p : cands) {
        // isFile() 而不是 exists()：目录同名（例如某个包把分类器做成了
        // 目录 /usr/share/accessterminal/haarcascade_frontalface_default.xml/）
        // 时 exists() 会为真，交给 CascadeClassifier::load() 只会失败并
        // 让人以为"路径已经找对了"。这里直接排除。
        if (QFileInfo(p).isFile()) {
            return p;
        }
    }
    return QString();
}

} // namespace accessterminal
