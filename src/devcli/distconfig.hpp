#pragma once

#include <filesystem>
#include <string>

namespace Pelican::DevCli {

struct DistConfigOptions {
    bool with_rpc = false;
    bool with_seqplayer = false;
};

struct DistConfigResult {
    std::filesystem::path project_root;
    std::filesystem::path project_file;

    bool with_vat = false;
    bool with_exr = false;
    bool with_rpc = false;
    bool with_seqplayer = false;

    std::string vat_reason;
    std::string exr_reason;
    std::string rpc_reason;
    std::string seqplayer_reason;
};

DistConfigResult deriveDistConfig(const std::filesystem::path &project_arg,
                                  const DistConfigOptions &options);

std::string renderDistConfigPreset(const DistConfigResult &result);

int runDistConfigCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
