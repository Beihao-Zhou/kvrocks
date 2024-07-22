/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include <gtest/gtest.h>
#include <test_base.h>

#include <iostream>
#include <memory>
#include <random>

#include "search/hnsw_indexer.h"
#include "search/indexer.h"
#include "search/search_encoding.h"
#include "search/value.h"
#include "storage/storage.h"

auto GetVectorKeys(std::vector<redis::KeyWithDistance>&& keys_by_dist) -> std::vector<std::string> {
  std::vector<std::string> result;
  for (auto&& [dist, key] : keys_by_dist) {
    result.push_back(key);
  }
  return result;
}

void InsertEntryIntoHnswIndex(std::string_view key, const kqir::NumericArray& vector, uint16_t target_level,
                              redis::HnswIndex* hnsw_index, engine::Storage* storage) {
  auto batch = storage->GetWriteBatchBase();
  auto s = hnsw_index->InsertVectorEntryInternal(key, vector, batch, target_level);
  ASSERT_TRUE(s.IsOK());
  auto status = storage->Write(storage->DefaultWriteOptions(), batch->GetWriteBatch());
  ASSERT_TRUE(status.ok());
}

void VerifyNodeMetadataAndNeighbours(redis::HnswNode* node, redis::HnswIndex* hnsw_index,
                                     std::unordered_set<std::string> expected_set) {
  auto s = node->DecodeMetadata(hnsw_index->search_key, hnsw_index->storage);
  ASSERT_TRUE(s.IsOK());
  auto node_meta = s.GetValue();
  EXPECT_EQ(node_meta.num_neighbours, static_cast<uint16_t>(expected_set.size()));
  node->DecodeNeighbours(hnsw_index->search_key, hnsw_index->storage);
  std::unordered_set<std::string> actual_set = {(node->neighbours).begin(), (node->neighbours).end()};
  EXPECT_EQ(actual_set, expected_set);
}

struct HnswIndexTest : TestBase {
  redis::HnswVectorFieldMetadata metadata;
  std::string ns = "hnsw_test_ns";
  std::string idx_name = "hnsw_test_idx";
  std::string key = "vector";
  std::unique_ptr<redis::HnswIndex> hnsw_index;

  HnswIndexTest() {
    metadata.vector_type = redis::VectorType::FLOAT64;
    metadata.dim = 3;
    metadata.m = 3;
    metadata.distance_metric = redis::DistanceMetric::L2;
    auto search_key = redis::SearchKey(ns, idx_name, key);
    hnsw_index = std::make_unique<redis::HnswIndex>(search_key, &metadata, storage_.get());
  }

  void TearDown() override { hnsw_index.reset(); }
};

TEST_F(HnswIndexTest, ComputeSimilarity) {
  redis::VectorItem vec1;
  auto status1 = redis::VectorItem::Create("1", {1.0, 1.2, 1.4}, hnsw_index->metadata, &vec1);
  ASSERT_TRUE(status1.IsOK());
  redis::VectorItem vec2;
  auto status2 = redis::VectorItem::Create("2", {3.0, 3.2, 3.4}, hnsw_index->metadata, &vec2);
  ASSERT_TRUE(status2.IsOK());
  redis::VectorItem vec3;  // identical to vec1
  auto status3 = redis::VectorItem::Create("3", {1.0, 1.2, 1.4}, hnsw_index->metadata, &vec3);
  ASSERT_TRUE(status3.IsOK());

  auto s1 = redis::ComputeSimilarity(vec1, vec3);
  ASSERT_TRUE(s1.IsOK());
  double similarity = s1.GetValue();
  EXPECT_EQ(similarity, 0.0);

  auto s2 = redis::ComputeSimilarity(vec1, vec2);
  ASSERT_TRUE(s2.IsOK());
  similarity = s2.GetValue();
  EXPECT_NEAR(similarity, std::sqrt(12), 1e-5);

  hnsw_index->metadata->distance_metric = redis::DistanceMetric::IP;
  auto s3 = redis::ComputeSimilarity(vec1, vec2);
  ASSERT_TRUE(s3.IsOK());
  similarity = s3.GetValue();
  EXPECT_NEAR(similarity, -(1.0 * 3.0 + 1.2 * 3.2 + 1.4 * 3.4), 1e-5);

  hnsw_index->metadata->distance_metric = redis::DistanceMetric::COSINE;
  double expected_res = (1.0 * 3.0 + 1.2 * 3.2 + 1.4 * 3.4) /
                        std::sqrt((1.0 * 1.0 + 1.2 * 1.2 + 1.4 * 1.4) * (3.0 * 3.0 + 3.2 * 3.2 + 3.4 * 3.4));
  auto s4 = redis::ComputeSimilarity(vec1, vec2);
  ASSERT_TRUE(s4.IsOK());
  similarity = s4.GetValue();
  EXPECT_NEAR(similarity, 1 - expected_res, 1e-5);

  hnsw_index->metadata->distance_metric = redis::DistanceMetric::L2;
}

