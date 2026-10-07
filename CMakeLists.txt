cmake_minimum_required(VERSION 3.16)
project(BDFFHDTrainer VERSION 1.2.2 LANGUAGES C CXX)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8 /W3)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no redistributable needed
endif()

add_library(minhook STATIC third_party/minhook/src/buffer.c third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)
add_library(bdffhd_payload SHARED src/payload.cpp src/common.cpp)
target_link_libraries(bdffhd_payload PRIVATE minhook)
target_link_libraries(bdffhd_payload PRIVATE bcrypt user32 gdi32 comctl32 dwmapi uxtheme)
set_target_properties(bdffhd_payload PROPERTIES OUTPUT_NAME "bdffhd_payload" PREFIX "")

add_executable(BDFFHDTrainer WIN32 src/launcher.cpp src/common.cpp src/launcher.rc)
target_link_libraries(BDFFHDTrainer PRIVATE advapi32 user32 shell32 comdlg32)

add_executable(BDFFHDNativePatch WIN32 tools/NativeGamePatch.cpp)
target_link_libraries(BDFFHDNativePatch PRIVATE bcrypt comdlg32 user32)

# Copy config templates next to the binaries
add_custom_command(TARGET BDFFHDTrainer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/features.ini $<TARGET_FILE_DIR:BDFFHDTrainer>)


install(TARGETS BDFFHDTrainer bdffhd_payload BDFFHDNativePatch RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES features.ini README.md DESTINATION .)
install(FILES third_party/minhook/LICENSE.txt DESTINATION . RENAME MinHook-LICENSE.txt)
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_FILE_NAME "BDFFHDTrainer-Portable")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
