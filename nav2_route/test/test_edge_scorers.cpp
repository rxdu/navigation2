// Copyright (c) 2023, Samsung Research America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License. Reserved.

#include <math.h>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "rclcpp/rclcpp.hpp"
#include "nav2_util/lifecycle_node.hpp"
#include "nav2_util/service_client.hpp"
#include "nav2_util/node_thread.hpp"
#include "nav2_route/edge_scorer.hpp"
#include "nav2_msgs/srv/adjust_edges.hpp"
#include "nav2_costmap_2d/costmap_2d.hpp"
#include "nav2_costmap_2d/costmap_2d_publisher.hpp"

class RclCppFixture
{
public:
  RclCppFixture() {rclcpp::init(0, nullptr);}
  ~RclCppFixture() {rclcpp::shutdown();}
};
RclCppFixture g_rclcppfixture;

using namespace nav2_route;  // NOLINT

TEST(EdgeScorersTest, test_lifecycle)
{
  auto node = std::make_shared<nav2_util::LifecycleNode>("edge_scorer_test");
  EdgeScorer scorer(node);
}

TEST(EdgeScorersTest, test_api)
{
  // Tests basic API and default behavior. Also covers the DistanceScorer plugin.
  auto node = std::make_shared<nav2_util::LifecycleNode>("edge_scorer_test");
  EdgeScorer scorer(node);
  EXPECT_EQ(scorer.numPlugins(), 2);  // default DistanceScorer, AdjustEdgesScorer

  Node n1, n2;
  n1.nodeid = 1;
  n2.nodeid = 2;

  DirectionalEdge edge;
  edge.edgeid = 10;
  edge.start = &n1;
  edge.end = &n2;

  float traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 0.0);  // Because nodes coords are 0/0

  n1.coords.x = 1.0;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 1.0);  // Distance is now 1m

  // For full coverage, add in a speed limit tag to make sure it is applied appropriately
  float speed_limit = 0.8f;
  edge.metadata.setValue<float>("speed_limit", speed_limit);
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 1.25);  // 1m / 0.8 = 1.25
}

TEST(EdgeScorersTest, test_failed_api)
{
  // Expect failure since plugin does not exist
  auto node = std::make_shared<nav2_util::LifecycleNode>("edge_scorer_test");
  node->declare_parameter(
    "edge_cost_functions", rclcpp::ParameterValue(std::vector<std::string>{"FakeScorer"}));
  nav2_util::declare_parameter_if_not_declared(
    node, "FakeScorer.plugin", rclcpp::ParameterValue(std::string{"FakePluginPath"}));
  EXPECT_THROW(EdgeScorer scorer(node), pluginlib::PluginlibException);
}

TEST(EdgeScorersTest, test_invalid_edge_scoring)
{
  // Test API for the edge scorer to maintain proper state when a plugin
  // rejects and edge. Also covers the AdjustEdgesScorer plugin to demonstrate.
  auto node = std::make_shared<nav2_util::LifecycleNode>("route_server");
  auto node_thread = std::make_unique<nav2_util::NodeThread>(node);
  auto node2 = std::make_shared<rclcpp::Node>("my_node2");

  node->declare_parameter(
    "edge_cost_functions", rclcpp::ParameterValue(std::vector<std::string>{"AdjustEdgesScorer"}));
  nav2_util::declare_parameter_if_not_declared(
    node, "AdjustEdgesScorer.plugin",
    rclcpp::ParameterValue(std::string{"nav2_route::AdjustEdgesScorer"}));

  EdgeScorer scorer(node);
  EXPECT_EQ(scorer.numPlugins(), 1);  // AdjustEdgesScorer

  // Send service to set an edge as invalid
  auto srv_client =
    nav2_util::ServiceClient<nav2_msgs::srv::AdjustEdges>(
    "route_server/AdjustEdgesScorer/adjust_edges", node2);
  auto req = std::make_shared<nav2_msgs::srv::AdjustEdges::Request>();
  req->closed_edges.push_back(10u);
  req->adjust_edges.resize(1);
  req->adjust_edges[0].edgeid = 11u;
  req->adjust_edges[0].cost = 42.0;
  auto resp = srv_client.invoke(req, std::chrono::nanoseconds(1000000000));
  EXPECT_TRUE(resp->success);

  // Create edge to score
  Node n1, n2;
  n1.nodeid = 1;
  n2.nodeid = 2;
  n1.coords.x = 1.0;

  DirectionalEdge edge;
  edge.edgeid = 10;
  edge.start = &n1;
  edge.end = &n2;

  // The score function should return false since closed
  float traversal_cost = -1;
  EXPECT_FALSE(scorer.score(&edge, traversal_cost));

  // The score function should return true since no longer the problematic edge ID
  // and edgeid 42 as the dynamic cost of 42 assigned to it
  traversal_cost = -1;
  edge.edgeid = 11;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 42.0);

  // Try to re-open this edge
  auto req2 = std::make_shared<nav2_msgs::srv::AdjustEdges::Request>();
  req2->opened_edges.push_back(10u);
  auto resp2 = srv_client.invoke(req2, std::chrono::nanoseconds(1000000000));
  EXPECT_TRUE(resp2->success);

  // The score function should return true since now opened up
  traversal_cost = -1;
  edge.edgeid = 10;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));

  node_thread.reset();
}

