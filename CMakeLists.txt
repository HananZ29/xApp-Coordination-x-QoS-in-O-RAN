# Build rules for the xapp-coord research scenario under ns-3.42.
#
# ns-3's scratch/CMakeLists.txt only globs *.cc in the top level of a scratch
# subdirectory; nested folders (simulation/...) are ignored unless this file
# exists, in which case ns-3 calls add_subdirectory() on it and these rules
# take over. Every C++ source must therefore be listed explicitly below.
#
# Target name: scratch_xapp-coord_xapp-coord (prefix derived by build_exec from
# this directory), executable build/scratch/xapp-coord/ns3.42-xapp-coord-<profile>,
# so `./ns3 run xapp-coord` works exactly as before the restructure.

# Sources use plain relative includes (e.g. "oran-command-lte-tx-power.h"), as in
# the original flat layout; expose each source folder on the include path so the
# recovered files compile unmodified. Directory-scoped: applies to targets below.
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/commands
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/terminators
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/lifecycle
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/lm
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/cmm
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/tests
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/metrics
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/telemetry
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/radio
)

# Listed in the same order (alphabetical by file name) that the previous
# automatic scratch glob produced, so the link order is unchanged.
set(xapp_coord_sources
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/commands/oran-command-lte-tx-power.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/cmm/oran-cmm-coord.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/cmm/oran-cmm-noop-logged.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/lifecycle/cmd-lifecycle.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/lm/oran-lm-scripted-commands.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/control/terminators/oran-e2-node-terminator-lte-enb-tpc.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/metrics/xapp-metrics.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/radio/link-shadowing-loss-model.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/telemetry/oran-query-trigger-telemetry-sink.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/telemetry/oran-report-lte-enb-load.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/telemetry/oran-reporter-lte-enb-load.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/telemetry/oran-report-lte-ue-service.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/telemetry/oran-reporter-lte-ue-service.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/tests/oran-lm-tx-power-step.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/xapp-policy.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/coord-policy.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/ric-telemetry-store.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/oran-lm-xapps.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/rate-estimator.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/b5-policy.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/b5sa-policy.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/scenarios/xapp-coord.cc
)

build_exec(
  EXECNAME xapp-coord
  SOURCE_FILES ${xapp_coord_sources}
  LIBRARIES_TO_LINK "${ns3-libs}" "${ns3-contrib-libs}"
  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/xapp-coord
)

# S2: unit test of the measurement layer (scripted events, no LTE network).
build_exec(
  EXECNAME xapp-metrics-test
  SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/tests/test-xapp-metrics.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/metrics/xapp-metrics.cc
  LIBRARIES_TO_LINK "${ns3-libs}" "${ns3-contrib-libs}"
  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/xapp-coord
)

# S4: known-answer tests of the TS / TPC decision logic (pure functions, no network).
build_exec(
  EXECNAME xapp-policy-test
  SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/tests/test-xapp-policy.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/xapp-policy.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/coord-policy.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/rate-estimator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/b5-policy.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/xapps/b5sa-policy.cc
  LIBRARIES_TO_LINK "${ns3-libs}" "${ns3-contrib-libs}"
  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/xapp-coord
)