TEST_F(HnswIndexTest, RandomizeLayer) {
  constexpr size_t kSampleSize = 50000;

  std::vector<uint16_t> layers;
  layers.reserve(kSampleSize);

  for (size_t i = 0; i < kSampleSize; ++i) {
    layers.push_back(hnsw_index->RandomizeLayer());
    EXPECT_GE(layers.back(), 0);
  }

  std::map<uint16_t, size_t> layer_frequency;
  for (const auto& layer : layers) {
    layer_frequency[layer]++;
  }

  uint16_t max_observed_layer = 0;
  for (const auto& [layer, freq] : layer_frequency) {
    // std::cout << "Layer: " << layer << " Frequency: " << freq << std::endl;
    if (layer > max_observed_layer) {
      max_observed_layer = layer;
    }
  }

  // Calculate expected frequencies for each layer based on the theoretical distribution
  std::vector<double> expected_frequencies(max_observed_layer + 1, 0);
  double normalization_factor = 1.0 / std::log(hnsw_index->metadata->m);
  double total_probability = 0.0;

  for (uint16_t i = 0; i <= max_observed_layer; ++i) {
    total_probability += std::exp(-i / normalization_factor);
  }

  for (uint16_t i = 0; i <= max_observed_layer; ++i) {
    double probability = std::exp(-i / normalization_factor) / total_probability;
    expected_frequencies[i] = kSampleSize * probability;
  }

  for (const auto& [layer, freq] : layer_frequency) {
    if (layer < expected_frequencies.size() / 3) {
      double expected_freq = expected_frequencies[layer];
      double deviation = std::abs(static_cast<double>(freq) - expected_freq) / expected_freq;
      EXPECT_LE(deviation, 0.1) << "Layer: " << layer << " Frequency: " << freq << " Expected: " << expected_freq;
    }
  }
}

TEST_F(HnswIndexTest, DefaultEntryPointNotFound) {
  auto initial_result = hnsw_index->DefaultEntryPoint(0);
  ASSERT_EQ(initial_result.GetCode(), Status::NotFound);
}

TEST_F(HnswIndexTest, DecodeNodesToVectorItems) {
  uint16_t layer = 1;
  std::string node_key1 = "node1";
  std::string node_key2 = "node2";
  std::string node_key3 = "node3";

  redis::HnswNode node1(node_key1, layer);
  redis::HnswNode node2(node_key2, layer);
  redis::HnswNode node3(node_key3, layer);

  redis::HnswNodeFieldMetadata metadata1(0, {1, 2, 3});
  redis::HnswNodeFieldMetadata metadata2(0, {4, 5, 6});
  redis::HnswNodeFieldMetadata metadata3(0, {7, 8, 9});

  auto batch = storage_->GetWriteBatchBase();
  node1.PutMetadata(&metadata1, hnsw_index->search_key, hnsw_index->storage, batch.Get());
  node2.PutMetadata(&metadata2, hnsw_index->search_key, hnsw_index->storage, batch.Get());
  node3.PutMetadata(&metadata3, hnsw_index->search_key, hnsw_index->storage, batch.Get());
  auto s = storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
  ASSERT_TRUE(s.ok());

  std::vector<std::string> keys = {node_key1, node_key2, node_key3};

  auto s1 = hnsw_index->DecodeNodesToVectorItems(keys, layer, hnsw_index->search_key, hnsw_index->storage,
                                                 hnsw_index->metadata);
  ASSERT_TRUE(s1.IsOK());
  auto vector_items = s1.GetValue();
  ASSERT_EQ(vector_items.size(), 3);
  EXPECT_EQ(vector_items[0].key, node_key1);
  EXPECT_EQ(vector_items[1].key, node_key2);
  EXPECT_EQ(vector_items[2].key, node_key3);
  EXPECT_TRUE(vector_items[0].vector == std::vector<double>({1, 2, 3}));
  EXPECT_TRUE(vector_items[1].vector == std::vector<double>({4, 5, 6}));
  EXPECT_TRUE(vector_items[2].vector == std::vector<double>({7, 8, 9}));
}