TEST(EdgeScorersTest, test_penalty_scoring)
{
  // Test Penalty scorer plugin loading + penalizing on metadata values
  auto node = std::make_shared<nav2_util::LifecycleNode>("edge_scorer_test");

  node->declare_parameter(
    "edge_cost_functions", rclcpp::ParameterValue(std::vector<std::string>{"PenaltyScorer"}));
  nav2_util::declare_parameter_if_not_declared(
    node, "PenaltyScorer.plugin",
    rclcpp::ParameterValue(std::string{"nav2_route::PenaltyScorer"}));

  EdgeScorer scorer(node);
  EXPECT_EQ(scorer.numPlugins(), 1);  // PenaltyScorer

  // Create edge to score
  Node n1, n2;
  n1.nodeid = 1;
  n2.nodeid = 2;
  n1.coords.x = 1.0;

  DirectionalEdge edge;
  edge.edgeid = 10;
  edge.start = &n1;
  edge.end = &n2;
  float penalty = 10.0f;
  edge.metadata.setValue<float>("penalty", penalty);

  // The score function should return 10.0 from penalty value
  float traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 10.0);
}

TEST(EdgeScorersTest, test_costmap_scoring)
{
  // Test Penalty scorer plugin loading + penalizing on metadata values
  auto node = std::make_shared<nav2_util::LifecycleNode>("edge_scorer_test");
  auto node_thread = std::make_unique<nav2_util::NodeThread>(node);

  node->declare_parameter(
    "edge_cost_functions", rclcpp::ParameterValue(std::vector<std::string>{"CostmapScorer"}));
  nav2_util::declare_parameter_if_not_declared(
    node, "CostmapScorer.plugin",
    rclcpp::ParameterValue(std::string{"nav2_route::CostmapScorer"}));

  EdgeScorer scorer(node);
  EXPECT_EQ(scorer.numPlugins(), 1);  // CostmapScorer

  // Create edge to score
  Node n1, n2;
  n1.nodeid = 1;
  n2.nodeid = 2;
  n1.coords.x = 1.0;

  DirectionalEdge edge;
  edge.edgeid = 10;
  edge.start = &n1;
  edge.end = &n2;

  // The score function should return false because no costmap given
  float traversal_cost = -1;
  EXPECT_FALSE(scorer.score(&edge, traversal_cost));

  // Create a demo costmap: * = 100, - = 0, / = 254
  // * * * * - - - - - - - -
  // * * * * - - - - - - - -
  // * * * * - - - - - - - -
  // * * * * / / / / - - - -
  // * * * * / / / / - - - -
  // * * * * / / / / - - - -
  // * * * * / / / / - - - -
  // * * * * - - - - - - - -
  // * * * * - - - - - - - -
  // * * * * - - - - - - - -
  nav2_costmap_2d::Costmap2D * costmap =
    new nav2_costmap_2d::Costmap2D(100, 100, 0.1, 0.0, 0.0, 0);
  for (unsigned int i = 40; i <= 60; ++i) {
    for (unsigned int j = 40; j <= 60; ++j) {
      costmap->setCost(i, j, 254);
    }
  }
  for (unsigned int i = 0; i < 40; ++i) {
    for (unsigned int j = 0; j < 100; ++j) {
      costmap->setCost(i, j, 100);
    }
  }

  nav2_costmap_2d::Costmap2DPublisher publisher(
    node, costmap, "map", "global_costmap/costmap", true);
  publisher.on_activate();
  publisher.publishCostmap();

  // Give it a moment to receive the costmap
  rclcpp::Rate r(10);
  r.sleep();

  n1.coords.x = 5.0;
  n1.coords.y = 8.0;
  n2.coords.x = 8.0;
  n2.coords.y = 8.0;
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  // Segment in freespace
  EXPECT_EQ(traversal_cost, 0.0);

  n1.coords.x = 2.0;
  n1.coords.y = 2.0;
  n2.coords.x = 2.0;
  n2.coords.y = 8.0;
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  // Segment in 100 space
  EXPECT_NEAR(traversal_cost, 100.0 / 254.0, 0.01);

  n1.coords.x = 4.1;
  n1.coords.y = 4.1;
  n2.coords.x = 5.9;
  n2.coords.y = 5.9;
  traversal_cost = -1;
  // Segment in lethal space, won't fill in
  EXPECT_FALSE(scorer.score(&edge, traversal_cost));

  n1.coords.x = 1.0;
  n1.coords.y = 1.0;
  n2.coords.x = 6.0;
  n2.coords.y = 1.0;
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  // Segment in 0 and 100 space, use_max so 100 (normalized)
  EXPECT_NEAR(traversal_cost, 100.0 / 254.0, 0.01);

  n1.coords.x = -1.0;
  n1.coords.y = -1.0;
  n2.coords.x = 11.0;
  n2.coords.y = 11.0;
  traversal_cost = -1;
  // Off map, so invalid
  EXPECT_FALSE(scorer.score(&edge, traversal_cost));

  node_thread.reset();
}

