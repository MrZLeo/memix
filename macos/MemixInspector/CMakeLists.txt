add_library(memix_inspector_bridge STATIC
  Bridge/MMXStore.mm
  Bridge/MMXStore.h
)

target_include_directories(memix_inspector_bridge
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Bridge
)

target_compile_options(memix_inspector_bridge
  PRIVATE
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
)

set_target_properties(memix_inspector_bridge
  PROPERTIES
    CXX_STANDARD ${PROJECT_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OBJCXX_STANDARD ${PROJECT_CXX_STANDARD}
    OBJCXX_STANDARD_REQUIRED ON
    OBJCXX_EXTENSIONS OFF
)

target_link_libraries(memix_inspector_bridge
  PRIVATE
    memix::memix
  PUBLIC
    "-framework Foundation"
)

add_executable(memix-inspector MACOSX_BUNDLE
  App/MemixInspectorApp.swift
  App/InspectorModel.swift
  App/ContentView.swift
)

set_target_properties(memix-inspector
  PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist
    MACOSX_BUNDLE_BUNDLE_NAME "Memix Inspector"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.memix.inspector"
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    Swift_COMPILATION_MODE wholemodule
    Swift_LANGUAGE_VERSION 6
    Swift_MODULE_NAME MemixInspector
)

target_compile_options(memix-inspector
  PRIVATE
    -I ${CMAKE_CURRENT_SOURCE_DIR}/Bridge
    "SHELL:-Xcc -fmodule-map-file=${CMAKE_CURRENT_SOURCE_DIR}/Bridge/module.modulemap"
    "SHELL:-Xcc -fmodules-cache-path=${CMAKE_BINARY_DIR}/swift-module-cache"
)

target_link_libraries(memix-inspector
  PRIVATE
    memix_inspector_bridge
    "-framework AppKit"
    "-framework SwiftUI"
)
