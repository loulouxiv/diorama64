# The Quest app's native library (libdiorama64.so), loaded by the NativeActivity (apps/quest/app). Gradle builds it from
# the repository's root CMakeLists.txt with the NDK; the OpenXR loader comes as a prefab package from its AAR.
set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(native_app_glue STATIC ${GLUE}/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${GLUE})
target_compile_options(native_app_glue PRIVATE -w)

add_library(diorama64 SHARED quest_main.cpp quest_assets.cpp quest_audio.cpp quest_log.cpp ../../common/sm64_launch.cpp)
target_include_directories(diorama64 PRIVATE . ../../common)
target_link_libraries(diorama64 PRIVATE native_app_glue ar_headless ar_render_null sm64_adapter android log aaudio)
# the glue's entry point is called by the framework, not by the library: keep it
target_link_options(diorama64 PRIVATE "-u" "ANativeActivity_onCreate")
find_package(OpenXR CONFIG QUIET)
if(OpenXR_FOUND)
  target_link_libraries(diorama64 PRIVATE OpenXR::openxr_loader)
  target_compile_definitions(diorama64 PRIVATE SM64AR_HAVE_OPENXR=1)
else()
  message(STATUS "Quest app: no OpenXR prefab package (Gradle provides it): built without the OpenXR probe")
endif()