TEST(EdgeScorersTest, test_costmap_scoring_alt_profile)
{
  // Test Penalty scorer plugin loading + penalizing on metadata values
  auto node = std::make_shared<nav2_util::LifecycleNode>("edge_scorer_test");
  auto node_thread = std::make_unique<nav2_util::NodeThread>(node);

  node->declare_parameter(
    "edge_cost_functions", rclcpp::ParameterValue(std::vector<std::string>{"CostmapScorer"}));
  nav2_util::declare_parameter_if_not_declared(
    node, "CostmapScorer.plugin",
    rclcpp::ParameterValue(std::string{"nav2_route::CostmapScorer"}));
  node->declare_parameter(
    "CostmapScorer.use_maximum", rclcpp::ParameterValue(false));
  node->declare_parameter(
    "CostmapScorer.invalid_on_collision", rclcpp::ParameterValue(false));
  node->declare_parameter(
    "CostmapScorer.invalid_off_map", rclcpp::ParameterValue(false));

  EdgeScorer scorer(node);
  EXPECT_EQ(scorer.numPlugins(), 1);  // CostmapScorer

  // Create edge to score
  Node n1, n2;
  n1.nodeid = 1;
  n2.nodeid = 2;
  n1.coords.x = 1.0;

  DirectionalEdge edge;
  edge.edgeid = 10;
  edge.start = &n1;
  edge.end = &n2;

  // Create a demo costmap: * = 100, - = 0, / = 254
  // * * * * - - - - - - - -
  // * * * * - - - - - - - -
  // * * * * - - - - - - - -
  // * * * * / / / / - - - -
  // * * * * / / / / - - - -
  // * * * * / / / / - - - -
  // * * * * / / / / - - - -
  // * * * * - - - - - - - -
  // * * * * - - - - - - - -
  // * * * * - - - - - - - -
  nav2_costmap_2d::Costmap2D * costmap =
    new nav2_costmap_2d::Costmap2D(100, 100, 0.1, 0.0, 0.0, 0);
  for (unsigned int i = 40; i <= 60; ++i) {
    for (unsigned int j = 40; j <= 60; ++j) {
      costmap->setCost(i, j, 254);
    }
  }
  for (unsigned int i = 0; i < 40; ++i) {
    for (unsigned int j = 0; j < 100; ++j) {
      costmap->setCost(i, j, 100);
    }
  }

  nav2_costmap_2d::Costmap2DPublisher publisher(
    node, costmap, "map", "global_costmap/costmap", true);
  publisher.on_activate();
  publisher.publishCostmap();

  // Give it a moment to receive the costmap
  rclcpp::Rate r(1);
  r.sleep();

  // Off map
  n1.coords.x = -1.0;
  n1.coords.y = -1.0;
  n2.coords.x = 11.0;
  n2.coords.y = 11.0;
  float traversal_cost = -1;
  // Off map, so cannot score
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 0.0);

  n1.coords.x = 4.1;
  n1.coords.y = 4.1;
  n2.coords.x = 5.9;
  n2.coords.y = 5.9;
  traversal_cost = -1;
  // Segment in lethal space, so score is maximum (1)
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_NEAR(traversal_cost, 1.0, 0.01);

  n1.coords.x = 1.0;
  n1.coords.y = 1.0;
  n2.coords.x = 6.0;
  n2.coords.y = 1.0;
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  // Segment in 0 and 100 space, 3m @ 100, 2m @ 0, averaged is 60
  EXPECT_NEAR(traversal_cost, 60.0 / 254.0, 0.01);

  node_thread.reset();
}

