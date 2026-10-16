find_package(Threads REQUIRED)

add_library(gpu_monitor STATIC GpuMonitor.cpp)
target_include_directories(gpu_monitor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# NVML is opened with dlopen at run time, so the build needs no NVIDIA driver or CUDA toolkit.
target_link_libraries(gpu_monitor PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
