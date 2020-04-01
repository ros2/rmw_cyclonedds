# ROS2 RMW for Eclipse Cyclone DDS

**Easy, fast, reliable, small [Eclipse Cyclone DDS](https://github.com/eclipse-cyclonedds/cyclonedds) middleware** for ROS2. Make your **🐢 run like a 🚀** [Eclipse Cyclone DDS has great adopters](https://iot.eclipse.org/adopters/) and contributors in the ROS community and is an [Eclipse Foundation](https://www.eclipse.org) open source project of [Eclipse IoT](https://iot.eclipse.org) and [OpenADx](https://openadx.eclipse.org) (autonomous driving).

This package lets [*ROS2*](https://index.ros.org/doc/ros2) use [*Eclipse Cyclone DDS*](https://github.com/eclipse-cyclonedds/cyclonedds) as the underlying DDS implementation.
Cyclone DDS is ready to use. It seeks to give the fastest, easiest, and most robust ROS2 experience. Let the Cyclone blow you away!

1. Install:

  ```
  apt install ros-eloquent-rmw-cyclonedds-cpp
  ```
  or
  ```
  apt install ros-dashing-rmw-cyclonedds-cpp
  ```

2) Set env variable and run ROS2 apps as usual:

  ```export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp```

3) Confirm RMW: In Eloquent, to confirm which RMW you're using:

   ```ros2 doctor --report```


## Building from source and contributing

Note the `master` branch maintains compatibility with ROS releases Dashing and later, including the not-yet-released [*Foxy*](https://index.ros.org/doc/ros2/Releases/Release-Foxy-Fitzroy/).

If building ROS2 from source ([ros2.repos](https://github.com/ros2/ros2/blob/master/ros2.repos)), you already have this package and Cyclone DDS:

    cd /opt/ros/master
    rosdep install --from src -i
    colcon build
    export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