TEST(EdgeScorersTest, test_time_scoring)
{
  // Test Time scorer plugin loading
  auto node = std::make_shared<nav2_util::LifecycleNode>("edge_scorer_test");

  node->declare_parameter(
    "edge_cost_functions", rclcpp::ParameterValue(std::vector<std::string>{"TimeScorer"}));
  nav2_util::declare_parameter_if_not_declared(
    node, "TimeScorer.plugin",
    rclcpp::ParameterValue(std::string{"nav2_route::TimeScorer"}));

  EdgeScorer scorer(node);
  EXPECT_EQ(scorer.numPlugins(), 1);  // TimeScorer

  // Create edge to score
  Node n1, n2;
  n1.nodeid = 1;
  n2.nodeid = 2;
  n1.coords.x = 1.0;

  DirectionalEdge edge;
  edge.edgeid = 10;
  edge.start = &n1;
  edge.end = &n2;
  float time_taken = 10.0f;
  edge.metadata.setValue<float>("abs_time_taken", time_taken);

  // The score function should return 10.0 from time taken
  float traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 10.0);  // 10.0 * 1.0 weight

  // Without time taken or abs speed limit set, uses default max speed of 0.5 m/s
  edge.metadata.data.clear();
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 2.0);  // 1.0 m / 0.5 m/s * 1.0 weight

  // Use speed limit if set
  float speed_limit = 0.85;
  edge.metadata.setValue<float>("abs_speed_limit", speed_limit);
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_NEAR(traversal_cost, 1.1764, 0.001);  // 1.0 m / 0.85 m/s * 1.0 weight

  // Still use time taken measurements if given first
  edge.metadata.setValue<float>("abs_time_taken", time_taken);
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 10.0);  // 10.0 * 1.0 weight
}

