// Copyright 2026 Open Source Robotics Foundation, Inc.
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

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <rcl_interfaces/srv/list_parameters.hpp>
#include <rclcpp/rclcpp.hpp>

namespace
{

std::string get_env_or(const char * name, const std::string & fallback)
{
  const char * value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  return value;
}

double get_env_double(const char * name, double fallback)
{
  const char * value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  return std::stod(value);
}

using ListParameters = rcl_interfaces::srv::ListParameters;

std::shared_ptr<ListParameters::Response> wait_for_response(
  rclcpp::executors::SingleThreadedExecutor & executor,
  rclcpp::Client<ListParameters>::FutureAndRequestId & future,
  std::chrono::milliseconds timeout)
{
  auto rc = executor.spin_until_future_complete(future.future, timeout);
  if (rc != rclcpp::FutureReturnCode::SUCCESS) {
    throw std::runtime_error("request timed out");
  }
  return future.future.get();
}

void verify_names(const std::shared_ptr<ListParameters::Response> & response)
{
  bool saw_foo = false;
  bool saw_bar = false;
  for (const auto & name : response->result.names) {
    if (name == "foo") {
      saw_foo = true;
    }
    if (name == "bar") {
      saw_bar = true;
    }
  }
  if (!saw_foo || !saw_bar) {
    throw std::runtime_error("response did not contain expected parameters");
  }
}

}  // namespace

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);

  try {
    const auto node_name = get_env_or("PHASE5_NODE_NAME", "phase5_rclcpp_client");
    const auto service_name = get_env_or(
      "PHASE5_SERVICE_NAME", "/phase5_param_server/list_parameters");
    const auto wait_sec = get_env_double("PHASE5_WAIT_SEC", 15.0);
    const auto timeout_sec = get_env_double("PHASE5_TIMEOUT_SEC", 10.0);

    auto options = rclcpp::NodeOptions();
    options.start_parameter_services(false);
    auto node = std::make_shared<rclcpp::Node>(node_name, options);
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);

    auto client = node->create_client<ListParameters>(service_name);
    const auto wait_deadline = std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(wait_sec));
    while (std::chrono::steady_clock::now() < wait_deadline) {
      if (client->wait_for_service(std::chrono::milliseconds(200))) {
        break;
      }
    }
    if (!client->service_is_ready()) {
      throw std::runtime_error("service unavailable");
    }

    auto request = std::make_shared<ListParameters::Request>();
    request->prefixes = {};
    request->depth = 0;

    const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::duration<double>(timeout_sec));
    auto future = client->async_send_request(request);
    auto response = wait_for_response(executor, future, timeout);
    verify_names(response);

    std::cout << "client_result=ok" << std::endl;
    executor.remove_node(node);
    rclcpp::shutdown();
    return 0;
  } catch (const std::exception & exc) {
    std::cerr << "client_error=" << exc.what() << std::endl;
    rclcpp::shutdown();
    return 1;
  }
}
