^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package rmw_cyclonedds_cpp
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

0.5.1 (2020-03-12)

* Use a list instead of a set for node names list
* Update for changes on Cyclone DDS security branch
* Fix leak in client/service topic error handling
* Fix sertopic referencing
* Update usage of rmw_topic_endpoint_info_array (`#101 <https://github.com/ros2/rmw_cyclonedds/issues/101>`_)
* Correct std::hash return type sizes (`#102 <https://github.com/ros2/rmw_cyclonedds/issues/102>`_)
* Correct the coding style to pass CI test.
* Update for cyclonedds changes needed for ros1 bridge
* Fix MSBuild warnings C4146 and C4267
* Add #if version >= 0.8.2 to fix ros2 dashing builds
* Implementation for rmw_get_pub/sub_info_by_topic (`#97 <https://github.com/ros2/rmw_cyclonedds/issues/97>`_)
* Remove unused CMake extras (`#84 <https://github.com/ros2/rmw_cyclonedds/issues/84>`_)
* code style only: wrap after open parenthesis if not in one line (`#95 <https://github.com/ros2/rmw_cyclonedds/issues/95>`_)
* Support for deadline, lifespan and liveliness qos  (`#88 <https://github.com/ros2/rmw_cyclonedds/issues/88>`_)
* rmw_get_topic_endpoint_info doesn't exist on Dashing (`#91 <https://github.com/ros2/rmw_cyclonedds/issues/91>`_)
* dds_time_t instead of dds_duration_t for absolute time
* Stubs for rmw_get_publishers_info_by_topic and rmw_get_subscriptions_info_by_topic (`#81 <https://github.com/ros2/rmw_cyclonedds/issues/81>`_)
* Cache serialization info when CDRWriter is constructed (`#80 <https://github.com/ros2/rmw_cyclonedds/issues/80>`_)
* Mark code that should be unreachable (`#77 <https://github.com/ros2/rmw_cyclonedds/issues/77>`_)
* Clean up topic namespace prefixes (`#76 <https://github.com/ros2/rmw_cyclonedds/issues/76>`_)
* Serialize into initialized memory, not vector (`#75 <https://github.com/ros2/rmw_cyclonedds/issues/75>`_)
* Rework serialization (`#42 <https://github.com/ros2/rmw_cyclonedds/issues/42>`_)
* Use rcutils_get_env() instead of getenv() (`#71 <https://github.com/ros2/rmw_cyclonedds/issues/71>`_) (`#72 <https://github.com/ros2/rmw_cyclonedds/issues/72>`_)
* Contributors: Erik Boasson, Dan Rose, Ivan Santiago Paunovic, Dirk Thomas, Dennis Potman, Emerson Knapp, Michael Carroll 

0.4.4 (2019-11-19)
------------------
* Minor CMakeLists cleanup
* Contributors: Dan Rose

0.4.3 (2019-11-13)
------------------
* Address "Precondition not met" on rmw_create_node (`#65 <https://github.com/ros2/rmw_cyclonedds/issues/65>`_) (`#66 <https://github.com/ros2/rmw_cyclonedds/issues/66>`_)
* Fix dashing breakage (`#64 <https://github.com/ros2/rmw_cyclonedds/issues/64>`_)
* Support localhost-only communications (`#60 <https://github.com/ros2/rmw_cyclonedds/issues/60>`_)
* Contributors: eboasson 

0.4.2 (2019-11-01)
------------------
* Suppress a syntax error identified by cppcheck 1.89 (`#59 <https://github.com/ros2/rmw_cyclonedds/issues/59>`_)
  Signed-off-by: Scott K Logan <logans@cottsay.net>
* Make RMW version acceptable to MSVC (`#58 <https://github.com/ros2/rmw_cyclonedds/issues/58>`_)
  GCC and Clang support the ternary operator in macros, MSVC does not.
  Signed-off-by: Erik Boasson <eb@ilities.com>
* skip compilation of rmw_cyclonedds when cyclone dds is not found (`#56 <https://github.com/ros2/rmw_cyclonedds/issues/56>`_)
  * skip compilation of rmw_cyclonedds when cyclone dds is not found
  Signed-off-by: Karsten Knese <karsten@openrobotics.org>
  * proper case and company name
  Signed-off-by: Karsten Knese <karsten@openrobotics.org>
  * linters
  Signed-off-by: Karsten Knese <karsten@openrobotics.org>
  * change ADLINK to Eclipse
  Signed-off-by: Karsten Knese <karsten@openrobotics.org>
