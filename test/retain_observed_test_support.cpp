#include "retain_observed_test_support.hpp"
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <cstdlib>
#include <fstream>

namespace {
class RetainObservationListener final : public Catch::EventListenerBase {
    unsigned depth = 0;
    std::string testName;
    std::uint64_t partial = 0;
    bool skipped = false;
public:
    explicit RetainObservationListener(const Catch::IConfig *config) : EventListenerBase(config) {
        m_preferences.shouldReportAllAssertions = true;
    }
    void testCasePartialStarting(const Catch::TestCaseInfo &info, std::uint64_t part) override {
        testName = info.name;
        partial = part;
        depth = 0;
        skipped = false;
        Pelican::TestSupport::retainObservationAudit.reset();
    }
    void assertionStarting(const Catch::AssertionInfo &) override {
        if (depth && Pelican::TestSupport::declareActiveRetainFixture)
            Pelican::TestSupport::declareActiveRetainFixture();
    }
    void assertionEnded(const Catch::AssertionStats &stats) override {
        if (stats.assertionResult.getResultType() == Catch::ResultWas::ExplicitSkip) {
            skipped = true;
            return;
        }
        if (depth && Pelican::TestSupport::declareActiveRetainFixture)
            Pelican::TestSupport::declareActiveRetainFixture();
    }
    void sectionStarting(const Catch::SectionInfo &) override { ++depth; }
    void sectionEnded(const Catch::SectionStats &) override {
        if (--depth || skipped) return;
        if (const auto *path = std::getenv("RC7_FIXTURE_LOG"); path && *path) {
            std::ofstream out{path, std::ios::binary | std::ios::app};
            for (const auto &[key, entry] : Pelican::TestSupport::retainObservationAudit.entries)
                if (!entry.declared.empty() || !entry.observed.empty())
                    out << nlohmann::json{{"test",testName},{"partial",partial},{"fixture",key.fixture},
                        {"graph",key.graph},{"variant",key.variant},{"declared",entry.declared},
                        {"observed",entry.observed}}.dump() << '\n';
        }
        // The outer section ends after fixture teardown and before Catch
        // collects the test totals. CHECK failures affect the test exit code.
        for (const auto &mismatch : Pelican::TestSupport::retainObservationAudit.mismatches()) {
            INFO(mismatch);
            CHECK(mismatch.empty());
        }
    }
};
CATCH_REGISTER_LISTENER(RetainObservationListener)
}
