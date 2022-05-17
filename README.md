# ROS 2 RMW for Eclipse Cyclone DDS

**Easy, fast, reliable, small [Eclipse Cyclone DDS](https://github.com/eclipse-cyclonedds/cyclonedds) Tier 1 ROS middleware** for ROS 2. Make your **🐢 run like a 🚀** [Eclipse Cyclone DDS has great adopters](https://iot.eclipse.org/adopters/) and contributors in the ROS community and is an [Eclipse Foundation](https://www.eclipse.org) open source project of [Eclipse IoT](https://iot.eclipse.org) and [OpenADx](https://openadx.eclipse.org) (autonomous driving).

This package lets [*ROS 2*](https://docs.ros.org/en/rolling/) use [*Eclipse Cyclone DDS*](https://github.com/eclipse-cyclonedds/cyclonedds) as the underlying DDS implementation.
Cyclone DDS is ready to use. It seeks to give the fastest, easiest, and most robust ROS 2 experience. Let the Cyclone blow you away!

1. Install:

   ```
   apt install ros-eloquent-rmw-cyclonedds-cpp
   ```
   or
   ```
   apt install ros-dashing-rmw-cyclonedds-cpp
   ```

2. Set env variable and run ROS 2 apps as usual:

   ```export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp```

3. Confirm RMW: In Eloquent and later, to confirm which RMW you're using:

   ```ros2 doctor --report```


## Performance recommendations

With large samples (100s of kilobytes), excessive latency can be caused by running out of space in the OS-level receive buffer. For this reason, on Linux, we recommend increasing the buffer size:
* Temporarily (until reboot): `sudo sysctl -w net.core.rmem_max=8388608 net.core.rmem_default=8388608`
* Permanently: `echo "net.core.rmem_max=8388608\nnet.core.rmem_default=8388608\n" | sudo tee /etc/sysctl.d/60-cyclonedds.conf`

## Debugging

So Cyclone isn't playing nice or not giving you the performance you had hoped for? That's not good... Please [file an issue against this repository](https://github.com/ros2/rmw_cyclonedds/issues/new)!

The `ddsperf` tool distributed with Cyclone DDS can be used to check that communication works *without* ROS. Run `ddsperf sanity` on two different machines - if the "mean" value is above `100000us`, there are likely network issues.

If you're having trouble with nodes discovering others or can't use multicast *at all* on your network setup, you can circumvent discovery:

  `export CYCLONEDDS_URI='<Discovery><Peers><Peer Address='myroshost.local' /><Peer Address='myroshost2.local' /></></>'`

Here are some ways to generate additional debugging info that can help identify the problem faster, and are helpful on an issue ticket:

* Configure Cyclone to create richer debugging output:

  * To see the output live:

    `export CYCLONEDDS_URI='<Tracing><Verbosity>trace</><Out>stderr</></>'`

  * To send to `/var/log/`:

    `export CYCLONEDDS_URI='<Tracing><Verbosity>trace</><Out>/var/log/cyclonedds.${CYCLONEDDS_PID}.log</></>'`

* Create a Wireshark capture:

  `wireshark -k -w wireshark.pcap.gz`

## Building from source and contributing

The following branches are actively maintained:

* `master`, which targets the upcoming ROS version, [*Foxy*](https://docs.ros.org/en/rolling/Releases/Release-Foxy-Fitzroy.html)
* `dashing-eloquent`, which maintains compatibility with ROS releases [*Dashing*](https://docs.ros.org/en/rolling/Releases/Release-Dashing-Diademata.html) and [*Eloquent*](https://docs.ros.org/en/rolling/Releases/Release-Eloquent-Elusor.html)

If building ROS 2 from source ([ros2.repos](https://github.com/ros2/ros2/blob/master/ros2.repos)), you already have this package and Cyclone DDS:

    cd /opt/ros/master
    rosdep install --from src -i
    colcon build
    export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp

## Quality Declaration

This package claims to be in the **Quality Level 2** category, see the [Quality Declaration](./rmw_cyclonedds_cpp/QUALITY_DECLARATION.md) for more details.