TEST_F(HnswIndexTest, SelectNeighbors) {
  redis::VectorItem vec1;
  auto status1 = redis::VectorItem::Create("1", {1.0, 1.0, 1.0}, hnsw_index->metadata, &vec1);
  ASSERT_TRUE(status1.IsOK());

  redis::VectorItem vec2;
  auto status2 = redis::VectorItem::Create("2", {2.0, 2.0, 2.0}, hnsw_index->metadata, &vec2);
  ASSERT_TRUE(status2.IsOK());

  redis::VectorItem vec3;
  auto status3 = redis::VectorItem::Create("3", {3.0, 3.0, 3.0}, hnsw_index->metadata, &vec3);
  ASSERT_TRUE(status3.IsOK());

  redis::VectorItem vec4;
  auto status4 = redis::VectorItem::Create("4", {4.0, 4.0, 4.0}, hnsw_index->metadata, &vec4);
  ASSERT_TRUE(status4.IsOK());

  redis::VectorItem vec5;
  auto status5 = redis::VectorItem::Create("5", {5.0, 5.0, 5.0}, hnsw_index->metadata, &vec5);
  ASSERT_TRUE(status5.IsOK());

  redis::VectorItem vec6;
  auto status6 = redis::VectorItem::Create("6", {6.0, 6.0, 6.0}, hnsw_index->metadata, &vec6);
  ASSERT_TRUE(status6.IsOK());

  redis::VectorItem vec7;
  auto status7 = redis::VectorItem::Create("7", {7.0, 7.0, 7.0}, hnsw_index->metadata, &vec7);
  ASSERT_TRUE(status7.IsOK());

  std::vector<redis::VectorItem> candidates = {vec3, vec2};
  auto s1 = hnsw_index->SelectNeighbors(vec1, candidates, 1);
  ASSERT_TRUE(s1.IsOK());
  auto selected = s1.GetValue();
  EXPECT_EQ(selected.size(), candidates.size());

  EXPECT_EQ(selected[0].key, vec2.key);
  EXPECT_EQ(selected[1].key, vec3.key);

  candidates = {vec4, vec2, vec5, vec7, vec3, vec6};
  auto s2 = hnsw_index->SelectNeighbors(vec1, candidates, 1);
  ASSERT_TRUE(s2.IsOK());
  selected = s2.GetValue();
  EXPECT_EQ(selected.size(), 3);

  EXPECT_EQ(selected[0].key, vec2.key);
  EXPECT_EQ(selected[1].key, vec3.key);
  EXPECT_EQ(selected[2].key, vec4.key);

  candidates = {vec4, vec2, vec5, vec7, vec3, vec6};
  auto s3 = hnsw_index->SelectNeighbors(vec1, candidates, 0);
  ASSERT_TRUE(s3.IsOK());
  selected = s3.GetValue();
  EXPECT_EQ(selected.size(), 6);

  EXPECT_EQ(selected[0].key, vec2.key);
  EXPECT_EQ(selected[1].key, vec3.key);
  EXPECT_EQ(selected[2].key, vec4.key);
  EXPECT_EQ(selected[3].key, vec5.key);
  EXPECT_EQ(selected[4].key, vec6.key);
  EXPECT_EQ(selected[5].key, vec7.key);
}

