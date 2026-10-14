cmake_minimum_required(VERSION 3.24)
project(podlens-linux-host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release artifacts must not embed developer-machine library search paths;
# every linked library (GTK4, GIO, GStreamer) is a system dependency resolved
# by the normal loader search on the target machine.
set(CMAKE_SKIP_RPATH TRUE)

# RIVET_ROOT points at the installed Rivet package. `raco rivet build` sets
# it; manual builds may export it themselves.
if(NOT DEFINED ENV{RIVET_ROOT})
  message(FATAL_ERROR "RIVET_ROOT is not set. Export RIVET_ROOT to a Rivet checkout.")
endif()
set(RIVET_ROOT "$ENV{RIVET_ROOT}")

# The Rivet platform is consumed as a package library target (project
# constraint, rivet#238): hosts never compile Rivet sources into their own
# executable lists — rivet adding a translation unit needs no downstream
# change. The platform target propagates GTK4 and GIO.
add_subdirectory("${RIVET_ROOT}/platform/linux"
                 "${CMAKE_BINARY_DIR}/rivet-platform-build")

find_package(PkgConfig REQUIRED)
# Local playback needs the GStreamer core at link time (playbin is created
# through the core C API); the plugins-base ABI rides along because the
# audio path is useless without it. gst-plugins-base/good (and gst-libav for
# m4a/mp3) are runtime plugin dependencies — release packages list them.
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0
                  gstreamer-plugins-base-1.0)

add_executable(RivetHost
  src/main.cpp
)

target_include_directories(RivetHost PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(RivetHost PRIVATE
  rivet::platform
  PkgConfig::GSTREAMER
)

target_compile_options(RivetHost PRIVATE -Wall -Wextra -Wpedantic)
