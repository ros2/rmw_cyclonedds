#!/usr/bin/env python3

from __future__ import annotations

import os
import sys

import rclpy
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node


def env_flag(name: str, default: bool) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def main(argv: list[str] | None = None) -> int:
    argv = argv if argv is not None else sys.argv
    node_name = os.environ.get("PHASE5_NODE_NAME", "phase5_param_server")
    start_parameter_services = env_flag("PHASE5_START_PARAMETER_SERVICES", True)

    rclpy.init(args=argv)
    node = Node(node_name, start_parameter_services=start_parameter_services)
    executor = SingleThreadedExecutor()
    executor.add_node(node)

    node.declare_parameter("foo", "bar")
    node.declare_parameter("bar", 42)

    print(f"server_ready name={node.get_name()}", flush=True)

    try:
        executor.spin()
    except KeyboardInterrupt:
        pass
    finally:
        executor.remove_node(node)
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
