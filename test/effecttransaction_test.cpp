#include "../src/core/audio/audio.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/archetypemigration.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/effecttransaction.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/persistence/persistence.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/gameobjects.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace Pelican {
namespace {

static_assert(noexcept(
    std::declval<internal::PreparedEffectTransaction &>().flush(
        std::declval<SceneLoadFaultInjector *>(),
        std::declval<SceneLoadPhaseTrace &>())));

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path project;
    std::filesystem::path user;
    Sandbox() {
        base = std::filesystem::temp_directory_path() /
            ("pelican_wp380b_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        project = base / "project";
        user = base / "user";
        std::filesystem::create_directories(project);
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

void writeU16(std::ofstream &file, std::uint16_t value) {
    file.put(static_cast<char>(value & 0xff));
    file.put(static_cast<char>((value >> 8) & 0xff));
}

void writeU32(std::ofstream &file, std::uint32_t value) {
    writeU16(file, static_cast<std::uint16_t>(value));
    writeU16(file, static_cast<std::uint16_t>(value >> 16));
}

void writeTone(const std::filesystem::path &path) {
    std::ofstream file{path, std::ios::binary};
    file.write("RIFF", 4); writeU32(file, 44);
    file.write("WAVEfmt ", 8); writeU32(file, 16);
    writeU16(file, 1); writeU16(file, 1); writeU32(file, 8000);
    writeU32(file, 16000); writeU16(file, 2); writeU16(file, 16);
    file.write("data", 4); writeU32(file, 8);
    writeU16(file, 0); writeU16(file, 512);
    writeU16(file, 0); writeU16(file, static_cast<std::uint16_t>(-512));
}

void configure(const Sandbox &sandbox) {
    setupLogger();
    GET_MODULE(EngineLaunchConfig).headless = true;
    GET_MODULE(PathResolver).setup(
        sandbox.project, false,
        nlohmann::json{{"schema", "pelican.project"}, {"version", 1},
                       {"name", "wp380b"},
                       {"engine_min_version", "0.1.0"}}.dump(),
        sandbox.user);
}

class EffectFlushFault final : public SceneLoadFaultInjector {
    std::string participant_;
    std::string mutation_;
  public:
    EffectFlushFault(std::string participant, std::string mutation)
        : participant_{std::move(participant)},
          mutation_{std::move(mutation)} {}
    bool shouldMutate(SceneLoadFaultPhase phase,
                      std::string_view participant,
                      std::string_view mutation) noexcept override {
        return phase == SceneLoadFaultPhase::flush &&
               participant == participant_ && mutation == mutation_;
    }
};

} // namespace

TEST_CASE("WP380b effect sink rollback keeps the committed semantic snapshot",
          "[WP380b][effect][rollback][cpu][mutation]") {
    Sandbox sandbox;
    writeTone(sandbox.project / "tone.wav");
    FastModuleContainer modules;
    configure(sandbox);
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &audio = GET_MODULE(Audio);
    auto &persistence = GET_MODULE(Persistence);
    auto &loader = GET_MODULE(SceneLoader);
    GameContext context;

    const auto existing = context.audio().playSound("project://tone.wav");
    const auto baseline_voices = audio.voiceCountForTesting();
    const auto baseline_cursor = audio.nextHandleForTesting();
    context.audio().setBusVolume("se", 0.75F);
    context.setGameSettings({{"world", "B"}});

    SoundHandle failed_handle{};
    {
        internal::PreparedEffectTransaction transaction{persistence, loader};
        internal::EffectTransactionScope scope{transaction};
        failed_handle = context.audio().playSound("project://tone.wav");
        REQUIRE_THROWS_AS(context.render().setRayTracingDispatchExtent("trace", 1, 1, 1),
                          SceneTransactionMutationRejected);
        REQUIRE_THROWS_AS(context.render().setRayTracingCallableRecord("trace", 0, {}),
                          SceneTransactionMutationRejected);
        REQUIRE(context.audio().isPlaying(failed_handle));
        context.audio().stopSound(failed_handle);
        REQUIRE_FALSE(context.audio().isPlaying(failed_handle));
        context.audio().stopSound(existing);
        REQUIRE_FALSE(context.audio().isPlaying(existing));
        context.audio().setBusVolume("se", 0.50F);
        context.audio().setBusVolume("se", 0.25F);
        REQUIRE(context.audio().busVolume("se") == Catch::Approx(0.25F));
        context.setGameSettings({{"world", "C-before"}});
        context.saveSettings();
        context.setGameSettings({{"world", "C"}});
        context.saveSettings();
        REQUIRE(context.gameSettings().at("world") == "C");
        context.save().saveData("slot_c", {{"value", 379}});
        context.save().saveData("slot_c", {{"value", 380}});
        REQUIRE(context.save().loadData("slot_c") == nlohmann::json{{"value", 380}});
        const auto saves = context.save().listSaves();
        REQUIRE(saves.size() == 1);
        REQUIRE(saves.front().slot == "slot_c");
        REQUIRE(saves.front().timestamp == std::filesystem::file_time_type{});
        const std::array components{
            ComponentIdByType<LocalTransformComponent>::value};
        REQUIRE_THROWS_AS(GameObjects::createWithComponents(
                              components, [](std::span<void *> values) {
                                  *static_cast<LocalTransformComponent *>(
                                      values.front()) = {};
                              }),
                          SceneTransactionMutationRejected);
        REQUIRE_THROWS_AS(GameObjects::removeAll(),
                          SceneTransactionMutationRejected);
    }

    REQUIRE(audio.voiceCountForTesting() == baseline_voices);
    REQUIRE(context.audio().isPlaying(existing));
    REQUIRE(context.audio().busVolume("se") == Catch::Approx(0.75F));
    REQUIRE(context.gameSettings().at("world") == "B");
    REQUIRE_FALSE(std::filesystem::exists(
        sandbox.user / "saves" / "slot_c.json"));
    REQUIRE(audio.nextHandleForTesting() > baseline_cursor);

    internal::PreparedEffectTransaction next{persistence, loader};
    internal::EffectTransactionScope next_scope{next};
    const auto next_handle = context.audio().playSound("project://tone.wav");
    REQUIRE(next_handle != failed_handle);
}

TEST_CASE("WP380b retired objects expose their final snapshot and reject setters",
          "[WP380b][effect][retire][cpu][mutation]") {
    Sandbox sandbox;
    writeTone(sandbox.project / "tone.wav");
    FastModuleContainer modules;
    configure(sandbox);
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &core = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &persistence = GET_MODULE(Persistence);
    auto &loader = GET_MODULE(SceneLoader);

    const std::array components{
        ComponentIdByType<LocalTransformComponent>::value,
        ComponentIdByType<SpriteViewComponent>::value};
    auto create = ECSEntityMutation::prepareCreate(
        core, components, [](std::span<void *> values) {
            auto &local = *static_cast<LocalTransformComponent *>(values[0]);
            local.pos = {3.0F, 8.0F, 0.0F};
            local.scale = {1.0F, 1.0F, 1.0F};
            local.rotation = {};
            auto &sprite = *static_cast<SpriteViewComponent *>(values[1]);
            sprite.texture = "project://final.png";
        });
    const auto retired = create.entity();
    create.publish();
    create.finish();
    auto destroy = ECSEntityMutation::prepareDestroy(core, retired);
    destroy.publish();
    REQUIRE_FALSE(core.isAlive(retired));

    SceneLoadPhaseTrace trace;
    std::vector<CommittedWithEffectFailure::EffectError> errors;
    {
        internal::PreparedEffectTransaction transaction{persistence, loader};
        transaction.setRetiredObjects(std::span{&destroy, 1U});
        internal::EffectTransactionScope scope{transaction};
        internal::EffectDestroyScope destroy_scope;
        GameContext context;
        REQUIRE(context.localTransform(retired).pos.x == Catch::Approx(3.0F));
        REQUIRE(context.localTransform(retired).pos.y == Catch::Approx(8.0F));
        REQUIRE(context.spriteView(retired)->texture ==
                "project://final.png");
        REQUIRE_FALSE(context.setLocalTransform(retired, {}));
        REQUIRE_FALSE(context.setSpriteView(retired, {}));
        REQUIRE_FALSE(context.setSpriteTexture(retired,
                                               "project://other.png"));
        const auto invalid = context.audio().playSound("project://missing.wav");
        REQUIRE(invalid == SoundHandle{});
        transaction.flush(nullptr, trace);
        errors = transaction.effectErrors();
    }

    REQUIRE(errors.size() == 4);
    REQUIRE(errors[0].error_code ==
            "SceneRetiredObjectMutationRejected");
    REQUIRE(errors[0].phase == "destroy");
    REQUIRE(errors[1].error_code ==
            "SceneRetiredObjectMutationRejected");
    REQUIRE(errors[2].error_code ==
            "SceneRetiredObjectMutationRejected");
    REQUIRE(errors[3].api == "playSound");
    REQUIRE(errors[3].error_code == "audio_play_stage_failed@1");
    REQUIRE(errors[3].phase == "destroy");
    destroy.finish();
}

TEST_CASE("WP380b flush reports one failed resource and continues in call order",
          "[WP380b][effect][flush][cpu][mutation]") {
    Sandbox sandbox;
    writeTone(sandbox.project / "tone.wav");
    FastModuleContainer modules;
    configure(sandbox);
    auto &audio = GET_MODULE(Audio);
    auto &persistence = GET_MODULE(Persistence);
    auto &loader = GET_MODULE(SceneLoader);
    GameContext context;
    SceneLoadPhaseTrace trace;
    EffectFlushFault fault{"persistence",
                           std::string{scene_load_mutation::effect_save_data}};

    SoundHandle played{};
    std::vector<CommittedWithEffectFailure::EffectError> errors;
    {
        internal::PreparedEffectTransaction transaction{persistence, loader};
        internal::EffectTransactionScope scope{transaction};
        played = context.audio().playSound("project://tone.wav");
        context.save().saveData("slot_c", {{"value", 380}});
        context.setGameSettings({{"world", "D"}});
        context.saveSettings();
        transaction.flush(&fault, trace);
        errors = transaction.effectErrors();
    }

    REQUIRE(errors.size() == 1);
    REQUIRE(errors.front().api == "saveData");
    REQUIRE(errors.front().error_code == "persistence_write_failed@1");
    REQUIRE(context.audio().isPlaying(played));
    REQUIRE(audio.voiceCountForTesting() == 1);
    REQUIRE_FALSE(std::filesystem::exists(
        sandbox.user / "saves" / "slot_c.json"));
    REQUIRE(std::filesystem::is_regular_file(sandbox.user / "settings.json"));
    REQUIRE(context.gameSettings().at("world") == "D");
}

TEST_CASE("WP380b every fallible effect manifest row has a killing flush mutation",
          "[WP380b][effect][manifest][cpu][mutation]") {
    struct MutationCase {
        std::string_view participant;
        std::string_view mutation;
        std::string_view api;
        std::string_view error_code;
    };
    constexpr MutationCase cases[]{
        {"audio", scene_load_mutation::effect_audio_play,
         "playSound", "audio_play_failed@1"},
        {"audio", scene_load_mutation::effect_audio_stop,
         "stopSound", "audio_stop_failed@1"},
        {"audio", scene_load_mutation::effect_audio_bus_volume,
         "setBusVolume", "audio_bus_volume_failed@1"},
        {"persistence", scene_load_mutation::effect_save_data,
         "saveData", "persistence_write_failed@1"},
        {"persistence", scene_load_mutation::effect_save_settings,
         "saveSettings", "persistence_write_failed@1"},
    };

    for (const auto &test : cases) {
        DYNAMIC_SECTION(std::string{test.mutation}) {
            Sandbox sandbox;
            writeTone(sandbox.project / "tone.wav");
            FastModuleContainer modules;
            configure(sandbox);
            auto &audio = GET_MODULE(Audio);
            auto &persistence = GET_MODULE(Persistence);
            auto &loader = GET_MODULE(SceneLoader);
            GameContext context;
            SoundHandle existing{};
            if (test.mutation == scene_load_mutation::effect_audio_stop) {
                existing = context.audio().playSound("project://tone.wav");
            }
            EffectFlushFault fault{std::string{test.participant},
                                   std::string{test.mutation}};
            SceneLoadPhaseTrace trace;
            internal::PreparedEffectTransaction transaction{persistence,
                                                             loader};
            internal::EffectTransactionScope scope{transaction};
            if (test.mutation == scene_load_mutation::effect_audio_play) {
                const auto provisional =
                    context.audio().playSound("project://tone.wav");
                REQUIRE(context.audio().isPlaying(provisional));
            } else if (test.mutation ==
                       scene_load_mutation::effect_audio_stop) {
                context.audio().stopSound(existing);
                context.audio().stopSound(existing);
                REQUIRE_FALSE(context.audio().isPlaying(existing));
            } else if (test.mutation ==
                       scene_load_mutation::effect_audio_bus_volume) {
                context.audio().setBusVolume("se", 0.25F);
                context.audio().setBusVolume("se", 0.38F);
            } else if (test.mutation ==
                       scene_load_mutation::effect_save_data) {
                context.save().saveData("slot_c", {{"value", 379}});
                context.save().saveData("slot_c", {{"value", 380}});
            } else {
                context.setGameSettings({{"world", "C"}});
                context.saveSettings();
                context.setGameSettings({{"world", "D"}});
                context.saveSettings();
            }
            transaction.flush(&fault, trace);
            const auto errors = transaction.effectErrors();
            REQUIRE(errors.size() == 1);
            REQUIRE(errors.front().api == std::string{test.api});
            REQUIRE(errors.front().error_code ==
                    std::string{test.error_code});

            if (test.mutation == scene_load_mutation::effect_audio_stop) {
                // Ownership moves before the fallible backend destroy.
                REQUIRE(audio.voiceCountForTesting() == 0);
                REQUIRE(audio.backendVoiceCountForTesting() == 1);
            }
            if (test.mutation ==
                scene_load_mutation::effect_audio_bus_volume) {
                REQUIRE(context.audio().busVolume("se") == Catch::Approx(0.38F));
            }
        }
    }
}

TEST_CASE("WP380e effect capacity reports overflow and provisional stop frees its slot",
          "[WP380e][effect][capacity][cpu][mutation]") {
    Sandbox sandbox;
    writeTone(sandbox.project / "tone.wav");
    FastModuleContainer modules;
    configure(sandbox);
    auto &persistence = GET_MODULE(Persistence);
    auto &loader = GET_MODULE(SceneLoader);
    GameContext context;

    {
        internal::PreparedEffectTransaction transaction{persistence, loader};
        internal::EffectTransactionScope scope{transaction};
        std::size_t staged = 0;
        for (std::size_t index = 0; index < 257; ++index) {
            if (context.audio().playSound("project://tone.wav") != SoundHandle{}) {
                ++staged;
            }
        }
        REQUIRE(staged == internal::sceneEffectCapacity);
        REQUIRE(transaction.fixedErrorCount() == 1);
        const auto errors = transaction.effectErrors();
        REQUIRE(errors.size() == 1);
        REQUIRE(errors.front().error_code == "effect_outbox_full@1");
        REQUIRE(errors.front().error_fields.at("dropped") == 1);
    }

    {
        internal::PreparedEffectTransaction transaction{persistence, loader};
        internal::EffectTransactionScope scope{transaction};
        for (std::size_t index = 0; index < 300; ++index) {
            const auto handle = context.audio().playSound("project://tone.wav");
            REQUIRE(handle != SoundHandle{});
            context.audio().stopSound(handle);
        }
        REQUIRE(transaction.fixedErrorCount() == 0);
        REQUIRE(context.audio().playSound("project://tone.wav") != SoundHandle{});
    }
}

TEST_CASE("WP380e destroy staging and flush errors retain API call order",
          "[WP380e][effect][sequence][cpu][mutation]") {
    Sandbox sandbox;
    FastModuleContainer modules;
    configure(sandbox);
    auto &persistence = GET_MODULE(Persistence);
    auto &loader = GET_MODULE(SceneLoader);
    GameContext context;
    SceneLoadPhaseTrace trace;
    EffectFlushFault fault{"persistence",
                           std::string{scene_load_mutation::effect_save_data}};

    internal::PreparedEffectTransaction transaction{persistence, loader};
    internal::EffectTransactionScope scope{transaction};
    {
        internal::EffectDestroyScope destroy_scope;
        (void)context.audio().playSound("project://missing-wp380e.wav");
        context.save().saveData("sequence", {{"value", 380}});
        context.audio().setBusVolume("missing-bus", 0.5F);
    }
    transaction.flush(&fault, trace);
    const auto errors = transaction.effectErrors();
    REQUIRE(errors.size() == 3);
    REQUIRE(errors[0].api == "playSound");
    REQUIRE(errors[0].phase == "destroy");
    REQUIRE(errors[1].api == "saveData");
    REQUIRE(errors[1].phase == "flush");
    REQUIRE(errors[2].api == "setBusVolume");
    REQUIRE(errors[2].phase == "destroy");
}

} // namespace Pelican
