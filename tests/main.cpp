#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <cstdio>
#include <cstdlib>

namespace {

// Flush before Doctest enters the case so CI can identify a hung test.
class TestProgress final : public doctest::IReporter {
public:
    explicit TestProgress(const doctest::ContextOptions&)
        : enabled(std::getenv("BZ_TEST_PROGRESS") != nullptr) {}

    void test_case_start(const doctest::TestCaseData& test) override {
        if (!enabled) return;
        std::fprintf(stderr, "[test] %s:%u %s\n", test.m_file.c_str(), test.m_line, test.m_name);
        std::fflush(stderr);
    }

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}

private:
    bool enabled;
};

} // namespace

DOCTEST_REGISTER_LISTENER("test-progress", 0, TestProgress);