TEST_F(HnswIndexTest, SearchLayer) {
  uint16_t layer = 3;
  std::string node_key1 = "node1";
  std::string node_key2 = "node2";
  std::string node_key3 = "node3";
  std::string node_key4 = "node4";
  std::string node_key5 = "node5";

  redis::HnswNode node1(node_key1, layer);
  redis::HnswNode node2(node_key2, layer);
  redis::HnswNode node3(node_key3, layer);
  redis::HnswNode node4(node_key4, layer);
  redis::HnswNode node5(node_key5, layer);

  redis::HnswNodeFieldMetadata metadata1(0, {1.0, 2.0, 3.0});
  redis::HnswNodeFieldMetadata metadata2(0, {4.0, 5.0, 6.0});
  redis::HnswNodeFieldMetadata metadata3(0, {7.0, 8.0, 9.0});
  redis::HnswNodeFieldMetadata metadata4(0, {2.0, 3.0, 4.0});
  redis::HnswNodeFieldMetadata metadata5(0, {6.0, 6.0, 7.0});

  // Add Nodes
  auto batch = storage_->GetWriteBatchBase();
  node1.PutMetadata(&metadata1, hnsw_index->search_key, hnsw_index->storage, batch.Get());
  node2.PutMetadata(&metadata2, hnsw_index->search_key, hnsw_index->storage, batch.Get());
  node3.PutMetadata(&metadata3, hnsw_index->search_key, hnsw_index->storage, batch.Get());
  node4.PutMetadata(&metadata4, hnsw_index->search_key, hnsw_index->storage, batch.Get());
  node5.PutMetadata(&metadata5, hnsw_index->search_key, hnsw_index->storage, batch.Get());
  auto s = storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
  ASSERT_TRUE(s.ok());

  // Add Neighbours
  batch = storage_->GetWriteBatchBase();
  auto s1 = node1.AddNeighbour("node2", hnsw_index->search_key, hnsw_index->storage, batch.Get());
  ASSERT_TRUE(s1.IsOK());
  auto s2 = node1.AddNeighbour("node4", hnsw_index->search_key, hnsw_index->storage, batch.Get());
  ASSERT_TRUE(s2.IsOK());
  auto s3 = node2.AddNeighbour("node1", hnsw_index->search_key, hnsw_index->storage, batch.Get());
  ASSERT_TRUE(s3.IsOK());
  auto s4 = node2.AddNeighbour("node3", hnsw_index->search_key, hnsw_index->storage, batch.Get());
  ASSERT_TRUE(s1.IsOK());
  auto s5 = node3.AddNeighbour("node2", hnsw_index->search_key, hnsw_index->storage, batch.Get());
  ASSERT_TRUE(s5.IsOK());
  auto s6 = node3.AddNeighbour("node5", hnsw_index->search_key, hnsw_index->storage, batch.Get());
  ASSERT_TRUE(s6.IsOK());
  auto s7 = node4.AddNeighbour("node1", hnsw_index->search_key, hnsw_index->storage, batch.Get());
  ASSERT_TRUE(s7.IsOK());
  auto s8 = node5.AddNeighbour("node3", hnsw_index->search_key, hnsw_index->storage, batch.Get());
  ASSERT_TRUE(s8.IsOK());
  s = storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
  ASSERT_TRUE(s.ok());

  redis::VectorItem target_vector;
  auto status = redis::VectorItem::Create("target", {2.0, 3.0, 4.0}, hnsw_index->metadata, &target_vector);
  ASSERT_TRUE(status.IsOK());

  // Test with multiple entry points
  std::vector<std::string> entry_points = {"node3", "node2"};
  uint32_t ef_runtime = 3;

  auto s9 = hnsw_index->SearchLayer(layer, target_vector, ef_runtime, entry_points);
  ASSERT_TRUE(s9.IsOK());
  auto candidates = s9.GetValue();

  ASSERT_EQ(candidates.size(), ef_runtime);
  EXPECT_EQ(candidates[0].key, "node4");
  EXPECT_EQ(candidates[1].key, "node1");
  EXPECT_EQ(candidates[2].key, "node2");

  // Test with a single entry point
  entry_points = {"node5"};
  auto s10 = hnsw_index->SearchLayer(layer, target_vector, ef_runtime, entry_points);
  ASSERT_TRUE(s10.IsOK());
  candidates = s10.GetValue();

  ASSERT_EQ(candidates.size(), ef_runtime);
  EXPECT_EQ(candidates[0].key, "node4");
  EXPECT_EQ(candidates[1].key, "node1");
  EXPECT_EQ(candidates[2].key, "node2");

  // Test with different ef_runtime
  ef_runtime = 10;
  auto s11 = hnsw_index->SearchLayer(layer, target_vector, ef_runtime, entry_points);
  ASSERT_TRUE(s11.IsOK());
  candidates = s11.GetValue();

  ASSERT_EQ(candidates.size(), 5);
  EXPECT_EQ(candidates[0].key, "node4");
  EXPECT_EQ(candidates[1].key, "node1");
  EXPECT_EQ(candidates[2].key, "node2");
  EXPECT_EQ(candidates[3].key, "node5");
  EXPECT_EQ(candidates[4].key, "node3");
}

