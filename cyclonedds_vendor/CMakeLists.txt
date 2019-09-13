cmake_minimum_required(VERSION 3.5)
project(cyclonedds_vendor)
find_package(ament_cmake REQUIRED)

include(ExternalProject)
ExternalProject_Add(cyclonedds
  GIT_REPOSITORY "https://github.com/eclipse-cyclonedds/cyclonedds.git"
  CMAKE_ARGS
   -DBUILD_CONFTOOL:BOOL=OFF
   -DBUILD_IDLC:BOOL=OFF
   -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
  )

ament_package()