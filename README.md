# A ROS2 RMW implementation for Eclipse Cyclone DDS

This is an extended proof-of-concept RMW implementation for
using [*ROS2*](https://index.ros.org/doc/ros2)
with [*Eclipse Cyclone DDS*](https://github.com/eclipse-cyclonedds/cyclonedds) as the underlying DDS
implementation.

Whatever ROS2 C++ test/demo code I could manage to run on macOS seems to work (that covers most of
the core features), and the ROS2 CLI (``ros2``) on Linux gives a reasonable indication that most of
introspection functions also work.  So basically, pretty much everything works.

There are a number of known limitations:

* Cyclone DDS does not yet implement DDS Security.  Consequently, there is no support for security
  in this RMW implementation either.

* Cyclone DDS does not allow creating a waitset or a guard condition outside a participant, and this
  forces the creation of an additional participant.  It can be fixed in the RMW layer, or it can be
  dealt with in Cyclone DDS, but the trouble with the latter is that there are solid reasons for not
  allowing it, even if it is easy to support it today.  (E.g., a remote procedure call interface
  ...)
    
* Cyclone DDS does not currently support multiple domains simultaneously, and so this RMW
  implementation ignores the domain_id parameter in create_node, instead creating all
  nodes/participants (including the special participant mentioned above) in the default domain,
  which can be controlled via CYCLONEDDS_URI.
    
* Deserialization only handles native format (it doesn't do any byte swapping).  This is pure
  laziness, adding it is trivial.
    
* Deserialization assumes the input is valid and will do terrible things if it isn't.  Again, pure
  laziness, it's just adding some bounds checks and other validation code.
    
* There are some "oddities" with the way service requests and replies are serialized and what it
  uses as a "GUID".  (It actually uses an almost-certainly-unique 64-bit number, the Cyclone DDS
  instance id, instead of a real GUID.)  I'm pretty sure the format is wildly different from that in
  other RMW implementations, and so services presumably will not function cross-implementation.
    
* The name mangling seems to be compatibl-ish with the FastRTPS implementation and in some cases
  using the ros2 CLI for querying the system works cross-implementation, but not always.  The one in
  this implementation is reverse-engineered, so trouble may be lurking somewhere.  As a related
  point: the "no_demangle" option is currently ignored ... it causes a compiler warning.