TEST_F(HnswIndexTest, InsertAndDeleteVectorEntry) {
  std::vector<double> vec1 = {11.0, 12.0, 13.0};
  std::vector<double> vec2 = {14.0, 15.0, 16.0};
  std::vector<double> vec3 = {17.0, 18.0, 19.0};
  std::vector<double> vec4 = {12.0, 13.0, 14.0};
  std::vector<double> vec5 = {15.0, 16.0, 17.0};

  std::string key1 = "n1";
  std::string key2 = "n2";
  std::string key3 = "n3";
  std::string key4 = "n4";
  std::string key5 = "n5";

  // Insert
  uint16_t target_level = 1;
  InsertEntryIntoHnswIndex(key1, vec1, target_level, hnsw_index.get(), storage_.get());

  rocksdb::PinnableSlice value;
  auto index_meta_key = hnsw_index->search_key.ConstructFieldMeta();
  auto s = storage_->Get(rocksdb::ReadOptions(), hnsw_index->storage->GetCFHandle(ColumnFamilyID::Search),
                         index_meta_key, &value);
  ASSERT_TRUE(s.ok());
  redis::HnswVectorFieldMetadata decoded_metadata;
  decoded_metadata.Decode(&value);
  ASSERT_TRUE(decoded_metadata.num_levels == 2);

  redis::HnswNode node1_layer0(key1, 0);
  VerifyNodeMetadataAndNeighbours(&node1_layer0, hnsw_index.get(), {});
  redis::HnswNode node1_layer1(key1, 1);
  VerifyNodeMetadataAndNeighbours(&node1_layer1, hnsw_index.get(), {});

  // Insert
  target_level = 3;
  InsertEntryIntoHnswIndex(key2, vec2, target_level, hnsw_index.get(), storage_.get());

  index_meta_key = hnsw_index->search_key.ConstructFieldMeta();
  s = storage_->Get(rocksdb::ReadOptions(), hnsw_index->storage->GetCFHandle(ColumnFamilyID::Search), index_meta_key,
                    &value);
  ASSERT_TRUE(s.ok());
  decoded_metadata.Decode(&value);
  ASSERT_TRUE(decoded_metadata.num_levels == 4);

  VerifyNodeMetadataAndNeighbours(&node1_layer0, hnsw_index.get(), {"n2"});
  VerifyNodeMetadataAndNeighbours(&node1_layer1, hnsw_index.get(), {"n2"});

  redis::HnswNode node2_layer0(key2, 0);
  VerifyNodeMetadataAndNeighbours(&node2_layer0, hnsw_index.get(), {"n1"});

  redis::HnswNode node2_layer1(key2, 1);
  VerifyNodeMetadataAndNeighbours(&node2_layer1, hnsw_index.get(), {"n1"});

  redis::HnswNode node2_layer2(key2, 2);
  VerifyNodeMetadataAndNeighbours(&node2_layer2, hnsw_index.get(), {});
  redis::HnswNode node2_layer3(key2, 3);
  VerifyNodeMetadataAndNeighbours(&node2_layer3, hnsw_index.get(), {});

  // Insert
  target_level = 2;
  InsertEntryIntoHnswIndex(key3, vec3, target_level, hnsw_index.get(), storage_.get());

  index_meta_key = hnsw_index->search_key.ConstructFieldMeta();
  s = storage_->Get(rocksdb::ReadOptions(), hnsw_index->storage->GetCFHandle(ColumnFamilyID::Search), index_meta_key,
                    &value);
  ASSERT_TRUE(s.ok());
  decoded_metadata.Decode(&value);
  ASSERT_TRUE(decoded_metadata.num_levels == 4);

  redis::HnswNode node3_layer2(key3, target_level);
  VerifyNodeMetadataAndNeighbours(&node3_layer2, hnsw_index.get(), {"n2"});
  redis::HnswNode node3_layer1(key3, 1);
  VerifyNodeMetadataAndNeighbours(&node3_layer1, hnsw_index.get(), {"n1", "n2"});

  // Insert
  target_level = 1;
  InsertEntryIntoHnswIndex(key4, vec4, target_level, hnsw_index.get(), storage_.get());

  redis::HnswNode node4_layer0(key4, 0);
  auto s11 = node4_layer0.DecodeMetadata(hnsw_index->search_key, hnsw_index->storage);
  ASSERT_TRUE(s11.IsOK());
  redis::HnswNodeFieldMetadata node4_layer0_meta = s11.GetValue();
  EXPECT_EQ(node4_layer0_meta.num_neighbours, 3);

  VerifyNodeMetadataAndNeighbours(&node1_layer1, hnsw_index.get(), {"n2", "n3", "n4"});
  VerifyNodeMetadataAndNeighbours(&node2_layer1, hnsw_index.get(), {"n1", "n3", "n4"});
  VerifyNodeMetadataAndNeighbours(&node3_layer1, hnsw_index.get(), {"n1", "n2", "n4"});

  // Insert n5 into layer 1
  InsertEntryIntoHnswIndex(key5, vec5, target_level, hnsw_index.get(), storage_.get());

  VerifyNodeMetadataAndNeighbours(&node2_layer1, hnsw_index.get(), {"n1", "n4", "n5"});
  VerifyNodeMetadataAndNeighbours(&node3_layer1, hnsw_index.get(), {"n1", "n5"});
  redis::HnswNode node4_layer1(key4, 1);
  VerifyNodeMetadataAndNeighbours(&node4_layer1, hnsw_index.get(), {"n1", "n2", "n5"});
  redis::HnswNode node5_layer1(key5, 1);
  VerifyNodeMetadataAndNeighbours(&node5_layer1, hnsw_index.get(), {"n2", "n3", "n4"});
  VerifyNodeMetadataAndNeighbours(&node1_layer0, hnsw_index.get(), {"n2", "n3", "n4", "n5"});
  redis::HnswNode node5_layer0(key5, 0);
  VerifyNodeMetadataAndNeighbours(&node5_layer0, hnsw_index.get(), {"n1", "n2", "n3", "n4"});

  // Delete n2
  auto batch = storage_->GetWriteBatchBase();
  auto s22 = hnsw_index->DeleteVectorEntry(key2, batch);
  ASSERT_TRUE(s22.IsOK());
  s = storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
  ASSERT_TRUE(s.ok());

  index_meta_key = hnsw_index->search_key.ConstructFieldMeta();
  s = storage_->Get(rocksdb::ReadOptions(), hnsw_index->storage->GetCFHandle(ColumnFamilyID::Search), index_meta_key,
                    &value);
  ASSERT_TRUE(s.ok());
  decoded_metadata.Decode(&value);
  ASSERT_TRUE(decoded_metadata.num_levels == 3);

  auto s23 = node2_layer3.DecodeMetadata(hnsw_index->search_key, hnsw_index->storage);
  EXPECT_TRUE(!s23.IsOK());
  auto s24 = node2_layer2.DecodeMetadata(hnsw_index->search_key, hnsw_index->storage);
  EXPECT_TRUE(!s24.IsOK());
  auto s25 = node2_layer1.DecodeMetadata(hnsw_index->search_key, hnsw_index->storage);
  EXPECT_TRUE(!s25.IsOK());
  auto s26 = node2_layer0.DecodeMetadata(hnsw_index->search_key, hnsw_index->storage);
  EXPECT_TRUE(!s26.IsOK());

  VerifyNodeMetadataAndNeighbours(&node3_layer2, hnsw_index.get(), {});
  VerifyNodeMetadataAndNeighbours(&node1_layer1, hnsw_index.get(), {"n3", "n4"});
  VerifyNodeMetadataAndNeighbours(&node3_layer1, hnsw_index.get(), {"n1", "n5"});
  VerifyNodeMetadataAndNeighbours(&node4_layer1, hnsw_index.get(), {"n1", "n5"});
  VerifyNodeMetadataAndNeighbours(&node5_layer1, hnsw_index.get(), {"n3", "n4"});
  VerifyNodeMetadataAndNeighbours(&node1_layer0, hnsw_index.get(), {"n3", "n4", "n5"});
  redis::HnswNode node3_layer0(key3, 0);
  VerifyNodeMetadataAndNeighbours(&node3_layer0, hnsw_index.get(), {"n1", "n4", "n5"});
  VerifyNodeMetadataAndNeighbours(&node4_layer0, hnsw_index.get(), {"n1", "n3", "n5"});
  VerifyNodeMetadataAndNeighbours(&node5_layer0, hnsw_index.get(), {"n1", "n3", "n4"});
}

