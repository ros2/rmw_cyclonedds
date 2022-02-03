This document is a declaration of software quality for the `rmw_cyclonedds_cpp` package, based on the guidelines in [REP-2004](https://www.ros.org/reps/rep-2004.html).

# `rmw_cyclonedds_cpp` Quality Declaration

The package `rmw_cyclonedds_cpp` claims to be in the **Quality Level 2** category.

Below are the rationales, notes, and caveats for this claim, organized by each requirement listed in the [Package Requirements for Quality Level 2 in REP-2004](https://www.ros.org/reps/rep-2004.html).

## Version Policy [1]

### Version Scheme [1.i]

`rmw_cyclonedds_cpp` uses `semver` according to the recommendation for ROS Core packages in the [ROS 2 Developer Guide](https://docs.ros.org/en/rolling/Contributing/Developer-Guide.html#versioning).

### Version Stability [1.ii]

`rmw_cyclonedds_cpp` is at a stable version, i.e. `>= 1.0.0`.
The current version can be found in its [package.xml](package.xml), and its change history can be found in its [CHANGELOG](CHANGELOG.rst).

### Public API Declaration [1.iii]

Public API for `rmw_cyclonedds_cpp` is declared through `rmw`.

### API Stability Within a Released ROS Distribution [1.iv]/[1.vi]

`rmw_cyclonedds_cpp` will not break public API within a released ROS distribution, i.e. no major releases once the ROS distribution is released.

### ABI Stability Within a Released ROS Distribution [1.v]/[1.vi]

`rmw_cyclonedds_cpp` contains C and C++ code and therefore must be concerned with ABI stability, and will maintain ABI stability within a ROS distribution.

## Change Control Process [2]

`rmw_cyclonedds_cpp` follows the recommended guidelines for ROS Core packages in the [ROS 2 Developer Guide](https://docs.ros.org/en/rolling/Contributing/Developer-Guide.html#quality-practices).

### Change Requests [2.i]

This package requires that all changes occur through a pull request.

### Contributor Origin [2.ii]

This package uses DCO as its confirmation of contributor origin policy.
More information can be found in [CONTRIBUTING](../CONTRIBUTING.md).

### Peer Review Policy [2.iii]

Following the recommended guidelines for ROS Core packages, all pull requests must have at least 1 peer review.

### Continuous Integration [2.iv]

All pull requests must pass CI on all [tier 1 platforms](https://www.ros.org/reps/rep-2000.html#support-tiers).

### Documentation Policy [2.v]

All pull requests must resolve related documentation changes before merging.

## Documentation [3]

### Feature Documentation [3.i]

`rmw_cyclonedds_cpp`'s features are documented through `rmw`.
It has a [feature list](https://docs.ros2.org/latest/api/rmw/) and each item in the list links to the corresponding feature documentation.
There is documentation for all of the features, and new features require documentation before being added.

### Public API Documentation [3.ii]

`rmw_cyclonedds_cpp`'s API is declared and documented by `rmw`.
It has embedded API documentation and it is generated using doxygen and is hosted [alongside the feature documentation](https://docs.ros2.org/latest/api/rmw/).
There is documentation for all of the public API, and new additions to the public API require documentation before being added.

### License [3.iii]

The license for `rmw_cyclonedds_cpp` is Apache 2.0, and a summary is in each source file, the type is declared in the [package.xml](package.xml) manifest file, and a full copy of the license is in the [LICENSE](../LICENSE) file.

There is an automated test which runs a linter that ensures each file has a license statement.

Most recent test results can be found [here](https://ci.ros2.org/view/nightly/job/nightly_linux_release/lastBuild/testReport/rmw_cyclonedds_cpp/copyright/)

### Copyright Statements [3.iv]

The copyright holders each provide a statement of copyright in each source code file in `rmw_cyclonedds_cpp`.

There is an automated test which runs a linter that ensures each file has at least one copyright statement.

The results of the test can be found [here](https://ci.ros2.org/view/nightly/job/nightly_linux_release/lastBuild/testReport/rmw_cyclonedds_cpp/copyright/).

## Testing [4]

### Feature Testing [4.i]

All of the `rmw_cyclonedds_cpp` public features are ROS middleware features.

Unit, integration, and system tests higher up in the stack, such as those found in [`test_rmw_implementation`](https://github.com/ros2/rmw_implementation/tree/master/test_rmw_implementation), [`test_rclcpp`](https://github.com/ros2/system_tests/tree/master/test_rclcpp), and [`test_communication`](https://github.com/ros2/system_tests/tree/master/test_communication) packages, provide feature coverage.
Nightly CI jobs in [`ci.ros2.org`](https://ci.ros2.org/) and [`build.ros2.org`](https://build.ros2.org/), where `rmw_cyclonedds_cpp` is the default `rmw` implementation, thoroughly exercise this middleware.

### Public API Testing [4.ii]

`rmw_cyclonedds_cpp` implements the ROS middleware public API.
Unit, integration, and system tests higher up in the stack, such as those found in [`test_rmw_implementation`](https://github.com/ros2/rmw_implementation/tree/master/test_rmw_implementation), [`test_rclcpp`](https://github.com/ros2/system_tests/tree/master/test_rclcpp), and [`test_communication`](https://github.com/ros2/system_tests/tree/master/test_communication) packages, ensure compliance with the ROS middleware API specification (see [`rmw`](https://github.com/ros2/rmw) package) and further extend coverage.
New additions or changes to this API require tests before being added.

### Coverage [4.iii]

`rmw_cyclonedds_cpp` follows the recommendations for ROS Core packages in the [ROS 2 Developer Guide](https://docs.ros.org/en/rolling/Contributing/Developer-Guide.html#code-coverage), and opts to use branch coverage instead of line coverage.

This includes:

- tracking and reporting line coverage statistics
- achieving and maintaining a reasonable branch line coverage (90-100%)
- no lines are manually skipped in coverage calculations

Changes are required to make a best effort to keep or increase coverage before being accepted, but decreases are allowed if properly justified and accepted by reviewers.

Current coverage statistics can be viewed [here](https://ci.ros2.org/job/ci_linux_coverage/lastSuccessfulBuild/cobertura/src_ros2_rmw_cyclonedds_rmw_cyclonedds_cpp_src/).
A summary of how these statistics are calculated can be found in the [ROS 2 Developer Guide](https://docs.ros.org/en/rolling/Contributing/Developer-Guide.html#note-on-coverage-runs).

This package claims to meet the coverage requirements for the current quality level, even though it doesn't have 95% line coverage.

### Performance [4.iv]

`rmw_cyclonedds_cpp` does not currently have performance tests.

### Linters and Static Analysis [4.v]

`rmw_cyclonedds_cpp` uses and passes all the standard linters and static analysis tools for a C++ package as described in the [ROS 2 Developer Guide](https://docs.ros.org/en/rolling/Contributing/Developer-Guide.html#linters-and-static-analysis).

Results of the nightly linter tests can be found [here](https://ci.ros2.org/view/nightly/job/nightly_linux_release/lastBuild/testReport/rmw_cyclonedds_cpp).

## Dependencies [5]

### Direct Runtime ROS Dependencies [5.i]/[5.ii]

`rmw_cyclonedds_cpp` has the following runtime ROS dependencies:
* `rcpputils`: [QUALITY DECLARATION](https://github.com/ros2/rcpputils/blob/master/QUALITY_DECLARATION.md)
* `rcutils`: [QUALITY DECLARATION](https://github.com/ros2/rcutils/blob/master/QUALITY_DECLARATION.md)
* `rmw`: [QUALITY DECLARATION](https://github.com/ros2/rmw/blob/master/rmw/QUALITY_DECLARATION.md)
* `rmw_dds_common`: [QUALITY DECLARATION](https://github.com/ros2/rmw_dds_common/blob/master/rmw_dds_common/QUALITY_DECLARATION.md)
* `rosidl_runtime_c`: [QUALITY DECLARATION](https://github.com/ros2/rosidl/blob/master/rosidl_runtime_c/QUALITY_DECLARATION.md)
* `rosidl_runtime_cpp`: [QUALITY DECLARATION](https://github.com/ros2/rosidl/blob/master/rosidl_runtime_cpp/QUALITY_DECLARATION.md)
* `rosidl_typesupport_introspection_c`: [QUALITY DECLARATION](https://github.com/ros2/rosidl/blob/master/rosidl_typesupport_introspection_c/QUALITY_DECLARATION.md)
* `rosidl_typesupport_introspection_cpp`: [QUALITY DECLARATION](https://github.com/ros2/rosidl/blob/master/rosidl_typesupport_introspection_cpp/QUALITY_DECLARATION.md)
* `tracetools`: [QUALITY DECLARATION](https://gitlab.com/ros-tracing/ros2_tracing/-/blob/master/tracetools/QUALITY_DECLARATION.md)

It has several "buildtool" dependencies, which do not affect the resulting quality of the package, because they do not contribute to the public library API.
It also has several test dependencies, which do not affect the resulting quality of the package, because they are only used to build and run the test code.

### Direct Runtime Non-ROS Dependencies [5.iii]

`rmw_cyclonedds_cpp` has the following runtime non-ROS dependencies:
* `cyclonedds`: Eclipse Cyclone DDS claims to be Quality Level 2. For more information, please refer to its [quality declaration document](https://github.com/eclipse-cyclonedds/cyclonedds/blob/releases/0.8.x/CYCLONEDDS_QUALITY_DECLARATION.md).

## Platform Support [6]

`rmw_cyclonedds_cpp` supports all of the tier 1 platforms as described in [REP-2000](https://www.ros.org/reps/rep-2000.html#support-tiers), and tests each change against all of them.

Currently nightly results can be seen here:
* [linux-aarch64_release](https://ci.ros2.org/view/nightly/job/nightly_linux-aarch64_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [linux_release](https://ci.ros2.org/view/nightly/job/nightly_linux_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [mac_osx_release](https://ci.ros2.org/view/nightly/job/nightly_osx_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [windows_release](https://ci.ros2.org/view/nightly/job/nightly_win_rel/lastBuild/testReport/rmw_cyclonedds_cpp/)

## Security [7]

### Vulnerability Disclosure Policy [7.i]

This package conforms to the Vulnerability Disclosure Policy in [REP-2006](https://www.ros.org/reps/rep-2006.html).

# Current status Summary

The chart below compares the requirements in the REP-2004 with the current state of the `rmw_cyclonedds_cpp` package.

|Number| Requirement| Current state |
|--|--|--|
|1| **Version policy** |---|
|1.i|Version Policy available | ✓ |
|1.ii|Stable version | ✓ |
|1.iii|Declared public API| ✓ |
|1.iv|API stability policy| ✓ |
|1.v|ABI stability policy| ✓ |
|1.vi_|API/ABI stable within ros distribution| ✓ |
|2| **Change control process** |---|
|2.i| All changes occur on change request | ✓ |
|2.ii| Contributor origin (DCO, CLA, etc) | ✓ |
|2.iii| Peer review policy | ✓ |
|2.iv| CI policy for change requests | ✓ |
|2.v| Documentation policy for change requests | ✓ |
|3| **Documentation** | --- |
|3.i| Per feature documentation | ✓ |
|3.ii| Per public API item documentation | ✓ |
|3.iii| Declared License(s) | ✓ |
|3.iv| Copyright in source files| ✓ |
|3.v.a| Quality declaration linked to README | ✓ |
|3.v.b| Centralized declaration available for peer review | ✓ |
|4| **Testing** | --- |
|4.i| Feature items tests | ✓ |
|4.ii| Public API tests | ✓ |
|4.iii.a| Using coverage | ✓ |
|4.iii.a| Coverage policy | ✓ |
|4.iv.a| Performance tests (if applicable) | ☓ |
|4.iv.b| Performance tests policy| ✓ |
|4.v.a| Code style enforcement (linters)| ✓ |
|4.v.b| Use of static analysis tools | ✓ |
|5| **Dependencies** | --- |
|5.i| Must not have ROS lower level dependencies | ✓ |
|5.ii| Optional ROS lower level dependencies| ✓ |
|5.iii| Justifies quality use of non-ROS dependencies | ✓ |
|6| **Platform support** | --- |
|6.i| Support targets Tier1 ROS platforms| ✓ |
|7| **Security** | --- |
|7.i| Vulnerability Disclosure Policy | ✓ |
