This document is a declaration of software quality for the `CycloneDDS` external dependency imported by the rmw\_cyclonedds package, based on the guidelines in [REP-2004](https://www.ros.org/reps/rep-2004.html).

# `CycloneDDS` Quality Declaration

This quality declaration claims that `CycloneDDS` is in the **Quality Level 3** category.

[The Eclipse Foundation](https://eclipse.org)'s `CycloneDDS` project meets a substantial number of the criteria stated for software quality.
The developers have taken many steps to ensure that `CycloneDDS` is a software product which can be relied upon.

Below are the rationales, notes, and caveats for this claim, organized by each requirement listed in the [Package Requirements for Quality Level 4 in REP-2004](https://www.ros.org/reps/rep-2004.html).

## Version Policy [1]

### Version Scheme [1.i]

`CycloneDDS` versioning follows the following rules:

* MAJOR version shall be incremented when an incompatible API change is made
* MINOR version when functionality is added in a backwards compatible manner. MINOR is source compatible (we strive to maintain binary compatibility as well).
* PATCH version when backwards compatible bug fixes are made. PATCH is binary compatible.

Major version 0 is considered stable. `CycloneDDS` was already a stable code base when it was contributed to [The Eclipse Foundation](https://eclipse.org)

Additional labels for pre-release and build metadata are available as extensions to the MAJOR.MINOR.PATCH format.

Here is information in the [Releases section](https://www.eclipse.org/projects/handbook/#release) of the Eclipse project handbook which discusses _Major_, _Minor_, and _Service_ release criteria.

### Version Stability [1.ii]

`CycloneDDS` is at a stable version. The current version can be found in its [package.xml](https://github.com/eclipse-cyclonedds/cyclonedds/blob/master/package.xml), and its change history can be found in its [CHANGELOG](https://github.com/eclipse-cyclonedds/cyclonedds/blob/master/CHANGELOG.rst).

### Public API Declaration [1.iii]

Symbols starting with dds_ or DDS_ and that are accessible after including only the top-level "dds.h" file, unless explicitly stated otherwise in a documentation comment are considered part of the stable API. 
In the source repository, these header files reside with each of the modules that make up CycloneDDS.

### API Stability Policy [1.iv]

`CycloneDDS` provides API stability for PATCH release. `CycloneDDS` strives to provide API stability for MINOR release. `CycloneDDS` does guarantee API stability for MAJOR release.

### ABI Stability Policy [1.v]

`CycloneDDS` provides ABI stability for PATCH release. `CycloneDDS` strives to provide ABI stability for MINOR release. `CycloneDDS` does guarantee ABI stability for MAJOR release.

### ABI and ABI Stability Within a Released ROS Distribution [1.vi]

A ROS 2 release is pinned to a `CycloneDDS` release, e.g., 0.6.x or 0.7.x. 
The "x" here stands for patch releases and those are binary compatible. This does not preclude releasing 0.8.0 or 1.0.0.

## Change Control [2]

`CycloneDDS` follows the recommended guidelines of the [Eclipse Development Process](https://www.eclipse.org/projects/dev_process/).

For the RMW layer, we follow the ROS core packages process. 
For Eclipse Cyclone DDS, we ensure the stability within the project through review, CI and tests and additionally run ROS CI for changes that are likely to affect ROS.

All commits must be signed by the author and there must be an [Eclipse Contributor Agreement](https://www.eclipse.org/legal/ECA.php) on file with the Eclipse Foundation.

Pull requests are required to pass all tests in the CI system, unless Committers consider there is sufficient evidence that a failure is the result of a mishap unrelated to the change. 
Pull requests are only merged if the Committers deem it of acceptable quality and provide sufficient coverage of new functionality or proof of a bug fix via tests.

Only the Committers have write access to the repository and they are voted for in elections monitored by the Eclipse Foundation staff.

### Change Requests [2.i]

All changes will occur through a pull request, check ROS 2 Developer Guide for additional information.

### Contributor Origin [2.ii]

This package uses DCO as its confirmation of contributor origin policy. 
More information can be found in [Eclipse Foundation's DCO policy](https://www.eclipse.org/legal/DCO.php).

### Peer Review Policy [2.iii]

All pull requests will be peer-reviewed, check [Eclipse Developer Process](https://www.eclipse.org/projects/dev_process/) for additional information.

### Continuous Integration [2.iv]

All pull requests must pass CI on all tier 1 platforms
Currently nightly results can be seen here:
* [linux-aarch64_release](https://ci.ros2.org/view/nightly/job/nightly_linux-aarch64_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [linux_release](https://ci.ros2.org/view/nightly/job/nightly_linux_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [mac_osx_release](https://ci.ros2.org/view/nightly/job/nightly_osx_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [windows_release](https://ci.ros2.org/view/nightly/job/nightly_win_rel/lastBuild/testReport/rmw_cyclonedds_cpp/)

Incoming pull requests automatically trigger continuous integration testing and must run successfully to be merged.
Automated testing runs for:
- Ubuntu Xenial with gcc 8 and Clang 7.0.0
- macOS Mojave with Xcode 11.1
- macOS Sierra with Xcode 9
- Windows Server semi-annual release with Visual Studio 2017

### Documentation Policy [2.v]
All pull requests must resolve related documentation changes before merging.

## Documentation [3]

### Feature Documentation [3.i]

[Project documentation](https://github.com/eclipse-cyclonedds/cyclonedds/blob/master/docs/dev/modules.md#feature-discovery) states that the features available in the product are "largely dynamic" based on compile-time discovery.
`CycloneDDS` documentation covers all of the stable interface, and PRs that add to it need to include documentation. 
An explicit list of those features, and what criteria is required for discovery, is unavailable.

### Public API Documentation [3.ii]

The repository includes a [section](https://github.com/eclipse-cyclonedds/cyclonedds#documentation) discussing documentation which states that there is currently only limited documentation.
Generated documentation for the API is available in the [PDF document](https://raw.githubusercontent.com/eclipse-cyclonedds/cyclonedds/assets/pdf/CycloneDDS-0.1.0.pdf) linked in the project README.

### License [3.iii]

The license for `CycloneDDS` is the Eclipse Public License 2.0 and the Eclipse Distribution License 1.0, and all of the code includes a header stating so.
The project includes a [`NOTICE`](https://github.com/eclipse-cyclonedds/cyclonedds/blob/master/NOTICE.md#declared-project-licenses) with links to more information about these licenses.
The `CycloneDDS` repository also includes a [`LICENSE`](https://github.com/eclipse-cyclonedds/cyclonedds/blob/master/LICENSE) file with the full terms.

There is some third-party content included with `CycloneDDS` which is licensed as Zlib, New BSD, and Public Domain.
Details can also be found in the included [`NOTICE`](https://github.com/eclipse-cyclonedds/cyclonedds/blob/master/NOTICE.md#third-party-content) document.

### Copyright Statement [3.iv]

The `CycloneDDS` documentation includes a [policy](https://github.com/eclipse-cyclonedds/cyclonedds/blob/master/NOTICE.md#copyright) regarding content copyright, each of the source files containing code include a copyright statement with the license information in the file's header.

## Testing [4]

Some directories within the `CycloneDDS` source tree contain subdirectories for test code.
In all, the test code appears to comprise approximately 25% of the codebase.

### Feature Testing [4.i]

Each feature in `CycloneDDS` has corresponding tests which simulate typical usage, and they are located in separate directories next to the sources. 
New features are required to have tests before being added.
Currently nightly results can be seen here:
* [linux-aarch64_release](https://ci.ros2.org/view/nightly/job/nightly_linux-aarch64_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [linux_release](https://ci.ros2.org/view/nightly/job/nightly_linux_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [mac_osx_release](https://ci.ros2.org/view/nightly/job/nightly_osx_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [windows_release](https://ci.ros2.org/view/nightly/job/nightly_win_rel/lastBuild/testReport/rmw_cyclonedds_cpp/)

A substantial amount of the tests found throughout the source tree appear to verify functionality of various features of `CycloneDDS`.
However the lack of a complete feature list (see section [3.i]) makes it difficult to analyze the breadth of the tests.

### Public API Testing [4.ii]

Each part of the public API has tests, and new additions or changes to the public API require tests before being added. 
The tests aim to cover both typical usage and corner cases.
There are some tests throughout the `CycloneDDS` source tree which specifically target the public API, but there are no policies or mechanisms to discover and measure which tests target the public API or how much of the public API is covered by the existing tests.

### Coverage [4.iii]

`CycloneDDS` has automated [Codecov](https://codecov.io) test coverage which can be seen [here](https://codecov.io/github/eclipse-cyclonedds/cyclonedds?branch=master).
New changes are required to include tests coverage.


### Performance [4.iv]

While there are no automated, public tests or results (more due Open Robotics lacking reliable ros2 CI infrastructure than to policy), there is evidence in PRs that performance does get taken into account (see, e.g., eclipse-cyclonedds/cyclonedds#558). 
`ddsperf` is used to check for performance regressions regularly and before releases.
PRs are tested for regressions using ddsperf before changes are accepted.
[ddsperf](https://github.com/eclipse-cyclonedds/cyclonedds/tree/master/src/tools/ddsperf) is the tool to use for assessing `CycloneDDS` performance.

ros2 [nightly CI performance tests](http://build.ros2.org/job/Fci__nightly-performance_ubuntu_focal_amd64/ ) exist but is not reliable infrastructure. We suggest Open Robotics move all performance testing to dedicated hardware.

### Linters and Static Analysis [4.v]
`rmw_cyclonedds` uses and passes all the ROS2 standard linters and static analysis tools for a C++ package as described in the [ROS 2 Developer Guide](https://index.ros.org/doc/ros2/Contributing/Developer-Guide/#linters-and-static-analysis). Passing implies there are no linter/static errors when testing against CI of supported platforms.
Currently nightly results can be seen here:
* [linux-aarch64_release](https://ci.ros2.org/view/nightly/job/nightly_linux-aarch64_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [linux_release](https://ci.ros2.org/view/nightly/job/nightly_linux_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [mac_osx_release](https://ci.ros2.org/view/nightly/job/nightly_osx_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [windows_release](https://ci.ros2.org/view/nightly/job/nightly_win_rel/lastBuild/testReport/rmw_cyclonedds_cpp/)

`CycloneDDS` has automated daily [Synopsys Coverity static code analysis](https://www.synopsys.com/software-integrity/security-testing/static-analysis-sast.html) with public results that can be seen [here](https://scan.coverity.com/projects/eclipse-cyclonedds-cyclonedds). 

`CycloneDDS` defect density is 0.05 per 1,000 lines of code as of Aug 11th 2020. 
Omitting the Java idlc which is **not** used by ROS 2 gives 0.032 per 1,000 lines of code.

`cyclonedds-cxx` OMG DDS C++ API automated daily static code analysis shows defect density of 0.0 per 1,000 lines of code as of Aug 29th 2020 as you can see [here](https://scan.coverity.com/projects/eclipse-cyclonedds-cyclonedds-cxx)

`rmw_cyclonedds` automated daily static code analysis is being setup, results will be [here]

For comparison the average defect density of open source software projects of similar size is 0.5 per 1,000 lines of code.
In continuous integration, ASAN is enabled for some of the test matrix. 
The CI run includes address sanitizer runs, ergo, no PRs can be accepted that are not clean with respect to the address sanitizer.
There do not appear to be any linters enabled for the `CycloneDDS` repository.

## Dependencies [5]

### Direct Runtime ROS Dependencies [5.i]

As an external dependency, there are no ROS dependencies in `CycloneDDS`.

### Optional Direct Runtime ROS Dependencies [5.ii]

As an external dependency, there are no ROS dependencies in `CycloneDDS`.

### Direct Runtime non-ROS Dependency [5.iii]

The only runtime dependency of `CycloneDDS` is `OpenSSL`, a widely-used secure communications suite.
If `CycloneDDS` is built without security enabled, the product has no apparent runtime dependencies.

## Platform Support [6]

`CycloneDDS` supports all of the tier 1 platforms as described in REP-2000, and tests each change against all of them.

`CycloneDDS` platform testing CI results are here: https://travis-ci.com/github/eclipse-cyclonedds 

Open Robotics ROS 2 CI nightly results can be seen here:
* [linux-aarch64_release](https://ci.ros2.org/view/nightly/job/nightly_linux-aarch64_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [linux_release](https://ci.ros2.org/view/nightly/job/nightly_linux_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [mac_osx_release](https://ci.ros2.org/view/nightly/job/nightly_osx_release/lastBuild/testReport/rmw_cyclonedds_cpp/)
* [windows_release](https://ci.ros2.org/view/nightly/job/nightly_win_rel/lastBuild/testReport/rmw_cyclonedds_cpp/)

Regarding minimum versions they are basically are not known exactly because `CycloneDDS` builds and runs on everything we have tested including ancient versions of Linux and macOS. 
For evidence, the fact that `CycloneDDS` builds and runs on [Solaris 2.6 on SPARCv8](https://github.com/eclipse-cyclonedds/cyclonedds/tree/master/ports/solaris2.6) (given pre-generated header files and IDL output) is a fair indication of its broad support of platforms and old versions.
CMake 3.7, Java 1.8 and Maven 3.5 are new enough.

## Security [7]

### Vulnerability Disclosure Policy [7.i]

This package conforms to the Vulnerability Disclosure Policy in REP-2006. 
The Eclipse Project Handbook states the project's vulnerability disclosure policy in detail. 
The Eclipse Project Handbook states the project's [vulnerability disclosure policy](https://www.eclipse.org/projects/handbook/#vulnerability-disclosure) in detail.
