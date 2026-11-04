#include "studiooscsettings.hpp"

#include <QSettings>
#include <QVariant>

#include <string_view>

namespace PelicanStudio {
namespace {

bool isIpv4(const QString &address)
{
    const QStringList parts = address.split(QLatin1Char('.'), Qt::KeepEmptyParts);
    if (parts.size() != 4) return false;
    for (const QString &part : parts) {
        if (part.isEmpty() || part.size() > 3) return false;
        for (const QChar character : part) {
            if (!character.isDigit()) return false;
        }
        bool ok = false;
        if (part.toUInt(&ok) > 255 || !ok) return false;
    }
    return true;
}

bool isOscOption(const QString &argument)
{
    for (const QString &flag :
         {QStringLiteral("--osc-port"), QStringLiteral("--osc-interface"),
          QStringLiteral("--osc-allow")}) {
        if (argument == flag || argument.startsWith(flag + QLatin1Char('='))) return true;
    }
    return false;
}

bool isOscOption(std::string_view argument)
{
    for (const std::string_view flag : {"--osc-port", "--osc-interface", "--osc-allow"}) {
        if (argument == flag) return true;
        if (argument.size() > flag.size() && argument.substr(0, flag.size()) == flag &&
            argument[flag.size()] == '=') {
            return true;
        }
    }
    return false;
}

} // namespace

QStringList studioOscPlayerArguments(const StudioOscSettings &settings)
{
    QStringList arguments;
    arguments << QStringLiteral("--osc-port") << QString::number(settings.port);
    arguments << QStringLiteral("--osc-interface") << settings.interface_address.trimmed();
    const QString allow = settings.allow.trimmed();
    if (!allow.isEmpty()) {
        arguments << QStringLiteral("--osc-allow") << allow;
    }
    return arguments;
}

QString validateStudioOscSettings(const StudioOscSettings &settings)
{
    if (settings.port < 0 || settings.port > 65535) {
        return QStringLiteral("The port must be between 0 and 65535.");
    }
    if (!isIpv4(settings.interface_address.trimmed())) {
        return QStringLiteral(
            "The interface must be an IPv4 address such as 127.0.0.1.");
    }
    const QString allow = settings.allow.trimmed();
    if (!allow.isEmpty()) {
        for (const QString &address : allow.split(QLatin1Char(','), Qt::KeepEmptyParts)) {
            if (!isIpv4(address)) {
                return QStringLiteral(
                    "The allow list must be comma-separated IPv4 addresses, with no "
                    "spaces and no trailing comma.");
            }
        }
    }
    return {};
}

bool studioOscInterfaceIsLoopback(const QString &address)
{
    const QString trimmed = address.trimmed();
    if (!isIpv4(trimmed)) return false;
    return trimmed.section(QLatin1Char('.'), 0, 0) == QStringLiteral("127");
}

bool studioOscOptionsFromEnvironment(const QStringList &arguments)
{
    for (const QString &argument : arguments) {
        if (isOscOption(argument)) return true;
    }
    return false;
}

bool studioOscOptionsFromEnvironment(const std::vector<std::string> &arguments)
{
    for (const std::string &argument : arguments) {
        if (isOscOption(std::string_view{argument})) return true;
    }
    return false;
}

bool studioBuildHasOsc()
{
#if defined(PELICAN_WITH_OSC) && PELICAN_WITH_OSC
    return true;
#else
    return false;
#endif
}

StudioOscSettingsStore::StudioOscSettingsStore(QSettings &settings) : settings_(settings) {}

StudioOscSettings StudioOscSettingsStore::load() const
{
    StudioOscSettings result;
    result.enabled = settings_.value(QStringLiteral("osc/enabled"), result.enabled).toBool();

    bool ok = false;
    const int port = settings_.value(QStringLiteral("osc/port"), result.port).toInt(&ok);
    if (ok) result.port = port;

    const QVariant interface_address = settings_.value(QStringLiteral("osc/interface"));
    if (interface_address.isValid() && !interface_address.isNull()) {
        result.interface_address = interface_address.toString();
    }
    const QVariant allow = settings_.value(QStringLiteral("osc/allow"));
    if (allow.isValid() && !allow.isNull()) result.allow = allow.toString();
    return result;
}

bool StudioOscSettingsStore::save(const StudioOscSettings &settings, QString *error)
{
    settings_.setValue(QStringLiteral("osc/enabled"), settings.enabled);
    settings_.setValue(QStringLiteral("osc/port"), settings.port);
    settings_.setValue(QStringLiteral("osc/interface"), settings.interface_address);
    settings_.setValue(QStringLiteral("osc/allow"), settings.allow);
    settings_.sync();
    if (settings_.status() != QSettings::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("The OSC settings could not be saved to %1.")
                         .arg(settings_.fileName());
        }
        return false;
    }
    return true;
}

} // namespace PelicanStudio
