#include <QCoreApplication>
#include <QSaveFile>
#include <QStringList>

int main(int argc, char **argv) {
    QCoreApplication app{argc, argv};
    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 2) return 2;
    QSaveFile out{args.at(1)};
    if (!out.open(QIODevice::WriteOnly)) return 3;
    QByteArray bytes;
    for (qsizetype at = 2; at < args.size(); ++at)
        bytes += args.at(at).toUtf8() + '\n';
    bytes += "env:ELECTRON_RUN_AS_NODE=" + qgetenv("ELECTRON_RUN_AS_NODE") + '\n';
    out.write(bytes);
    return out.commit() ? 0 : 4;
}
