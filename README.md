# A ROS2 RMW implementation for Eclipse Cyclone DDS

With the code in this repository, it is possible to use [*ROS2*](https://index.ros.org/doc/ros2)
with [*Eclipse Cyclone DDS*](https://github.com/eclipse-cyclonedds/cyclonedds) as the underlying DDS
implementation.

## Getting, building and using it

All it takes to get Cyclone DDS support into ROS2 is to clone this repository into the ROS2 workspace
source directory, and then run colcon build in the usual manner:

    cd ros2_ws/src
    git clone https://github.com/ros2/rmw_cyclonedds
    git clone https://github.com/eclipse-cyclonedds/cyclonedds
    cd ..
    rosdep install --from src -i
    colcon build
    export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp

This seems to work fine on Linux with a binary ROS2 installation as well as when building ROS2 from
source.  On macOS it has only been tested in a source build on a machine in an "unsupported"
configuration (macOS 10.14 with SIP enabled, instead of 10.12 with SIP disabled), and apart from a
few details that are caused by the machine configuration, that works fine, too.  There is no reason
why it wouldn't work the same on Windows, but I haven't tried.

If you want to use a pre-existing installation of Cyclone DDS, you don't need to clone it, but you
may have to tell CMake where to look for it using the `CycloneDDS_DIR` variable.  That also appears
to be the case if there are other packages in the ROS2 workspace that you would like to use Cyclone
DDS directly instead of via the ROS2 abstraction.

## Known limitations

Cyclone DDS doesn't yet implement the DDS Security standard, nor does it fully implement
the Lifespan, Deadline and some of the Liveliness QoS modes.  Consequently these features
of ROS2 are also not yet supported when using Cyclone DDS.
