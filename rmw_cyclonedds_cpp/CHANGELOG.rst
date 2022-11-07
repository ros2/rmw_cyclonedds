^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package rmw_cyclonedds_cpp
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

1.3.4 (2022-11-07)
------------------
* Export CycloneDDS dependency (`#424 <https://github.com/ros2/rmw_cyclonedds/issues/424>`_) (`#425 <https://github.com/ros2/rmw_cyclonedds/issues/425>`_)
* Merge pull request `#420 <https://github.com/ros2/rmw_cyclonedds/issues/420>`_ from ros2/mergify/bp/humble/pr-410
* Makes topic_name a const ref
* Adds topic name to error msg when create_topic fails
* Improve error message when create_topic fails (`#405 <https://github.com/ros2/rmw_cyclonedds/issues/405>`_) (`#406 <https://github.com/ros2/rmw_cyclonedds/issues/406>`_)
* [Fix] Add cstring header for memset (`#398 <https://github.com/ros2/rmw_cyclonedds/issues/398>`_)
* Contributors: Homalozoa X, Tully Foote, Voldivh, mergify[bot]

1.3.3 (2022-04-06)
------------------
* Fix get_topic_name and handling long service names
* Contributors: Erik Boasson

1.3.2 (2022-04-05)
------------------
* Add serialization for SDK_DATA
* Additional checks for loan API
* Contributors: Dietrich Krönke

1.3.1 (2022-03-31)
------------------
* Depend on just rmw_dds_common::rmw_dds_common_library (`#385 <https://github.com/ros2/rmw_cyclonedds/issues/385>`_)
* Contributors: Shane Loretz

1.3.0 (2022-03-25)
------------------
* Fix error message in rmw_init_options_copy(). (`#380 <https://github.com/ros2/rmw_cyclonedds/issues/380>`_)
* Add content filter topic feature empty stub. (`#289 <https://github.com/ros2/rmw_cyclonedds/issues/289>`_)
* Update to work with Cyclone 0.9.0 and Iceoryx 2.0 (`#379 <https://github.com/ros2/rmw_cyclonedds/issues/379>`_)
* Fill message info sequence numbers as unsupported, add rmw_feature_supported() implementation. (`#381 <https://github.com/ros2/rmw_cyclonedds/issues/381>`_)
* Contributors: Chen Lihui, Haowei Wen, Ivan Santiago Paunovic, Sumanth Nirmal

1.2.0 (2022-03-01)
------------------
* Fix a warning by making a pointer nullptr. (`#375 <https://github.com/ros2/rmw_cyclonedds/issues/375>`_)
* Bump QDs to QL2 (`#371 <https://github.com/ros2/rmw_cyclonedds/issues/371>`_)
* Add EventsExecutor (`#256 <https://github.com/ros2/rmw_cyclonedds/issues/256>`_)
* Call dissociate_reader in rmw_destroy_subscription
* Wrap creation of new serdata_rmw within a try-catch block
* Fix memory leak in error scenario on the publish side with SHM
* Fix memory leaks on the take side with SHM
* rename _cyclonedds_has_shm to follow the convention
* Add iceoryx_binding_c as dependency to rmw_cyclonedds_cpp
* Release iox_chunk to iceoryx in serdata_free if the iox_chunk is still available
* Update iceoryx_subscriber also when constructing the serdata from the iox chunk
* Contributors: Chris Lalancette, Christophe Bedard, Erik Boasson, Sumanth Nirmal, iRobot ROS

1.1.2 (2022-01-14)
------------------
* Fix cpplint errors (`#363 <https://github.com/ros2/rmw_cyclonedds/issues/363>`_)
* Contributors: Jacob Perron

1.1.1 (2021-12-17)
------------------
* Updates for uncrustify 0.72 (`#358 <https://github.com/ros2/rmw_cyclonedds/issues/358>`_)
* Export only rmw::rmw to downstream targets (`#360 <https://github.com/ros2/rmw_cyclonedds/issues/360>`_)
* Export modern CMake targets (`#357 <https://github.com/ros2/rmw_cyclonedds/issues/357>`_)
* Free with the same allocator in rmw_destroy_node (`#355 <https://github.com/ros2/rmw_cyclonedds/issues/355>`_)
* Contributors: Chris Lalancette, Jacob Perron, Shane Loretz