* remove executive flags from source code files
  Signed-off-by: Karsten Knese <karsten@openrobotics.org>
* Contributors: Karsten Knese, Scott K Logan, eboasson

0.4.1 (2019-10-24)
------------------
* rename return functions
* Solve the lint issue.
* Add already obsoleted loaned message interfaces
* zero copy api for cyclonedds
* Use right event info for RMW_EVENT_LIVELINESS_LOST
* unbreak Dashing build after `#50 <https://github.com/ros2/rmw_cyclonedds/issues/50>`_
* Add compilation guards for RMW compatibility
* update signature for added pub/sub options
* Remove dead string serialization code (`#41 <https://github.com/ros2/rmw_cyclonedds/issues/41>`_)
* Use RMW_RET_NODE_NAME_NON_EXISTENT only if defined
* Code improvements in ser/deser code wrt passing data size (`#39 <https://github.com/ros2/rmw_cyclonedds/issues/39>`_)
* Return NODE_NAME_NON_EXISTENT instead of ERROR.
* Address uncrustify linter violation
* Validation in deserializer (`#36 <https://github.com/ros2/rmw_cyclonedds/issues/36>`_)
* make cyclonedds vender package play nice with colcon (`#34 <https://github.com/ros2/rmw_cyclonedds/issues/34>`_)
* Address CMake and uncrustify linter violations
* Fix "type punning" warning in printing floats (`#33 <https://github.com/ros2/rmw_cyclonedds/issues/33>`_)
* Use rosdep (`#32 <https://github.com/ros2/rmw_cyclonedds/issues/32>`_)
* Implemented byte-swapping in deserializer (`#31 <https://github.com/ros2/rmw_cyclonedds/issues/31>`_)
* Optional reporting of late messages
* Multi-domain support
* Add support for printing messages to DDSI trace
* Contributors: Brian Marchi, Dan Rose, Erik Boasson, Karsten Knese, Scott K Logan, dennis-adlink, eboasson, evshary

0.4.0 (2019-08-29)
------------------
* Revert "Replace cyclonedds by CycloneDDS for colcon"
* Replace cyclonedds by CycloneDDS for colcon
* Use NO_KEY GUID variant if Cyclone DDS supports it
* Implement no_demangle in various get\_... functions
* Set encoding to CDR rather than parameterised-CDR
* Code formatting fix
* Implement rmw_take_event
* Use dummy guardcond to block on empty waitset
* Handle RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT
* Add wstring support
* Support creating a waitset without creating a node
* Uncrustify and fix issues reported by cpplint
* Fix retrieving client/server topic names
* Return error when querying a non-existent node
* Add get_client_names_and_types_by_node
* Start request sequence numbers at 1
* Create topics in the right node's participant
* Update get_actual_qos based on test results
* Return error for invalid name nodes
* Fix serialization of bool sequence/array
* Create one DDS publisher, subscriber per node
* Share built-in readers across nodes
* Don't retain all data in builtin-topics readers
* Initialize common ddsi_sertopic with memset
* Fix return of rmw_wait
* Replace __attribute_\_((unused)) with static_cast<void>
* Check for nullptr.
* Add rmw_subscription_get_actual_qos implementation
* Specialize deserializer for strings (`#3 <https://github.com/ros2/rmw_cyclonedds/issues/3>`_)
* Avoid triggering graph guard cond after destroying it (`#3 <https://github.com/ros2/rmw_cyclonedds/issues/3>`_)
* Make various introspection features work
* add get service_names_and_types
* add type names, some more introspection functions
* update to match ROS2 Dashing interface
* remove use of C99-style designated initializers
* add rmw_get_topic_names_and_types (untested)
* add server_is_available, count_matched functions
* add write/take of serialized messages
* update for fixes in Cyclone sertopic interface
* fix string serialization, vector deserialization
* remove compile error when gcc 7
* update to allow talker/listener demos to run
* update for Cyclone DDS changes and ROS2 changes
* replace FastCDR and serialise straight into a serdata to avoid an extra copy
* use dds conditions and waitsets
* use waitsets, readconditions, guardconditions for waiting
* fix extern "C" use upsetting gcc (and accepted by clang)
* initial commit
* Contributors: Erik Boasson, Hunter L. Allen, Juan Oxoby, Scott K Logan, YuSheng T
