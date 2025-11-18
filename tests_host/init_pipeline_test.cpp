/**
 * @file init_pipeline_test.cpp
 * @brief Unit tests for InitializationPipeline
 *
 * Tests the initialization pipeline infrastructure without requiring full ESP-IDF.
 * Uses mock phases to verify pipeline behavior (execution order, error handling).
 */

#include "app/init_phase.hpp"
#include "gtest/gtest.h"
#include "esp_err.h"

namespace {

// Mock phase for testing - records execution order
class MockPhase : public app::InitPhase {
 public:
  MockPhase(const char* name, bool critical, esp_err_t result, int* execution_counter, int expected_order)
      : name_(name), critical_(critical), result_(result),
        execution_counter_(execution_counter), expected_order_(expected_order),
        actual_order_(-1) {}

  esp_err_t Execute() override {
    actual_order_ = (*execution_counter_)++;
    return result_;
  }

  const char* GetName() const override { return name_; }
  bool IsCritical() const override { return critical_; }

  int GetActualOrder() const { return actual_order_; }
  int GetExpectedOrder() const { return expected_order_; }

 private:
  const char* name_;
  bool critical_;
  esp_err_t result_;
  int* execution_counter_;
  int expected_order_;
  int actual_order_;
};

class InitPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    execution_counter_ = 0;
  }

  int execution_counter_;
};

}  // namespace

TEST_F(InitPipelineTest, EmptyPipelineReturnsTrue) {
  app::InitializationPipeline pipeline;
  EXPECT_TRUE(pipeline.Execute());
}

TEST_F(InitPipelineTest, PhasesExecuteInRegistrationOrder) {
  app::InitializationPipeline pipeline;

  auto phase1 = std::make_unique<MockPhase>("Phase1", false, ESP_OK, &execution_counter_, 0);
  auto phase2 = std::make_unique<MockPhase>("Phase2", false, ESP_OK, &execution_counter_, 1);
  auto phase3 = std::make_unique<MockPhase>("Phase3", false, ESP_OK, &execution_counter_, 2);

  MockPhase* phase1_ptr = phase1.get();
  MockPhase* phase2_ptr = phase2.get();
  MockPhase* phase3_ptr = phase3.get();

  pipeline.AddPhase(std::move(phase1));
  pipeline.AddPhase(std::move(phase2));
  pipeline.AddPhase(std::move(phase3));

  EXPECT_TRUE(pipeline.Execute());

  EXPECT_EQ(0, phase1_ptr->GetActualOrder());
  EXPECT_EQ(1, phase2_ptr->GetActualOrder());
  EXPECT_EQ(2, phase3_ptr->GetActualOrder());
}

TEST_F(InitPipelineTest, NonCriticalPhaseFailureContinuesPipeline) {
  app::InitializationPipeline pipeline;

  auto phase1 = std::make_unique<MockPhase>("Phase1", false, ESP_OK, &execution_counter_, 0);
  auto phase2 = std::make_unique<MockPhase>("Phase2", false, ESP_FAIL, &execution_counter_, 1);  // Non-critical failure
  auto phase3 = std::make_unique<MockPhase>("Phase3", false, ESP_OK, &execution_counter_, 2);

  MockPhase* phase3_ptr = phase3.get();

  pipeline.AddPhase(std::move(phase1));
  pipeline.AddPhase(std::move(phase2));
  pipeline.AddPhase(std::move(phase3));

  // Non-critical failure should not prevent pipeline from continuing
  EXPECT_TRUE(pipeline.Execute());

  // Verify phase 3 was executed despite phase 2 failure
  EXPECT_EQ(2, phase3_ptr->GetActualOrder());
  EXPECT_EQ(3, execution_counter_);  // All 3 phases executed
}

// Note: Cannot test critical phase failure in unit test because FatalInitError() calls abort()
// This behavior must be verified through integration testing or manual verification

TEST_F(InitPipelineTest, SuccessfulPipelineReturnsTrue) {
  app::InitializationPipeline pipeline;

  pipeline.AddPhase(std::make_unique<MockPhase>("Phase1", true, ESP_OK, &execution_counter_, 0));
  pipeline.AddPhase(std::make_unique<MockPhase>("Phase2", true, ESP_OK, &execution_counter_, 1));
  pipeline.AddPhase(std::make_unique<MockPhase>("Phase3", false, ESP_OK, &execution_counter_, 2));

  EXPECT_TRUE(pipeline.Execute());
  EXPECT_EQ(3, execution_counter_);
}