1.1.0 (2021-11-19)
------------------
* Add client/service QoS getters. (`#343 <https://github.com/ros2/rmw_cyclonedds/issues/343>`_)
* Updated version number and quality level. (`#349 <https://github.com/ros2/rmw_cyclonedds/issues/349>`_)
* Update package maintainers. (`#351 <https://github.com/ros2/rmw_cyclonedds/issues/351>`_)
* Contributors: Joe Speed, Michel Hidalgo, mauropasse

1.0.0 (2021-10-25)
------------------
* Fix undesired memory initialization in zero-copy data path. (`#348 <https://github.com/ros2/rmw_cyclonedds/issues/348>`_)
* Fix QoS depth settings for clients/service being ignored. (`#340 <https://github.com/ros2/rmw_cyclonedds/issues/340>`_)
* Link to Cyclone DDS in Quality Declaration. (`#342 <https://github.com/ros2/rmw_cyclonedds/issues/342>`_)
* Contributors: Chen Lihui, Erik Boasson, Joe Speed, Sumanth Nirmal

0.24.0 (2021-09-15)
-------------------
* Update rmw_context_impl_t definition (`#337 <https://github.com/ros2/rmw_cyclonedds/issues/337>`_)
* Add quality declaration for rmw_cyclonedds_cpp (`#335 <https://github.com/ros2/rmw_cyclonedds/issues/335>`_)
* Fix use of deprecated is_loan_available (`#336 <https://github.com/ros2/rmw_cyclonedds/issues/336>`_)
* Add -latomic for RISC-V (`#332 <https://github.com/ros2/rmw_cyclonedds/issues/332>`_)
* Add pub/sub init, publish and take instrumentation using tracetools (`#329 <https://github.com/ros2/rmw_cyclonedds/issues/329>`_)
* Pass the CRL down to CycloneDDS if it exists (`#325 <https://github.com/ros2/rmw_cyclonedds/issues/325>`_)
* Use the new rmw_dds_common::get_security_files API (`#323 <https://github.com/ros2/rmw_cyclonedds/issues/323>`_)
* Contributors: Chris Lalancette, Christophe Bedard, Michel Hidalgo, eboasson, guillaume-pais-siemens

0.23.1 (2021-06-16)
-------------------
* Add rmw_publisher_wait_for_all_acked support. (`#294 <https://github.com/ros2/rmw_cyclonedds/issues/294>`_)
* Contributors: Barry Xu

0.23.0 (2021-06-07)
-------------------
* Fix zero copy issues. (`#309 <https://github.com/ros2/rmw_cyclonedds/issues/309>`_)
* Handle allocation errors during message deserialization. (`#313 <https://github.com/ros2/rmw_cyclonedds/issues/313>`_)
* Update includes after rcutils/get_env.h deprecation. (`#312 <https://github.com/ros2/rmw_cyclonedds/issues/312>`_)
* Contributors: Christophe Bedard, Michel Hidalgo, Sumanth Nirmal

0.22.2 (2021-04-26)
-------------------
* Fix the history depth for KEEP_ALL. (`#305 <https://github.com/ros2/rmw_cyclonedds/issues/305>`_)
* Contributors: Chris Lalancette

0.22.1 (2021-04-12)
-------------------
* Use the macros from Cyclone DDS to work with sample payload when using SHM (`#300 <https://github.com/ros2/rmw_cyclonedds/issues/300>`_)
* Contributors: Sumanth Nirmal

0.22.0 (2021-04-07)
-------------------
* Add loaned sample zero-copy API support (`#297 <https://github.com/ros2/rmw_cyclonedds/issues/297>`_)
* Contributors: Sumanth Nirmal

0.21.0 (2021-04-06)
-------------------
* Indicate missing support for unique network flows (`#282 <https://github.com/ros2/rmw_cyclonedds/issues/282>`_)
* Contributors: Ananya Muddukrishna

0.20.0 (2021-03-18)
-------------------
* Take and return new RMW_DURATION_INFINITE correctly (`#288 <https://github.com/ros2/rmw_cyclonedds/issues/288>`_)
* Contributors: Emerson Knapp

