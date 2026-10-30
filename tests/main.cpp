#include "test_support.hpp"

namespace {

// Clears the per-test step label so failures never report a step from a previous test.
class StepResetListener final : public ::testing::EmptyTestEventListener {
public:
    void OnTestStart(const ::testing::TestInfo&) override { wall_test::g_current_test_step.clear(); }
};

}  // namespace

// Runs all registered GoogleTest cases.
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::UnitTest::GetInstance()->listeners().Append(new StepResetListener());
    return RUN_ALL_TESTS();
}
