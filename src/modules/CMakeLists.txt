add_library (veloc-modules SHARED
  module_manager.cpp
  # simple modules
  client_watchdog.cpp transfer_module.cpp chksum_module.cpp versioning_module.cpp
  # aggregation modules
  client_aggregator.cpp ec_module.cpp
  # storage modules
  ${PROJECT_SOURCE_DIR}/src/storage/storage_module.cpp
  ${PROJECT_SOURCE_DIR}/src/storage/posix_module.cpp
  ${PROJECT_SOURCE_DIR}/src/storage/posix_agg_module.cpp
  # common code
  ${PROJECT_SOURCE_DIR}/src/common/command.cpp
  ${PROJECT_SOURCE_DIR}/src/common/config.cpp
  ${PROJECT_SOURCE_DIR}/src/common/file_util.cpp
  ${PROJECT_SOURCE_DIR}/src/common/ckpt_util.cpp
)

add_library (veloc::modules ALIAS veloc-modules)

target_link_libraries (veloc-modules PUBLIC MPI::MPI_CXX er::er OpenSSL::Crypto)

if (axl_FOUND)
  target_sources (veloc-modules PRIVATE  ${PROJECT_SOURCE_DIR}/src/storage/axl_module.cpp)
  target_link_libraries (veloc-modules PUBLIC axl::axl)
endif()
if (DAOS_FOUND)
  target_sources (veloc-modules PRIVATE  ${PROJECT_SOURCE_DIR}/src/storage/daos_module.cpp)
  target_link_libraries (veloc-modules PUBLIC ${DAOS_LIBRARIES})
endif()
if (WITH_DPU_RELAY)
  # Build relay_bridge as a static library from the DPURelayBridge project
  add_library (relay_bridge STATIC ${DPU_RELAY_DIR}/host/relay_bridge.cpp)
  set_target_properties (relay_bridge PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories (relay_bridge PUBLIC ${DPU_RELAY_DIR} ${DPU_RELAY_DIR}/host)
  find_library(IBVERBS_LIB ibverbs REQUIRED)
  find_library(MLX5_LIB mlx5 REQUIRED)
  target_link_libraries (relay_bridge PUBLIC ${IBVERBS_LIB} ${MLX5_LIB})
  # relay_bridge.cpp uses spdlog — pull it in via FetchContent
  include(FetchContent)
  FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.12.0)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  FetchContent_MakeAvailable(spdlog)
  target_link_libraries (relay_bridge PUBLIC spdlog::spdlog)

  target_sources (veloc-modules PRIVATE ${PROJECT_SOURCE_DIR}/src/storage/relay_module.cpp)
  target_link_libraries (veloc-modules PRIVATE relay_bridge)
endif()

target_include_directories (veloc-modules PUBLIC
  $<BUILD_INTERFACE:${VELOC_SOURCE_DIR}>
  $<BUILD_INTERFACE:${VELOC_SOURCE_DIR}/src>
  $<BUILD_INTERFACE:${VELOC_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

set_target_properties (veloc-modules PROPERTIES EXPORT_NAME modules)

# Install libraries
install (TARGETS veloc-modules EXPORT velocTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
