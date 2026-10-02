#include "audiocommand.hpp"

#include "../project/audioanalysisformat.hpp"
#include "../project/graphcomposition.hpp"
#if PELICAN_WITH_AUDIO
#include "../core/audio/audioanalysis.hpp"
#endif

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican::DevCli {
namespace {

// A number option as JSON, so the settings parser judges it as it judges
// the RPC's (an integer stays an integer).
nlohmann::json numberOption(const std::string &text, const char *option) {
    try {
        const auto value = nlohmann::json::parse(text);
        if (value.is_number()) return value;
    } catch (const std::exception &) {
    }
    throw std::runtime_error(std::string{option} + " must be a number: " + text);
}

int runAnalyze(int argc, char *argv[]) {
    argparse::ArgumentParser program("pelican_cli audio analyze");
    program.add_argument("--asset").required().metavar("file").help("the sound (.wav or .ogg)");
    program.add_argument("--out").required().metavar("file.ktx2").help("the table; its manifest is <out>.import.json");
    program.add_argument("--features").default_value(std::string{}).metavar("rms,peak,bands");
    program.add_argument("--bands").default_value(std::string{}).metavar("N");
    program.add_argument("--hop-ms").default_value(std::string{}).metavar("MS");
    program.add_argument("--window-ms").default_value(std::string{}).metavar("MS");
    program.add_argument("--attack-ms").default_value(std::string{}).metavar("MS");
    program.add_argument("--release-ms").default_value(std::string{}).metavar("MS");
    program.add_argument("--channels").default_value(std::string{}).metavar("mix|left|right");
    program.parse_args(argc, argv);

    nlohmann::json settings = nlohmann::json::object();
    if (const auto features = program.get<std::string>("--features"); !features.empty()) {
        settings["features"] = nlohmann::json::array();
        std::stringstream list{features};
        for (std::string feature; std::getline(list, feature, ',');) settings["features"].push_back(feature);
    }
    const auto number = [&](const char *option) {
        if (const auto text = program.get<std::string>(option); !text.empty()) return numberOption(text, option);
        return nlohmann::json{};
    };
    if (auto value = number("--bands"); !value.is_null()) settings["bands"] = value;
    if (auto value = number("--hop-ms"); !value.is_null()) settings["hop_ms"] = value;
    if (auto value = number("--window-ms"); !value.is_null()) settings["window_ms"] = value;
    if (auto value = number("--attack-ms"); !value.is_null()) settings["smoothing"]["attack_ms"] = value;
    if (auto value = number("--release-ms"); !value.is_null())
        settings["smoothing"]["release_ms"] = value;
    if (const auto channels = program.get<std::string>("--channels"); !channels.empty())
        settings["channels"] = channels;

    const std::filesystem::path out{program.get<std::string>("--out")};
    if (out.extension() != ".ktx2") throw std::runtime_error("--out must name a .ktx2 file: " + out.string());
    const auto parsed = parseAudioAnalysisSettings(settings);
#if PELICAN_WITH_AUDIO
    const auto baked = bakeAudioAnalysis(std::filesystem::path{program.get<std::string>("--asset")}, out, parsed);
    std::cout << nlohmann::ordered_json{{"out", baked.out.generic_string()},
                                        {"manifest", baked.manifest.generic_string()},
                                        {"layout", baked.layout}}
                     .dump(2)
              << std::endl;
    return 0;
#else
    (void)parsed;
    throw GraphRegistryError("audio.analysis_unavailable@1", {{"reason", "built_without_audio"}});
#endif
}

} // namespace

int runAudioCommand(int argc, char *argv[]) {
    try {
        if (argc > 1 && std::string_view{argv[1]} == "analyze") return runAnalyze(argc - 1, argv + 1);
        std::cerr << "usage: pelican_cli audio analyze --asset <file> --out <file.ktx2> [--features rms,peak,bands] "
                     "[--bands N] [--hop-ms MS] [--window-ms MS] [--attack-ms MS] [--release-ms MS] "
                     "[--channels mix|left|right]"
                  << std::endl;
        return -1;
    } catch (const GraphRegistryError &error) {
        std::cerr << error.code() << " " << error.detail().dump() << std::endl;
        return -1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return -1;
    }
}

} // namespace Pelican::DevCli