0.19.0 (2021-03-01)
-------------------
* Add RMW function to check QoS compatibility (`#286 <https://github.com/ros2/rmw_cyclonedds/issues/286>`_)
* Contributors: Jacob Perron

0.18.4 (2021-01-25)
-------------------
* Fix use-after-free in error handling bug
* Drop compatibility with ancient cyclone versions
* Update to use Cyclone's renamed ddsi_sertype
* Use init-on-first-use for global state (`#275 <https://github.com/ros2/rmw_cyclonedds/issues/275>`_)
* Make sure to reset the error when a typesupport can't be found.
* Switch to using the generic functions for the typesupport handles.
* Handle typesupport errors on fetch. (`#271 <https://github.com/ros2/rmw_cyclonedds/issues/271>`_)
* Handle potential divide by 0 (`#267 <https://github.com/ros2/rmw_cyclonedds/issues/267>`_)
* Fix incorrect log message(rmw_fastrtps_shared_cpp -> rmw_cylonedds_cpp) (`#260 <https://github.com/ros2/rmw_cyclonedds/issues/260>`_)
* Update maintainers (`#254 <https://github.com/ros2/rmw_cyclonedds/issues/254>`_)
* Change wrong use of %ld to print std::size_t to %zu
* Contributors: Chris Lalancette, Erik Boasson, Ivan Santiago Paunovic, Michel Hidalgo, Stephen Brawner, Sven Brinkmann, eboasson, pluris

0.18.3 (2020-09-29)
-------------------
* Return RMW_RET_UNSUPPORTED in rmw_get_serialized_message_size (`#250 <https://github.com/ros2/rmw_cyclonedds/issues/250>`_)
* Update service/client request/response API error returns (`#249 <https://github.com/ros2/rmw_cyclonedds/issues/249>`_)
* Contributors: Alejandro Hernández Cordero, Jose Tomas Lorente

0.18.2 (2020-09-25)
-------------------
* Updated publisher/subscription allocation and wait set API return codes (`#246 <https://github.com/ros2/rmw_cyclonedds/issues/246>`_)
* Contributors: Alejandro Hernández Cordero

0.18.1 (2020-09-24)
-------------------
* Fix array `get_function` semantics (`#248 <https://github.com/ros2/rmw_cyclonedds/issues/248>`_)
* Update service/client construction/destruction API return codes. (`#247 <https://github.com/ros2/rmw_cyclonedds/issues/247>`_)
* Contributors: Ivan Santiago Paunovic, Michel Hidalgo

0.18.0 (2020-09-23)
-------------------
* Update gid API return codes. (`#244 <https://github.com/ros2/rmw_cyclonedds/issues/244>`_)
* Update graph API return codes. (`#243 <https://github.com/ros2/rmw_cyclonedds/issues/243>`_)
* Check for message_info on take where appropriate. (`#245 <https://github.com/ros2/rmw_cyclonedds/issues/245>`_)
  Fix for regression introduced in `#241 <https://github.com/ros2/rmw_cyclonedds/issues/241>`_.
* Contributors: Michel Hidalgo

0.17.0 (2020-09-18)
-------------------
* Updated error returns on rmw_take_serialized() and rmw_take_with_message_info() (`#242 <https://github.com/ros2/rmw_cyclonedds/issues/242>`_)
* Updated error returns on rmw_take() (`#241 <https://github.com/ros2/rmw_cyclonedds/issues/241>`_)
* Add quality declaration for Cyclone DDS (`#218 <https://github.com/ros2/rmw_cyclonedds/issues/218>`_)
* Contributors: Erik Boasson, Joe Speed, Jose Tomas Lorente, Scott K Logan 

0.16.0 (2020-09-14)
-------------------
* Fix that not to delete some objects after destroying functions (`#236 <https://github.com/ros2/rmw_cyclonedds/issues/236>`_)
* Update rmw_publish_serialized_message() error returns (`#240 <https://github.com/ros2/rmw_cyclonedds/issues/240>`_)
* Update rmw_publish() error returns (`#239 <https://github.com/ros2/rmw_cyclonedds/issues/239>`_)
* Remove public declarations (`#230 <https://github.com/ros2/rmw_cyclonedds/issues/230>`_)
* Use quotes for non-system includes (`#231 <https://github.com/ros2/rmw_cyclonedds/issues/231>`_)
* Use correct functions to resize and get an item, avoiding memory leaks in typesupport code (`#228 <https://github.com/ros2/rmw_cyclonedds/issues/228>`_)
* Contributors: Chen Lihui, Dan Rose, Lobotuerk