TEST_F(HnswIndexTest, SearchKnnAndRange) {
  std::vector<double> query_vector = {31.0, 32.0, 23.0};
  uint32_t k = 3;
  auto s1 = hnsw_index->KnnSearch(query_vector, k);
  ASSERT_FALSE(s1.IsOK());
  EXPECT_EQ(s1.GetCode(), Status::NotFound);

  std::vector<double> vec1 = {11.0, 12.0, 13.0};
  std::vector<double> vec2 = {14.0, 15.0, 16.0};
  std::vector<double> vec3 = {17.0, 18.0, 19.0};
  std::vector<double> vec4 = {12.0, 13.0, 14.0};
  std::vector<double> vec5 = {30.0, 40.0, 35.0};
  std::vector<double> vec6 = {10.0, 9.0, 8.0};
  std::vector<double> vec7 = {7.0, 6.0, 5.0};
  std::vector<double> vec8 = {36.0, 37.0, 38.0};

  std::string key1 = "key1";
  std::string key2 = "key2";
  std::string key3 = "key3";
  std::string key4 = "key4";
  std::string key5 = "key5";
  std::string key6 = "key5";
  std::string key7 = "key5";
  std::string key8 = "key5";

  uint16_t target_level = 1;
  InsertEntryIntoHnswIndex(key1, vec1, target_level, hnsw_index.get(), storage_.get());

  // Search when HNSW graph contains less than k nodes
  auto s3 = hnsw_index->KnnSearch(query_vector, k);
  ASSERT_TRUE(s3.IsOK());
  auto key_strs = GetVectorKeys(std::move(s3.GetValue()));
  std::vector<std::string> expected = {"key1"};
  EXPECT_EQ(key_strs, expected);

  target_level = 2;
  InsertEntryIntoHnswIndex(key2, vec2, target_level, hnsw_index.get(), storage_.get());

  target_level = 0;
  InsertEntryIntoHnswIndex(key3, vec3, target_level, hnsw_index.get(), storage_.get());

  // Search when HNSW graph contains exactly k nodes
  auto s6 = hnsw_index->KnnSearch(query_vector, k);
  ASSERT_TRUE(s6.IsOK());
  key_strs = GetVectorKeys(std::move(s6.GetValue()));
  expected = {"key3", "key2", "key1"};
  EXPECT_EQ(key_strs, expected);

  target_level = 1;
  InsertEntryIntoHnswIndex(key4, vec4, target_level, hnsw_index.get(), storage_.get());

  target_level = 0;
  InsertEntryIntoHnswIndex(key5, vec5, target_level, hnsw_index.get(), storage_.get());

  // Search when HNSW graph contains more than k nodes
  auto s9 = hnsw_index->KnnSearch(query_vector, k);
  ASSERT_TRUE(s9.IsOK());
  key_strs = GetVectorKeys(std::move(s9.GetValue()));
  expected = {"key5", "key3", "key2"};
  EXPECT_EQ(key_strs, expected);

  // Edge case: If ef_runtime is smaller than k, enlarge ef_runtime equal to k
  hnsw_index->metadata->ef_runtime = 1;
  auto s10 = hnsw_index->KnnSearch(query_vector, k);
  ASSERT_TRUE(s10.IsOK());
  key_strs = GetVectorKeys(std::move(s10.GetValue()));
  expected = {"key5", "key3", "key2"};
  EXPECT_EQ(key_strs, expected);
}
