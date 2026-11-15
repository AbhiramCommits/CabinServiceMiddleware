add_library(cabin_ipc STATIC
  broker/broker.cpp
  core/client.cpp
)
target_link_libraries(cabin_ipc PUBLIC cabin_core)
cabin_set_warnings(cabin_ipc)

add_executable(cabind broker/main.cpp)
target_link_libraries(cabind PRIVATE cabin_ipc)
cabin_set_warnings(cabind)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/signals)
  add_library(cabin_signals STATIC
    signals/can.cpp
    signals/codec.cpp
    signals/cache.cpp
    signals/services.cpp
  )
  target_link_libraries(cabin_signals PUBLIC cabin_ipc cabin_core)
  cabin_set_warnings(cabin_signals)

  add_executable(signald signals/signald_main.cpp)
  target_link_libraries(signald PRIVATE cabin_signals)
  cabin_set_warnings(signald)

  add_executable(cabin-services signals/services_main.cpp)
  target_link_libraries(cabin-services PRIVATE cabin_signals)
  cabin_set_warnings(cabin-services)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../bench/bench_ipc.cpp)
  add_executable(bench_ipc ${CMAKE_CURRENT_SOURCE_DIR}/../bench/bench_ipc.cpp)
  target_link_libraries(bench_ipc PRIVATE cabin_ipc)
  target_compile_definitions(bench_ipc PRIVATE
    CABIN_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    CABIN_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  )
  cabin_set_warnings(bench_ipc)
endif()