0.15.0 (2020-08-28)
-------------------
* Fix context cleanup. (`#227 <https://github.com/ros2/rmw_cyclonedds/issues/227>`_)
* Fix memory leak that type support not deleted. (`#225 <https://github.com/ros2/rmw_cyclonedds/issues/225>`_)
* Ensure compliant matched pub/sub count API. (`#223 <https://github.com/ros2/rmw_cyclonedds/issues/223>`_)
* Fix memory leak that string not deleted. (`#224 <https://github.com/ros2/rmw_cyclonedds/issues/224>`_)
* Change RET_WRONG_IMPLID() to return RMW_RET_INCORRECT_IMPLEMENTATION (`#226 <https://github.com/ros2/rmw_cyclonedds/issues/226>`_)
* Fix bad conditional in rmw_serialize(). (`#217 <https://github.com/ros2/rmw_cyclonedds/issues/217>`_)
* Contributors: Chen Lihui, Michel Hidalgo

0.14.0 (2020-08-06)
-------------------
* Ensure compliant subscription API. (`#214 <https://github.com/ros2/rmw_cyclonedds/issues/214>`_)
* Contributors: Michel Hidalgo

0.13.0 (2020-07-30)
-------------------
* Ensure compliant publisher API (`#210 <https://github.com/ros2/rmw_cyclonedds/issues/210>`_)
* rmw_destroy_node must remove node from graph cache (`#213 <https://github.com/ros2/rmw_cyclonedds/issues/213>`_)
* Add space between 'ROS' and '2' (`#195 <https://github.com/ros2/rmw_cyclonedds/issues/195>`_)
* Contributors: Christophe Bedard, Erik Boasson, Michel Hidalgo

0.12.0 (2020-07-22)
-------------------
* Set context actual domain id (`#208 <https://github.com/ros2/rmw_cyclonedds/issues/208>`_)
* Contributors: Ivan Santiago Paunovic

0.11.0 (2020-07-20)
-------------------
* Ensure compliant node construction/destruction API (`#206 <https://github.com/ros2/rmw_cyclonedds/issues/206>`_)
* Contributors: Michel Hidalgo

0.10.0 (2020-07-08)
-------------------
* Remove domain_id and localhost_only from node API (`#205 <https://github.com/ros2/rmw_cyclonedds/issues/205>`_)
* Amend rmw_init() implementation: require enclave. (`#204 <https://github.com/ros2/rmw_cyclonedds/issues/204>`_)
* Contributors: Ivan Santiago Paunovic, Michel Hidalgo

0.9.0 (2020-06-29)
------------------
* Ensure compliant init/shutdown API implementations. (`#202 <https://github.com/ros2/rmw_cyclonedds/issues/202>`_)
* Ensure compliant init options API implementations. (`#200 <https://github.com/ros2/rmw_cyclonedds/issues/200>`_)
* Finalize context iff shutdown. (`#196 <https://github.com/ros2/rmw_cyclonedds/issues/196>`_)
* Contributors: Michel Hidalgo

0.8.1 (2020-06-22)
------------------
* Handle RMW_DEFAULT_DOMAIN_ID. (`#194 <https://github.com/ros2/rmw_cyclonedds/issues/194>`_)
* Contributors: Michel Hidalgo

0.8.0 (2020-06-18)
------------------
* Add support to message lost event (`#192 <https://github.com/ros2/rmw_cyclonedds/issues/192>`_)
* Mitigate lost service responses discovery issue (`#187 <https://github.com/ros2/rmw_cyclonedds/issues/187>`_)
* Contributors: Ivan Santiago Paunovic, eboasson

0.7.1 (2020-06-02)
------------------
* Restore dashing/eloquent behaviour of "service_is_available" (`#190 <https://github.com/ros2/rmw_cyclonedds/issues/190>`_)
* Contributors: Erik Boasson

