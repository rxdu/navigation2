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

#include "nav2_route/route_server.hpp"

using nav2_util::declare_parameter_if_not_declared;
using std::placeholders::_1;

namespace nav2_route
{

RouteServer::RouteServer(const rclcpp::NodeOptions & options)
: nav2_util::LifecycleNode("route_server", "", options)
{}

nav2_util::CallbackReturn
RouteServer::on_configure(const rclcpp_lifecycle::State & /*state*/)
{
  RCLCPP_INFO(get_logger(), "Configuring");

  tf_ = std::make_shared<tf2_ros::Buffer>(get_clock());
  auto timer_interface = std::make_shared<tf2_ros::CreateTimerROS>(
    get_node_base_interface(),
    get_node_timers_interface());
  tf_->setCreateTimerInterface(timer_interface);
  transform_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_);

  auto node = shared_from_this();
  graph_vis_publisher_ =
    node->create_publisher<visualization_msgs::msg::MarkerArray>(
      "route_graph", rclcpp::QoS(rclcpp::KeepLast(1)).transient_local().reliable());

  action_server_ = std::make_unique<ActionServerBasic>(
    node, "compute_route",
    std::bind(&RouteServer::computeRoute, this),
    nullptr, std::chrono::milliseconds(500), true);

 declare_parameter_if_not_declared(
    node, "route_frame", rclcpp::ParameterValue(std::string("map")));
  declare_parameter_if_not_declared(
    node, "base_frame", rclcpp::ParameterValue(std::string("base_link")));
  declare_parameter_if_not_declared(
    node, "max_planning_time", rclcpp::ParameterValue(2.0));

  route_frame_ = node->get_parameter("route_frame").as_string(); 
  base_frame_ = node->get_parameter("base_frame").as_string(); 
  max_planning_time_ = node->get_parameter("max_planning_time").as_double(); 

  // Load graph and convert poses to the route frame, if required
  graph_loader_ = std::make_shared<GraphFileLoader>(node, tf_, route_frame_);
  if (!graph_loader_->loadGraphFromFile(graph_)) {
    return nav2_util::CallbackReturn::FAILURE;
  }

  return nav2_util::CallbackReturn::SUCCESS;
}

nav2_util::CallbackReturn
RouteServer::on_activate(const rclcpp_lifecycle::State & /*state*/)
{
  RCLCPP_INFO(get_logger(), "Activating");

  action_server_->activate();
  graph_vis_publisher_->on_activate();
  graph_vis_publisher_->publish(utils::toMsg(graph_, route_frame_, this->now()));

  // Add callback for dynamic parameters
  dyn_params_handler_ = this->add_on_set_parameters_callback(
    std::bind(&RouteServer::dynamicParametersCallback, this, _1));

  // create bond connection
  createBond();

  return nav2_util::CallbackReturn::SUCCESS;
}

nav2_util::CallbackReturn
RouteServer::on_deactivate(const rclcpp_lifecycle::State & /*state*/)
{
  RCLCPP_INFO(get_logger(), "Deactivating");

  action_server_->deactivate();
  graph_vis_publisher_->on_deactivate();
  dyn_params_handler_.reset();

  // destroy bond connection
  destroyBond();

  return nav2_util::CallbackReturn::SUCCESS;
}

nav2_util::CallbackReturn
RouteServer::on_cleanup(const rclcpp_lifecycle::State & /*state*/)
{
  RCLCPP_INFO(get_logger(), "Cleaning up");
  action_server_.reset();
  graph_vis_publisher_.reset();
  transform_listener_.reset();
  tf_.reset();
  graph_.clear();
  return nav2_util::CallbackReturn::SUCCESS;
}

nav2_util::CallbackReturn
RouteServer::on_shutdown(const rclcpp_lifecycle::State &)
{
  RCLCPP_INFO(get_logger(), "Shutting down");
  return nav2_util::CallbackReturn::SUCCESS;
}