TEST(EdgeScorersTest, test_semantic_scoring_key)
{
  // Test Time scorer plugin loading
  auto node = std::make_shared<nav2_util::LifecycleNode>("edge_scorer_test");

  node->declare_parameter(
    "edge_cost_functions", rclcpp::ParameterValue(std::vector<std::string>{"SemanticScorer"}));
  nav2_util::declare_parameter_if_not_declared(
    node, "SemanticScorer.plugin",
    rclcpp::ParameterValue(std::string{"nav2_route::SemanticScorer"}));

  std::vector<std::string> classes;
  classes.push_back("Test");
  classes.push_back("Test1");
  classes.push_back("Test2");
  nav2_util::declare_parameter_if_not_declared(
    node, "SemanticScorer.semantic_classes",
    rclcpp::ParameterValue(classes));

  for (unsigned int i = 0; i != classes.size(); i++) {
    nav2_util::declare_parameter_if_not_declared(
      node, "SemanticScorer." + classes[i],
      rclcpp::ParameterValue(static_cast<float>(i)));
  }

  EdgeScorer scorer(node);
  EXPECT_EQ(scorer.numPlugins(), 1);  // SemanticScorer

  // Create edge to score
  Node n1, n2;
  n1.nodeid = 1;
  n2.nodeid = 2;
  n1.coords.x = 1.0;

  DirectionalEdge edge;
  edge.edgeid = 10;
  edge.start = &n1;
  edge.end = &n2;

  // Should fail, since both nothing under key `class` nor metadata set at all
  float traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 0.0);  // nothing is set in semantics

  // Should be valid under the right key
  std::string test_n = "Test1";
  edge.metadata.setValue<std::string>("class", test_n);
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 1.0);  // 1.0 * 1.0 weight

  test_n = "Test2";
  edge.metadata.setValue<std::string>("class", test_n);
  n2.metadata.setValue<std::string>("class", test_n);
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 4.0);  // (2.0 + 2.0) * 1.0 weight

  // Cannot find, doesn't exist
  test_n = "Test4";
  edge.metadata.setValue<std::string>("class", test_n);
  n2.metadata.setValue<std::string>("class", test_n);
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 0.0);  // 0.0 * 1.0 weight
}

TEST(EdgeScorersTest, test_semantic_scoring_keys)
{
  // Test Time scorer plugin loading
  auto node = std::make_shared<nav2_util::LifecycleNode>("edge_scorer_test");

  node->declare_parameter(
    "edge_cost_functions", rclcpp::ParameterValue(std::vector<std::string>{"SemanticScorer"}));
  nav2_util::declare_parameter_if_not_declared(
    node, "SemanticScorer.plugin",
    rclcpp::ParameterValue(std::string{"nav2_route::SemanticScorer"}));
  nav2_util::declare_parameter_if_not_declared(
    node, "SemanticScorer.semantic_key",
    rclcpp::ParameterValue(std::string{""}));

  std::vector<std::string> classes;
  classes.push_back("Test");
  classes.push_back("Test1");
  classes.push_back("Test2");
  nav2_util::declare_parameter_if_not_declared(
    node, "SemanticScorer.semantic_classes",
    rclcpp::ParameterValue(classes));

  for (unsigned int i = 0; i != classes.size(); i++) {
    nav2_util::declare_parameter_if_not_declared(
      node, "SemanticScorer." + classes[i],
      rclcpp::ParameterValue(static_cast<float>(i)));
  }

  EdgeScorer scorer(node);
  EXPECT_EQ(scorer.numPlugins(), 1);  // SemanticScorer

  // Create edge to score
  Node n1, n2;
  n1.nodeid = 1;
  n2.nodeid = 2;
  n1.coords.x = 1.0;

  DirectionalEdge edge;
  edge.edgeid = 10;
  edge.start = &n1;
  edge.end = &n2;

  // Should fail, since both nothing under key `class` nor metadata set at all
  float traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 0.0);  // nothing is set in semantics

  // Should fail, since under the class key when the semantic key is empty string
  // so it will look for the keys themselves
  std::string test_n = "Test1";
  edge.metadata.setValue<std::string>("class", test_n);
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 0.0);  // 0.0 * 1.0 weight

  // Should succeed, since now actual class is a key, not a value of the `class` key
  test_n = "Test2";
  edge.metadata.setValue<std::string>(test_n, test_n);
  n2.metadata.setValue<std::string>(test_n, test_n);
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 4.0);  // (2.0 + 2.0) * 1.0 weight

  // Cannot find, doesn't exist
  edge.metadata.data.clear();
  n2.metadata.data.clear();
  test_n = "Test4";
  edge.metadata.setValue<std::string>(test_n, test_n);
  traversal_cost = -1;
  EXPECT_TRUE(scorer.score(&edge, traversal_cost));
  EXPECT_EQ(traversal_cost, 0.0);  // 0.0 * 1.0 weight
}