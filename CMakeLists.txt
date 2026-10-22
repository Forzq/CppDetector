cmake_minimum_required(VERSION 3.25)
project(CppDetector LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(cppdetector
  src/main.cpp
  src/inspect/inspector.cpp
  src/inspect/auto_roi.cpp
  src/inspect/seam.cpp
  src/inspect/geometry.cpp
  src/app/app.cpp
  src/app/decision_log.cpp
  src/app/bench.cpp
  src/web/routes.cpp)
target_compile_definitions(cppdetector PRIVATE CD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
  target_compile_options(cppdetector PRIVATE /utf-8 /W3)
endif()
target_link_libraries(cppdetector PRIVATE
  opencv_core opencv_imgproc opencv_imgcodecs
  httplib::httplib nlohmann_json::nlohmann_json)
