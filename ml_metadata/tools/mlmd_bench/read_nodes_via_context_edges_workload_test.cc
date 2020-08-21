/* Copyright 2020 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "ml_metadata/tools/mlmd_bench/read_nodes_via_context_edges_workload.h"

#include <gtest/gtest.h>
#include "absl/memory/memory.h"
#include "ml_metadata/metadata_store/metadata_store.h"
#include "ml_metadata/metadata_store/metadata_store_factory.h"
#include "ml_metadata/metadata_store/test_util.h"
#include "ml_metadata/proto/metadata_store.pb.h"
#include "ml_metadata/proto/metadata_store_service.pb.h"
#include "ml_metadata/tools/mlmd_bench/fill_context_edges_workload.h"
#include "ml_metadata/tools/mlmd_bench/proto/mlmd_bench.pb.h"
#include "ml_metadata/tools/mlmd_bench/util.h"
#include "tensorflow/core/lib/core/status_test_util.h"

namespace ml_metadata {
namespace {

constexpr int kNumberOfOperations = 100;
constexpr int kNumberOfExistedTypesInDb = 100;
constexpr int kNumberOfExistedNodesInDb = 100;
constexpr int kNumberOfExistedContextEdgesInDb = 100;

// Configuration string for later FillContextEdges workload.
constexpr char kConfig[] = R"(
        non_context_node_popularity: {dirichlet_alpha : 1000}
        context_node_popularity: {dirichlet_alpha : 1000}
        num_edges: { minimum: 1 maximum: 10 }
      )";

// Enumerates the workload configurations as the test parameters that ensure
// test coverage.
std::vector<WorkloadConfig> EnumerateConfigs() {
  std::vector<WorkloadConfig> configs;
  std::vector<ReadNodesViaContextEdgesConfig::Specification> specifications = {
      ReadNodesViaContextEdgesConfig::ARTIFACTS_BY_CONTEXT,
      ReadNodesViaContextEdgesConfig::EXECUTIONS_BY_CONTEXT,
      ReadNodesViaContextEdgesConfig::CONTEXTS_BY_ARTIFACT,
      ReadNodesViaContextEdgesConfig::CONTEXTS_BY_EXECUTION};

  for (const ReadNodesViaContextEdgesConfig::Specification& specification :
       specifications) {
    WorkloadConfig config;
    config.set_num_operations(kNumberOfOperations);
    config.mutable_read_nodes_via_context_edges_config()->set_specification(
        specification);
    configs.push_back(config);
  }

  return configs;
}

// Inserts some context edges(Attributions / Associations) into db so that we
// can read nodes via these context edges. Returns detailed error if query
// executions failed.
tensorflow::Status InsertContextEdgesInDb(const int64 num_attributions,
                                          const int64 num_associations,
                                          MetadataStore& store) {
  // Inserts Attributions.
  {
    FillContextEdgesConfig fill_context_edges_config =
        testing::ParseTextProtoOrDie<FillContextEdgesConfig>(kConfig);
    fill_context_edges_config.set_specification(
        FillContextEdgesConfig::ATTRIBUTION);
    std::unique_ptr<FillContextEdges> prepared_db_workload =
        absl::make_unique<FillContextEdges>(
            FillContextEdges(fill_context_edges_config, num_attributions));
    TF_RETURN_IF_ERROR(prepared_db_workload->SetUp(&store));
    for (int64 i = 0; i < prepared_db_workload->num_operations(); ++i) {
      OpStats op_stats;
      TF_RETURN_IF_ERROR(prepared_db_workload->RunOp(i, &store, op_stats));
    }
  }

  // Inserts Associations.
  {
    FillContextEdgesConfig fill_context_edges_config =
        testing::ParseTextProtoOrDie<FillContextEdgesConfig>(kConfig);
    fill_context_edges_config.set_specification(
        FillContextEdgesConfig::ASSOCIATION);
    std::unique_ptr<FillContextEdges> prepared_db_workload =
        absl::make_unique<FillContextEdges>(
            FillContextEdges(fill_context_edges_config, num_associations));
    TF_RETURN_IF_ERROR(prepared_db_workload->SetUp(&store));
    for (int64 i = 0; i < prepared_db_workload->num_operations(); ++i) {
      OpStats op_stats;
      TF_RETURN_IF_ERROR(prepared_db_workload->RunOp(i, &store, op_stats));
    }
  }

  return tensorflow::Status::OK();
}

// Test fixture that uses the same data configuration for multiple following
// parameterized ReadNodesViaContextEdges tests.
// The parameter here is the specific Workload configuration that contains
// the ReadNodesViaContextEdges configuration and the number of operations.
class ReadNodesViaContextEdgesParameterizedTestFixture
    : public ::testing::TestWithParam<WorkloadConfig> {
 protected:
  void SetUp() override {
    ConnectionConfig mlmd_config;
    // Uses a fake in-memory SQLite database for testing.
    mlmd_config.mutable_fake_database();
    TF_ASSERT_OK(CreateMetadataStore(mlmd_config, &store_));
    read_nodes_via_context_edges_ =
        absl::make_unique<ReadNodesViaContextEdges>(ReadNodesViaContextEdges(
            GetParam().read_nodes_via_context_edges_config(),
            GetParam().num_operations()));
    TF_ASSERT_OK(InsertTypesInDb(
        /*num_artifact_types=*/kNumberOfExistedTypesInDb,
        /*num_execution_types=*/kNumberOfExistedTypesInDb,
        /*num_context_types=*/kNumberOfExistedTypesInDb, *store_));
    TF_ASSERT_OK(InsertNodesInDb(
        /*num_artifact_types=*/kNumberOfExistedNodesInDb,
        /*num_execution_types=*/kNumberOfExistedNodesInDb,
        /*num_context_types=*/kNumberOfExistedNodesInDb, *store_));
    TF_ASSERT_OK(InsertContextEdgesInDb(
        /*num_attributions=*/kNumberOfExistedContextEdgesInDb,
        /*num_associations=*/kNumberOfExistedContextEdgesInDb, *store_));
  }

  std::unique_ptr<ReadNodesViaContextEdges> read_nodes_via_context_edges_;
  std::unique_ptr<MetadataStore> store_;
};

