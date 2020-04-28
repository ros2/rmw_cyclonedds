This document is a declaration of software quality for the `CycloneDDS` external dependency imported by the rmw\_cyclonedds package, based on the guidelines in [REP-2004](https://www.ros.org/reps/rep-2004.html).

# `CycloneDDS` Quality Declaration

The package `CycloneDDS` claims to be in the **Quality Level 4** category.

The `CycloneDDS` project meets a substantial number of the criteria stated for software quality.
The developers have taken many steps to ensure that `CycloneDDS` is a software product which can be relied upon, however there is a significant lack of documentation regarding the interpretation of versions, features, and testing.
Even minimal investments in these areas would contribute to the quality of the project, and could increase the level category for `CycloneDDS` in a subsequent review.

Below are the rationales, notes, and caveats for this claim, organized by each requirement listed in the [Package Requirements for Quality Level 4 in REP-2004](https://www.ros.org/reps/rep-2004.html).

## Version Policy [1]

### Version Scheme [1.i]

There is a three-part version attached to `CycloneDDS` releases.
The sources define the version parts as `MAJOR`.`MINOR`.`PATCH`.
The CMake sources configure the compatibility mode to [`SameMajorVersion`](https://cmake.org/cmake/help/latest/module/CMakePackageConfigHelpers.html#generating-a-package-version-file), indicating that breaking changes are accompanied by a change to the `MAJOR` version part.
There is no explicit policy regarding versioning in the documentation for `CycloneDDS`, however there is some information in the [Releases section](https://www.eclipse.org/projects/handbook/#release) of the Eclipse project handbook which discusses _Major_, _Minor_, and _Service_ release criteria.
It is possible that these release categories align align with changes to the three version parts, but this is not explicitly stated in either `CycloneDDS` documentation or in the Eclipse project handbook.

### Version Stability [1.ii]

The latest release of `CycloneDDS`, `0.5.1`, does not have a stable version, i.e. `>= 1.0.0`.

### Public API Declaration [1.iii]

The public API for `CycloneDDS` is made up of symbols defined in the header files it installs.
In the source repository, these header files reside with each of the modules that make up `CycloneDDS`.

### API Stability Policy [1.iv]

There is no published policy for API stability in `CycloneDDS`, however there is some information in the [Releases section](https://www.eclipse.org/projects/handbook/#release) of the Eclipse project handbook which states that releases which include breaking API changes are considered "Major".

### ABI Stability Policy [1.v]

There is no published policy for ABI stability in `CycloneDDS`.

### ABI and ABI Stability Within a Released ROS Distribution [1.vi]

As an external package, `CycloneDDS` is not released on the same cadence as ROS.
Changes to the branch of the `CycloneDDS` repository that is targeted by a given ROS release would be picked up by development builds of that ROS release.
There is no published policy for what criteria are required for changes that are accepted in those branches.

## Change Control Process [2]

### Change Requests [2.i]

The source code repository for `CycloneDDS` is hosted in GitHub, and the change process follows a typical GitHub pull request model.
There are several [team members](https://projects.eclipse.org/projects/iot.cyclonedds/who) with rights to merge the pull requests, and an election process exists for promoting new members of that team.

### Contributor Origin [2.ii]

Perspective contributors to `CycloneDDS` must accept the terms of the [Eclipse Contributor Agreement](https://github.com/eclipse-cyclonedds/cyclonedds/blob/master/CONTRIBUTING.md#eclipse-contributor-agreement).
This is confirmed by a signed-off-by line in the commits put forth for review, which is enforced by a GitHub plugin which blocks completion of the pull request until all contributions contain a sign-off.

### Peer Review Policy [2.iii]

There is no published policy for peer review of `CycloneDDS` contributions, but because all changes are made through a pull request, each requires an authorized committer to be merged.

### Continuous Integration [2.iv]

Incoming pull requests automatically trigger continuous integration testing and must run successfully to be merged.
Automated testing runs for:
- Ubuntu Xenial with gcc 8 and Clang 7.0.0
- macOS Mojave with Xcode 11.1
- macOS Sierra with Xcode 9
- Windows Server semi-annual release with Visual Studio 2017

## Documentation [3]

### Feature Documentation [3.i]

[Project documentation](https://github.com/eclipse-cyclonedds/cyclonedds/blob/master/docs/dev/modules.md#feature-discovery) states that the features available in the product are "largely dynamic" based on compile-time discovery.
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

A substantial amount of the tests found throughout the source tree appear to verify functionality of various features of `CycloneDDS`.
However the lack of a complete feature list (see section [3.i]) makes it difficult to analyze the breadth of the tests.

### Public API Testing [4.ii]

There are some tests throughout the `CycloneDDS` source tree which specifically target the public API, but there are no policies or mechanisms to discover and measure which tests target the public API or how much of the public API is covered by the existing tests.

### Coverage [4.iii]

There is no test coverage tracking in `CycloneDDS`.

### Performance [4.iv]

Though the `CycloneDDS` [documentation](https://github.com/eclipse-cyclonedds/cyclonedds#performance) discusses the product's performance and discusses what metrics describe it, very few tests seem to validate the given statistics or ensure that the performance does not regress.
One of the [example projects](https://github.com/eclipse-cyclonedds/cyclonedds/blob/15e68152c9d14105e87ab1afc7e5af9c9589f776/examples/throughput/readme.rst) can be used to measure the throughput of the product, but does not provide a mechanism for analyzing resource usage or latency.

### Linters and Static Analysis [4.v]

There is no explicit documentation about the use of sanitizers in `CycloneDDS`, but it appears that the `USE_SANITIZER` build option can be used to enable address and thread sanitizers.
In continuous integration, ASAN is enabled for some of the test matrix.

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

The `CycloneDDS` [repository documentation](https://github.com/eclipse-cyclonedds/cyclonedds/blob/master/README.md#building-eclipse-cyclone-dds) states that Linux, Mac and Windows 10 are supported.
Platform version requirements are not explicitly stated, nor are all of the minimum build tool versions.

## Security [7]

### Vulnerability Disclosure Policy [7.i]

The Eclipse Project Handbook states the project's [vulnerability disclosure policy](https://www.eclipse.org/projects/handbook/#vulnerability-disclosure) in detail.
