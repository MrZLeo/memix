set(PROJECT_CPM_VERSION 0.42.0)
set(PROJECT_CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${PROJECT_CPM_VERSION}.cmake")

# A matching cached file is reused without a download. CPM handles CPM_SOURCE_CACHE.
file(DOWNLOAD
  "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${PROJECT_CPM_VERSION}/CPM.cmake"
  "${PROJECT_CPM_DOWNLOAD_LOCATION}"
  EXPECTED_HASH SHA256=2020b4fc42dba44817983e06342e682ecfc3d2f484a581f11cc5731fbe4dce8a
  TLS_VERIFY ON
)
include("${PROJECT_CPM_DOWNLOAD_LOCATION}")
