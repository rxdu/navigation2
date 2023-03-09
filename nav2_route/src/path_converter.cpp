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
// limitations under the License.

#include <string>
#include <limits>
#include <memory>
#include <vector>
#include <mutex>
#include <algorithm>

#include "nav2_route/path_converter.hpp"

namespace nav2_route
{

void PathConverter::configure(nav2_util::LifecycleNode::SharedPtr node)
{
  nav2_util::declare_parameter_if_not_declared(
    node, "path_density", rclcpp::ParameterValue(0.05));
  density_ = static_cast<float>(node->get_parameter("path_density").as_double());

  path_pub_ = node->create_publisher<nav_msgs::msg::Path>("plan", 1);
  path_pub_->on_activate();
}

nav_msgs::msg::Path PathConverter::densify(
  const Route & route,
  const ReroutingState & rerouting_info,
  const std::string & frame,
  const rclcpp::Time & now)
{
  nav_msgs::msg::Path path;
  path.header.stamp = now;
  path.header.frame_id = frame;

  // If we're rerouting and covering the same previous edge to start,
  // the path should contain the relevent partial information along edge
  // to avoid unnecessary free-space planning where state is retained
  if (rerouting_info.curr_edge) {
    const Coordinates & start = rerouting_info.closest_pt_on_edge;
    const Coordinates & end = rerouting_info.curr_edge->end->coords;
    interpolateEdge(start.x, start.y, end.x, end.y, path.poses);
  }

  // Fill in path via route edges
  for (unsigned int i = 0; i != route.edges.size(); i++) {
    const EdgePtr edge = route.edges[i];
    const Coordinates & start = edge->start->coords;
    const Coordinates & end = edge->end->coords;
    interpolateEdge(start.x, start.y, end.x, end.y, path.poses);
  }

  if (route.edges.empty()) {
    path.poses.push_back(utils::toMsg(route.start_node->coords.x, route.start_node->coords.y));
  } else {
    path.poses.push_back(
      utils::toMsg(route.edges.back()->end->coords.x, route.edges.back()->end->coords.y));
  }

  // publish path similar to planner server
  auto path_ptr = std::make_unique<nav_msgs::msg::Path>(path);
  path_pub_->publish(std::move(path_ptr));

  return path;
}

void PathConverter::interpolateEdge(
  float x0, float y0, float x1, float y1,
  std::vector<geometry_msgs::msg::PoseStamped> & poses)
{
  // Find number of points to populate by given density
  const float mag = hypotf(x1 - x0, y1 - y0);
  const unsigned int num_pts = ceil(mag / density_);
  const float iterpolated_dist = mag / num_pts;

  // Find unit vector direction
  float ux = (x1 - x0) / mag;
  float uy = (y1 - y0) / mag;

  // March along it until dist
  float x = x0;
  float y = y0;
  poses.push_back(utils::toMsg(x, y));

  unsigned int pt_ctr = 0;
  while (pt_ctr < num_pts - 1) {
    x += ux * iterpolated_dist;
    y += uy * iterpolated_dist;
    pt_ctr++;
    poses.push_back(utils::toMsg(x, y));
  }
}

}  // namespace nav2_route