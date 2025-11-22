cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0057)
  cmake_policy(SET CMP0057 NEW)
endif()

project(StereoReconPro LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(GLEW REQUIRED)
find_package(OpenGL REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_executable(StereoReconPro
  main.cpp
  StereoProcessor.hpp
  GLRenderer.hpp
)

target_include_directories(StereoReconPro PRIVATE
  ${OpenCV_INCLUDE_DIRS}
  .
)

set(_opencv_targets "")
if (TARGET opencv_world)
  list(APPEND _opencv_targets opencv_world)
elseif (TARGET OpenCV::opencv_world)
  list(APPEND _opencv_targets OpenCV::opencv_world)
elseif (TARGET opencv_core)
  list(APPEND _opencv_targets opencv_core opencv_imgproc opencv_highgui opencv_calib3d opencv_features2d)
else()
  list(APPEND _opencv_targets ${OpenCV_LIBS})
endif()

target_link_libraries(StereoReconPro PRIVATE
  ${_opencv_targets}
  glfw
  GLEW::GLEW
  glm::glm
  OpenGL::GL
  imgui::imgui
)

add_compile_definitions(UNICODE _UNICODE)
set_property(TARGET StereoReconPro PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:StereoReconPro>)