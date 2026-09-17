#include "packagecommand.hpp"

#include "../project/packagemanifest.hpp"

#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Pelican::DevCli {
namespace {

using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view usage =
    "usage: pelican_cli package install <dir> --project <project>\n"
    "       pelican_cli package list --project <project>\n"
    "       pelican_cli package inspect <dir>";

std::filesystem::path absolutePath(const std::filesystem::path &path) {
    std::error_code error;
    auto absolute = path.is_absolute()
                        ? path
                        : std::filesystem::current_path(error) / path;
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute : canonical;
}

// `--project <dir>`, consumed from anywhere in the remaining arguments so the
// order of the positional and the option does not matter.
bool takeProjectOption(std::vector<std::string> &arguments,
                       std::filesystem::path &project) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] != "--project") continue;
        if (index + 1 >= arguments.size()) {
            std::cerr << "--project requires a directory" << std::endl;
            return false;
        }
        project = arguments[index + 1];
        arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index),
                        arguments.begin() +
                            static_cast<std::ptrdiff_t>(index) + 2);
        return true;
    }
    std::cerr << "--project is required" << std::endl;
    return false;
}

OrderedJson manifestJson(const PackageManifest &manifest) {
    OrderedJson requirements{
        {"engine_min_version", manifest.requirements.engine_min_version}};
    if (manifest.requirements.sdk_abi)
        requirements["sdk_abi"] = *manifest.requirements.sdk_abi;
    requirements["capabilities"] = manifest.requirements.capabilities;
    auto dependencies = OrderedJson::array();
    for (const auto &dependency : manifest.dependencies) {
        dependencies.push_back(
            OrderedJson{{"namespace", dependency.name_space},
                        {"package_version", dependency.package_version}});
    }
    return OrderedJson{
        {"schema", std::string{packageManifestSchema}},
        {"version", packageManifestVersion},
        {"namespace", manifest.name_space},
        {"package_version", manifest.package_version},
        {"description", manifest.description},
        {"requires", std::move(requirements)},
        {"dependencies", std::move(dependencies)},
        {"provides",
         {{"features", manifest.provides.features},
          {"graphs", manifest.provides.graphs},
          {"components", manifest.provides.components},
          {"code", manifest.provides.code}}},
    };
}

int reportError(const PackageError &error) {
    std::cerr << error.code() << ": " << error.what() << std::endl;
    for (const auto &name : error.names()) {
        std::cerr << "  " << name << std::endl;
    }
    return -1;
}

int runInstall(std::vector<std::string> arguments) {
    std::filesystem::path project;
    if (!takeProjectOption(arguments, project)) return -1;
    if (arguments.size() != 1) {
        std::cerr << usage << std::endl;
        return -1;
    }
    try {
        const auto result = installPackage(absolutePath(arguments.front()),
                                           absolutePath(project));
        OrderedJson report{
            {"status", "installed"},
            {"namespace", result.manifest.name_space},
            {"package_version", result.manifest.package_version},
            {"sha256", result.sha256},
            {"destination", result.destination.generic_string()},
            {"feature_references", result.feature_references},
            {"feature_names", result.feature_names},
        };
        std::cout << report.dump(2) << std::endl;
    } catch (const PackageError &error) {
        return reportError(error);
    } catch (const std::exception &error) {
        std::cerr << "package_manifest_invalid: " << error.what() << std::endl;
        return -1;
    }
    return 0;
}

int runList(std::vector<std::string> arguments) {
    std::filesystem::path project;
    if (!takeProjectOption(arguments, project)) return -1;
    if (!arguments.empty()) {
        std::cerr << usage << std::endl;
        return -1;
    }
    try {
        const auto root = absolutePath(project);
        const auto installed = readInstalledPackages(root);
        auto packages = OrderedJson::array();
        bool digest_failed = false;
        for (const auto &entry : installed) {
            OrderedJson encoded{{"namespace", entry.name_space},
                                {"package_version", entry.package_version},
                                {"sha256", entry.sha256}};
            const auto directory = root / packageVendorDirectory /
                                   entry.name_space;
            std::error_code error;
            if (!std::filesystem::is_directory(directory, error) || error) {
                encoded["status"] = "missing";
                digest_failed = true;
            } else {
                // B4: the digest is checked by a production path, not only
                // written.  Editing a vendored package in place is visible.
                const auto observed = packageTreeDigest(directory);
                encoded["observed_sha256"] = observed;
                if (observed == entry.sha256) {
                    encoded["status"] = "installed";
                } else {
                    encoded["status"] = "modified";
                    digest_failed = true;
                }
            }
            packages.push_back(std::move(encoded));
        }
        std::cout << OrderedJson{{"packages", std::move(packages)}}.dump(2)
                  << std::endl;
        if (digest_failed) {
            std::cerr << "package_digest_mismatch: a vendored package does "
                         "not match the digest project.json recorded"
                      << std::endl;
            return -1;
        }
    } catch (const PackageError &error) {
        return reportError(error);
    } catch (const std::exception &error) {
        std::cerr << "package_manifest_invalid: " << error.what() << std::endl;
        return -1;
    }
    return 0;
}

int runInspect(std::vector<std::string> arguments) {
    if (arguments.size() != 1) {
        std::cerr << usage << std::endl;
        return -1;
    }
    try {
        const auto directory = absolutePath(arguments.front());
        const auto manifest = readPackageManifest(directory);
        auto report = manifestJson(manifest);
        report["directory"] = directory.generic_string();
        report["sdk_abi_expected"] = packageSdkAbiVersion();
        std::cout << report.dump(2) << std::endl;
    } catch (const PackageError &error) {
        return reportError(error);
    } catch (const std::exception &error) {
        std::cerr << "package_manifest_invalid: " << error.what() << std::endl;
        return -1;
    }
    return 0;
}

} // namespace

int runPackageCommand(int argc, char *argv[]) {
    std::vector<std::string> arguments;
    for (int index = 2; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    const std::string_view subcommand = argc > 1 ? argv[1] : "";
    if (subcommand == "install") return runInstall(std::move(arguments));
    if (subcommand == "list") return runList(std::move(arguments));
    if (subcommand == "inspect") return runInspect(std::move(arguments));

    std::cerr << "unknown package subcommand";
    if (!subcommand.empty()) std::cerr << ": " << subcommand;
    std::cerr << std::endl << usage << std::endl;
    return -1;
}

} // namespace Pelican::DevCli