0.7.0 (2020-05-12)
------------------
* Remove API related to manual by node liveliness. (`#178 <https://github.com/ros2/rmw_cyclonedds/issues/178>`_)
* Contributors: Ivan Santiago Paunovic

0.6.0 (2020-05-04)
------------------
* Fix how topic name should be when not using ros topic name conventions (`#177 <https://github.com/ros2/rmw_cyclonedds/issues/177>`_)
* Initialize participant on first use and destroy participant after last node is destroyed (`#176 <https://github.com/ros2/rmw_cyclonedds/issues/176>`_)
* Fix error message (`#175 <https://github.com/ros2/rmw_cyclonedds/issues/175>`_)
  Only generate "Recompile with '-DENABLESECURITY=ON' error when
  ROS_SECURITY_STRATEGY="Enforce"
* Cast size_t to uint32_t explicitly (`#171 <https://github.com/ros2/rmw_cyclonedds/issues/171>`_)
* Rename rosidl_message_bounds_t (`#166 <https://github.com/ros2/rmw_cyclonedds/issues/166>`_)
* Add support for taking a sequence of messages (`#148 <https://github.com/ros2/rmw_cyclonedds/issues/148>`_)
* Implement with_info version of take (`#161 <https://github.com/ros2/rmw_cyclonedds/issues/161>`_)
* Fill in message_info timestamps (`#163 <https://github.com/ros2/rmw_cyclonedds/issues/163>`_)
* Fix build warnings (`#162 <https://github.com/ros2/rmw_cyclonedds/issues/162>`_)
* Switch to one participant per context model (`#145 <https://github.com/ros2/rmw_cyclonedds/issues/145>`_)
* Fix serialization on non-32-bit, big-endian systems (`#159 <https://github.com/ros2/rmw_cyclonedds/issues/159>`_)
* Correct fallthrough macro (`#154 <https://github.com/ros2/rmw_cyclonedds/issues/154>`_)
* Register RMW output filters.
* Implement safer align\_ function (`#141 <https://github.com/ros2/rmw_cyclonedds/issues/141>`_)
* Make case fallthrough explicit (`#153 <https://github.com/ros2/rmw_cyclonedds/issues/153>`_)
* Implement rmw_set_log_severity (`#149 <https://github.com/ros2/rmw_cyclonedds/issues/149>`_)
* security-context -> enclave (`#146 <https://github.com/ros2/rmw_cyclonedds/issues/146>`_)
* Rename rosidl_generator_c namespace to rosidl_runtime_c (`#150 <https://github.com/ros2/rmw_cyclonedds/issues/150>`_)
* Added rosidl_runtime c and cpp dependencies (`#138 <https://github.com/ros2/rmw_cyclonedds/issues/138>`_)
* Remove cyclonedds_cmake_module (`#139 <https://github.com/ros2/rmw_cyclonedds/issues/139>`_)
* Enable use of DDS security (`#123 <https://github.com/ros2/rmw_cyclonedds/issues/123>`_)
* Clean up package xml dependencies (`#132 <https://github.com/ros2/rmw_cyclonedds/issues/132>`_)
* API changes to sync with one Participant per Context change in rmw_fastrtps (`#106 <https://github.com/ros2/rmw_cyclonedds/issues/106>`_)
* Support for ON_REQUESTED_INCOMPATIBLE_QOS and ON_OFFERED_INCOMPATIBLE_QOS events (`#125 <https://github.com/ros2/rmw_cyclonedds/issues/125>`_)
* Uncrustify (`#124 <https://github.com/ros2/rmw_cyclonedds/issues/124>`_)
* Prevent undefined behavior when serializing empty vector (`#122 <https://github.com/ros2/rmw_cyclonedds/issues/122>`_)
* Add rmw\_*_event_init() functions (`#115 <https://github.com/ros2/rmw_cyclonedds/issues/115>`_)
* Contributors: Alejandro Hernández Cordero, Dan Rose, Dirk Thomas, Erik Boasson, Ingo Lütkebohle, Ivan Santiago Paunovic, Karsten Knese, Miaofei Mei, Michael Carroll, Michel Hidalgo, Mikael Arguedas, Sid Faber, dodsonmg

0.5.1 (2020-03-12)
------------------
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
* Contributors: Erik Boasson

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
