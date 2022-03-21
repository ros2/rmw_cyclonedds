# Using Shared Memory with ROS 2

This rmw_cyclonedds implementation uses [cyclonedds](https://projects.eclipse.org/projects/iot.cyclonedds) which includes support for fast Shared Memory data transfer based on [iceoryx](https://projects.eclipse.org/projects/technology.iceoryx). Shared Memory is disabled by default but can be enabled easily by providing a cyclonedds.xml configuration file.

## Requirements

Currently Shared Memory transport is only supported on Linux. It is available in the rmw_cyclonedds implementation used by the ROS 2 Rolling or Humble Hawksbill distribution.

Note that the Shared Memory feature is not available on Windows.

## Installation

ROS 2 needs to be installed as described in [Installing ROS 2 Rolling](https://docs.ros.org/en/rolling/Installation/Ubuntu-Install-Binary.html).
It can also be build from sources directly [Building ROS 2 Rolling](https://docs.ros.org/en/rolling/Installation/Ubuntu-Development-Setup.html). Using the latest ROS 2 installation [ROS 2 Humble](https://docs.ros.org/en/galactic/Releases.html) is also possible.

In both cases rmw_cyclonedds is built with Shared Memory support by default.

## Configuration

In your ROS 2 workspace `ros2_ws` create a configuration file `cyclonedds.xml` with the following content.

```xml
<?xml version="1.0" encoding="UTF-8" ?>
<CycloneDDS xmlns="https://cdds.io/config" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="https://cdds.io/config https://raw.githubusercontent.com/eclipse-cyclonedds/cyclonedds/iceoryx/etc/cyclonedds.xsd">
    <Domain id="any">
        <SharedMemory>
            <Enable>true</Enable>
            <LogLevel>info</LogLevel>
        </SharedMemory>
    </Domain>
</CycloneDDS>
```

Enter your ROS 2 workspace and enable this configuration.

```console
cd ros2_ws
export CYCLONEDDS_URI=file://$PWD/cyclonedds.xml
```

### Configuration file options

Enabling Shared Memory imposes additional restrictions on publishers and subscriptions in ROS applications if Shared Memory is used for communication. In this case the data is received using an internal queue (one queue per subscription).
Note that depending on the QoS settings Shared Memory might not be used (cf. [Restrictions](#Restrictions)).

The *Loglevel* controls the output of the iceoryx runtime and can be set to verbose, debug, info, warn, error, fatal and off in order of decreasing output level.

The settings in the configuration file above are also the default values when left unspecified and setting them any higher is currently not possible.
Note that further aligment with DDS QoS is work in progress. Currently these options exist alongside QoS settings as additional limits.

Further information about these options can be found in [manual of the cyclonedds project](https://github.com/eclipse-cyclonedds/cyclonedds/blob/iceoryx/docs/manual/options.md#cycloneddsdomainsharedmemory).

## Run an Example

We can now run the basic [talker/listener example](https://docs.ros.org/en/rolling/Installation/Ubuntu-Development-Setup.html#try-some-examples).
In one terminal start the talker

```console
export CYCLONEDDS_URI=file://$PWD/cyclonedds.xml
. ~/ros2_ws/install/local_setup.bash
RMW_IMPLEMENTATION=rmw_cyclonedds_cpp ros2 run demo_nodes_cpp talker
```

and in another terminal start the listener

```console
export CYCLONEDDS_URI=file://$PWD/cyclonedds.xml
. ~/ros2_ws/install/local_setup.bash
RMW_IMPLEMENTATION=rmw_cyclonedds_cpp ros2 run demo_nodes_cpp listener
```

Note that we have to specify the middleware implementation with `RMW_IMPLEMENTATION=rmw_cyclonedds_cpp`
since otherwise `rmw fastrtps cpp` is used by default.

If the Shared Memory configuration was successfully activated and recognized, you should get some output containing

```console
[Warning]: RouDi not found - waiting
```

in both terminals. This is because with Shared Memory enabled, the iceoryx middleware daemon RouDi (abbreviation for Routing and Discovery) needs to be present and was not started yet.
We can do so by simply running

```console
. ~/ros2_ws/install/local_setup.bash
iox-roudi
```

in a third terminal. Now the talker should start sending data to the listener.

While this example uses shared memory, it does not benefit from zero-copy since the message type contains a string, which is a dynamically sized data type (cf. [Types](#Types)).

Note that RouDi is still required whenever we activate a Shared Memory configuration by exporting the configuration file (it is needed by the underlying cyclonedds implementation in this case).
We could also run the listener or talker without exporting the configuration file and it would still receive data (via network interface). By running both without exporting the configuration file we will not need to have RouDi running anymore (this is the regular ROS 2 setup).

### Using Shared Memory in the example

To actually use Shared Memory the talker/listener example needs to be slightly rewritten to use a fixed size data type such as an unsigned integer. Adapting the publisher and subscription to use messages of type `std_msgs::msg::Uint32` instead leads to an example which uses Shared Memory to transport the data. See [Restrictions](#Restrictions) for further information about when Shared Memory transfer will be used.

In the talker we use

```cpp
    std::unique_ptr<std_msgs::msg::Uint32> msg_;
    rclcpp::Publisher<std_msgs::msg::UInt32>::SharedPtr pub_;
    rclcpp::TimerBase::SharedPtr timer_;
    // ...
    pub_ = this->create_publisher<std_msgs::msg::Uint32>("chatter", qos);
```

and send the data with

```cpp
    msg_ = std::make_unique<std_msgs::msg::Uint32>();
    msg_->data = count_++;
    RCLCPP_INFO(this->get_logger(), "Publishing: 'Hello World: %lu'", msg_->data);
    pub_->publish(std::move(msg_));
```

Similarly on the listener side we also change the message type of the subscription

```cpp
    rclcpp::Subscription<std_msgs::msg::Uint32>::SharedPtr sub_;
    // ...
    sub_ = create_subscription<std_msgs::msg::Uint32>("chatter", 10, callback);
```

and can then receive the data with

```cpp
auto callback =
      [this](const std::shared_ptr<std_msgs::msg::Uint32> msg) -> void
      {
        RCLCPP_INFO(this->get_logger(), "I heard: [Hello World: %lu]", msg->data);
      };
```

Note that by just observing the received data it cannot be determined whether Shared Memory is actually used, but for large data the maximum data rate will be significantly faster compared to the network loopback interface.

## Zero-Copy Publish-Subscribe Communication

Whether true zero-copy publish subscribe communication via Shared Memory is possible depends on several factors. Here zero-copy means no copy or serialization is performed.

1. Shared Memory has to be enabled in the configuration.
2. The subscription has to be local on the same machine (i.e. same or different process, but no remote subscription).
3. The QoS settings admit Shared Memory transfer (cf. [QoS settings](#QoS-settings))

If these conditions are satisfied we can publish the data in two different ways, but only by using the loan API we will achieve zero-copy transfer.

### Regular Publish API

We first create a message, populate it before passing it to the publisher.

```cpp
    msg_ = std::make_unique<std_msgs::msg::Uint32>();
    msg_->data = count_++;      
    pub_->publish(std::move(msg_));
```

If the Shared Memory transfer conditions are met, publish will internally loan a Shared Memory chunk from iceoryx and copy (only for fixed size types) or serialize the message payload into the memory chunk (non-fixed size types). Any connected subscription has read-only access to this message data.

While this API will not allow true zero-copy transfer, it still will improve performance for sufficiently large message sizes since it bypasses the loopback interface and does not perform serialization. The actual size where it will outperform the loopback interface primarily depends on the actual hardware and system load.

### Loan API

The loan API allows for true zero-copy transfer by directly consctructing the message data in Shared Memory. To do so, we first have to loan this Shared Memory chunk explicitly.

```cpp
auto loaned_msg = pub_->borrow_loaned_message();
loaned_msg.get().data = count_++;
pub_->publish(std::move(loaned_msg));
```

The publish overload here does not need to copy the data to Shared Memory since it already resides there. Hence no copy is performed and the data is transferred to any subscription in constant time (i.e. independent of the message size). Depending on the data type there serialization into the loaned message may still be required and publish will therefore not be independent of message size. This is not the case for [fixed size](#Types) data types.

Note that to properly use loaning move semantics are essential, i.e. using `std::move` when publishing is required. This allows efficiently transferring ownership of the internal message data back to the middleware without copy overhead. As a consequence read access of `loaned_msg` after it was published is illegal (undefined behavior).

## Restrictions

The factors listed above govern whether Shared Memory is used, assuming it was enabled in the configuration. Here we summarize those restrictions and provide some additional details.

### Local Subscription

Only local subscriptions on the same machine will receive data via Shared Memory and RouDi is required to enable this. Remote subscriptions will always receive the data via the network interface. There can only be one Roudi process running per machine.

### Types

#### Zero-copy

To benefit from zero-copy transfer, the message types used must be of a fixed size, i.e. the size can be determined at compile time and does not refer to memory
outside of the message struct itself. This means strings or variable length arrays of the [available message types](http://wiki.ros.org/msg) cannot be used.
Nesting types satisfying the fixed size restriction is also possible.

#### Shared Memory Serialization

All non-fixed size types like strings will be serialized into shared memory. This incurs much more overhead for serialization and deserialization but still avoids some copies that would be performed if the network stack is used (i.e. compared to the loopback interface in the same process). The runtime/latency benefit in this case is much smaller compared to zero-copy for fixed size types.

### QoS settings

Only a subset of QoS settings supports Shared Memory. Those are:

1. Liveliness: Automatic
2. Deadline: Infinity (0)
3. Reliability: Reliable or Best Effort
4. Durability: Volatile or Transient Local
5. History: Keep Last

The Keep Last history depth of a writer cannot be larger than the maximum capacity allowed by an iceoryx publisher
(currently 16). Otherwise the network stack is implictly used.

The ROS 2 default settings Reliable, Volatile and Keep Last(10) are supported and applicable to a wide range of applications.

### Number of subscriptions per Process

Currently a process can only have up to 127 subscriptions when Shared Memory is enabled.

### Number of active Loans

A single publisher can only hold up to 8 loaned messages simultaneously.
To obtain more loaned messages, it needs to publish some of the loaned messages.

### Iceoryx Shared Memory Configuration

Iceoryx uses configurable memory pools to define different sizes of memory chunks that will be used to store messages in Shared Memory. These will be obtained when data is sent via iceoryx, either when explicitly requested with the [Loan API](#Loan-API) or implicitly by the [Regular Publish API](#Regular-Publish-API).

Depending on the size and frequency of messages send, the default configuration may not be sufficient to guarantee that memory can be loaned and hence the data sent. In this case it might help to use a custom configuration for the shared memory pools to increase the available Shared Memory. The configuration options are described in the [iceoryx configuration guide](https://github.com/eclipse-iceoryx/iceoryx/blob/master/doc/website/advanced/configuration-guide.md).

Note that currently the internal loan call is blocking, which means if no memory is available it will not return (this will change in the future). This may happen if the configured memory is not sufficient for the overall system load, i.e. the memory needed was not available in the first place or is used by other samples which are currently read or written.

## Verifing Shared Memory Usage

It is currently not possible to accurately check whether Shared Memory transfer actually takes place for a specific subscription. If the conditions in [Restrictions](#Restrictions) are met this should be the case. If the data rate or latency are beyond what is achievable using regular network communication this is an indication that at least partially Shared Memory communication via iceoryx is being used.

Another way to check whether Shared Memory is used is running the iceoryx introspection client, which allows tracking of various statistics of the iceoryx communication, Shared Memory utilization being one of them.

### Building the iceoryx introspection

The introspection client is not build by default, so we need to do so manually.
The introspection depends on iceoryx_utils and iceoryx_posh and we will build against the libraries already build by the ROS 2 installation.
After installing ROS 2 as described in [Installation](#Installation) navigate to the ROS 2 workspace and execute

```console
cd ros2_ws
. ~/ros2_ws/install/setup.bash
```

In the introspection folder of the iceoryx repository (part of the ROS 2 installation) run cmake followed by make to build the introspection.

```console
cd src/eclipse-iceoryx/iceoryx/tools/introspection
cmake -Bbuild
cd build
make
```

Afterwards the executable *iox-introspection-client* should appear in the build folder.

### Using the iceoryx introspection

The introspection requires RouDi to be running since it receives the statistics information directly from the RouDi middleware daemon by subscribing to built-in topics.

Start RouDi and any applications of your system, e.g. [talker and listener](#Using-Shared-Memory-in-the-example).

The introspection is able to track the processes using iceoryx, the subscriptions using Shared Memory (those using network cannot be found here) and Shared Memory utilization data (i.e. the number and size of allocations in Shared Memory).

Executing

```console
./iox-introspection-client --h
```

in the folder where the introspection was build provides us with a list of the various options. In the following we will display all statistics by running

```console
./iox-introspection-client --all
```

To verify that a particular connection is using Shared Memory we can proceed as follows. In the particular talker and listener example the internal ID of the talker and lister process should appear in the *Processes* section of the introspection. These IDs are unique but unfortunately cannot be easily traced back to the process.

From the *Connections* section we can infer whether a specific topic is offered and whether a subscription to this topic exists. Note that AUTOSAR terminology is used for the displayed data but the topic information is available in the Instance and Event columns. Also note that the built-in topics used by the introspection are also listed.

If a publisher which uses Shared Memory publishes data, the memory utilization changes. This is visible in the *MemPool Status* section in the part listed in Segment Id: 1. If Shared Memory is used, the number of chunks in use should go up until a specific saturation point where it will stagnate or start to fluctuate slightly as chunks are freed by the suscribing party. This is related to the settings in the [configuration](#Configuration-file-options) as these essentially control how much Shared Memory a specific subscribtion can use at most.

If we can observe memory chunks being used this is a strong indication that data is transferred by Shared Memory. However, this does not rule out that part of the communication (e.g. other subscriptions) is using the regular network tranfer. This cannot be conclusive in general if multiple subscriptions exist since we do not know which of them transfer data via Shared Memory by observing this statistic in isolation.

The Min Free statistic is also important, as it counts the minimum number of chunks of a specific size that were free in a particular system execution (i.e. this can only decrease monotonically). If this is running low it indicates that there may be not enough Shared Memory available. This can be increased by using a different [Shared Memory configuration](#Iceoryx-Shared-Memory-Configuration).