// Tests the SetUpImpl() for ReadNodesViaContextEdges. Checks the SetUpImpl()
// indeed prepares a list of work items whose length is the same as the
// specified number of operations.
TEST_P(ReadNodesViaContextEdgesParameterizedTestFixture, SetUpImplTest) {
  TF_ASSERT_OK(read_nodes_via_context_edges_->SetUp(store_.get()));
  EXPECT_EQ(GetParam().num_operations(),
            read_nodes_via_context_edges_->num_operations());
}

// Tests the RunOpImpl() for ReadNodesViaContextEdges. Checks indeed all the
// work items have been executed and some bytes are transferred during the
// reading process.
TEST_P(ReadNodesViaContextEdgesParameterizedTestFixture, RunOpImplTest) {
  TF_ASSERT_OK(read_nodes_via_context_edges_->SetUp(store_.get()));

  int64 total_done = 0;
  ThreadStats stats;
  stats.Start();
  for (int64 i = 0; i < read_nodes_via_context_edges_->num_operations(); ++i) {
    OpStats op_stats;
    TF_ASSERT_OK(
        read_nodes_via_context_edges_->RunOp(i, store_.get(), op_stats));
    stats.Update(op_stats, total_done);
  }
  stats.Stop();
  EXPECT_EQ(stats.done(), GetParam().num_operations());
  // Checks that the transferred bytes is greater that 0(the reading process
  // indeed occurred).
  EXPECT_GT(stats.bytes(), 0);
}

INSTANTIATE_TEST_CASE_P(ReadNodesViaContextEdgesTest,
                        ReadNodesViaContextEdgesParameterizedTestFixture,
                        ::testing::ValuesIn(EnumerateConfigs()));

}  // namespace
}  // namespace ml_metadata
