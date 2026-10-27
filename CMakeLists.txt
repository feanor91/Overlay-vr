cmake_minimum_required(VERSION 3.20)
project(OverlayVR CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
  message(FATAL_ERROR "OverlayVR cible Windows (Virtual Desktop + VDXR).")
endif()
add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
if(MSVC)
  add_compile_options(/utf-8)  # textes accentues dans le code
endif()

include(FetchContent)
FetchContent_Declare(openxr
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
  GIT_TAG release-1.0.34)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(openxr json)

# Couche API OpenXR globale (chargee par tous les jeux OpenXR).
add_library(overlayvr_layer SHARED layer/layer.cpp)
set_target_properties(overlayvr_layer PROPERTIES
  OUTPUT_NAME XR_APILAYER_OVERLAYVR_zones
  PREFIX "")
target_link_libraries(overlayvr_layer PRIVATE OpenXR::headers nlohmann_json::nlohmann_json dxguid)
add_custom_command(TARGET overlayvr_layer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/layer/XR_APILAYER_OVERLAYVR_zones.json $<TARGET_FILE_DIR:overlayvr_layer>)

# Editeur en casque.
add_executable(overlayvr_editor editor/editor.cpp)
set_target_properties(overlayvr_editor PROPERTIES OUTPUT_NAME OverlayVR_Editor)
target_link_libraries(overlayvr_editor PRIVATE OpenXR::headers OpenXR::openxr_loader
  nlohmann_json::nlohmann_json d3d11 dxgi d3dcompiler dxguid gdi32)

# Scripts d'installation a cote de la DLL.
add_custom_command(TARGET overlayvr_layer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/install/install_layer.bat
          ${CMAKE_SOURCE_DIR}/install/uninstall_layer.bat $<TARGET_FILE_DIR:overlayvr_layer>)

# Application de la barre d'etat : zones ON/OFF, couche OpenXR, lancement de l'editeur.
add_executable(overlayvr_tray WIN32 tray/tray.cpp)
set_target_properties(overlayvr_tray PROPERTIES OUTPUT_NAME OverlayVR_Tray)
target_compile_definitions(overlayvr_tray PRIVATE UNICODE _UNICODE)
target_link_libraries(overlayvr_tray PRIVATE OpenXR::headers nlohmann_json::nlohmann_json shell32 advapi32 user32 gdi32)
