add_executable(sm64ar_headless main.cpp)
target_link_libraries(sm64ar_headless PRIVATE ar_headless ar_render_null sm64_adapter)
if(SM64AR_HAVE_VULKAN)
  target_link_libraries(sm64ar_headless PRIVATE ar_render_vulkan)
  target_compile_definitions(sm64ar_headless PRIVATE SM64AR_HAVE_VULKAN=1)
endif()
# The game resolves lang/, mods/, palettes/, dynos/ next to its executable on Linux (src/pc/platform.c): link them
# from the upstream desktop build (scripts/sm64_oracle.py).
set(SM64AR_HEADLESS_DIR ${CMAKE_BINARY_DIR}/headless)
file(MAKE_DIRECTORY ${SM64AR_HEADLESS_DIR})
set_target_properties(sm64ar_headless PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SM64AR_HEADLESS_DIR})
foreach(dir lang dynos palettes mods)
  file(CREATE_LINK ${PROJECT_SOURCE_DIR}/build/_src/sm64/build/us_pc/${dir} ${SM64AR_HEADLESS_DIR}/${dir} SYMBOLIC)
endforeach()