NodeExtents
RouteServer::findStartandGoalNodeIDs(std::shared_ptr<const ActionBasicGoal> goal)
{
  // If not using the poses, then use the requests Node IDs to establish start and goal
  if (!goal->use_poses) {
    return {goal->start_id, goal->goal_id};
  }

  // Find request start pose
  geometry_msgs::msg::PoseStamped start_pose, goal_pose = goal->goal;
  if (goal->use_start) {
    start_pose = goal->start;
  } else {
    nav2_util::getCurrentPose(start_pose, *tf_, route_frame_, base_frame_);
  }

  // If start or goal not provided in route_frame, transform
  if (start_pose.header.frame_id != route_frame_) {
    RCLCPP_INFO(
      get_logger(),
      "Request start pose not in %s frame. Converting %s to route server frame.",
      start_pose.header.frame_id.c_str(), route_frame_.c_str());
    nav2_util::transformPoseInTargetFrame(start_pose, start_pose, *tf_, route_frame_);
  }

  if (goal_pose.header.frame_id != route_frame_) {
    RCLCPP_INFO(
      get_logger(),
      "Request goal pose not in %s frame. Converting %s to route server frame.",
      start_pose.header.frame_id.c_str(), route_frame_.c_str());
    nav2_util::transformPoseInTargetFrame(goal_pose, goal_pose, *tf_, route_frame_);
  }

  // Find closest route nodes to start and goal to plan between
  unsigned int start_route = 0, end_route = 5;
  // node_spatial_quadtree_->findNearestNodesToPoses(start_pose, goal_pose, start_route, end_route);//TODO
  return {start_route, end_route};
}

void
RouteServer::computeRoute()
{
  std::lock_guard<std::mutex> lock(dynamic_params_lock_);
  auto start_time = this->now();
  auto goal = action_server_->get_current_goal();
  auto result = std::make_shared<ActionBasicResult>();

  // Make sure request is valid
  if (!action_server_ || !action_server_->is_server_active()) {
    RCLCPP_DEBUG(get_logger(), "Action server unavailable or inactive. Stopping.");
    action_server_->terminate_all();
    return;
  }

  if (action_server_->is_cancel_requested()) {
    RCLCPP_INFO(get_logger(), "Goal was canceled. Canceling route planning action.");
    action_server_->terminate_all();
    return;
  }

  if (action_server_->is_preempt_requested()) {
    goal = action_server_->accept_pending_goal();
  }

  // try {
  //   // Find the search boundaries
  //   unsigned int [start_route, end_route] = findStartandGoalNodeIDs(goal);

  //   // Compute the route via graph-search, returns a node-edge sequence
  //   auto route = route_planner_->findRoute(start_route, end_route);//TODO

  //   // Create a dense path for use and debugging visualization
  //   result->route = utils::toMsg(route, route_frame_);//TODO
  //   result->path = path_converter_->densify(route);//TODO
  // } catch (...) {
  //   // contextual exceptions or logging?
  // }

  auto cycle_duration = this->now() - start_time;
  result->planning_time = cycle_duration;
  if (max_planning_time_ && cycle_duration.seconds() > max_planning_time_) {
    RCLCPP_WARN(
      get_logger(),
      "Route planner missed its desired rate of %.4f Hz. Current loop rate is %.4f Hz",
      1 / max_planning_time_, 1 / cycle_duration.seconds());
  }

  action_server_->succeeded_current(result);
}

rcl_interfaces::msg::SetParametersResult
RouteServer::dynamicParametersCallback(std::vector<rclcpp::Parameter> parameters)
{
  std::lock_guard<std::mutex> lock(dynamic_params_lock_);
  
  using rcl_interfaces::msg::ParameterType;
  rcl_interfaces::msg::SetParametersResult result;
  for (auto parameter : parameters) {
    const auto & type = parameter.get_type();
    const auto & name = parameter.get_name();

    if (type == ParameterType::PARAMETER_DOUBLE) {
      if (name == "max_planning_time") {
        max_planning_time_ = parameter.as_double();
      }
    }
  }

  result.successful = true;
  return result;
}

}  // namespace nav2_route

#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
// This acts as a sort of entry point, allowing the component to be discoverable when its library
// is being loaded into a running process.
RCLCPP_COMPONENTS_REGISTER_NODE(nav2_route::RouteServer)
