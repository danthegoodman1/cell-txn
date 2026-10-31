cmake_minimum_required(VERSION 3.20)
project(celltnx C CXX)

# C++20, as MySQL 8.4 compiles; position-independent so the engine plugin
# can link the core.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)

option(CELLTNX_SIMBUGS "Build the deliberate bugs for simulator self-tests" OFF)

add_library(celltnx_core core/bugs.cc core/db.cc core/txn.cc core/commit.cc)
target_include_directories(celltnx_core PUBLIC core)
if(CELLTNX_SIMBUGS)
  target_compile_definitions(celltnx_core PUBLIC CELLTNX_SIMBUGS)
endif()

# The serializability checker and the workloads, ports of the Go packages
# check and workload. Go rounds every floating-point operation, so nothing
# that must match its draws may fuse multiply-adds.
add_library(celltnx_check check/record.cc check/replay.cc)
target_include_directories(celltnx_check PUBLIC check PRIVATE workload)
target_link_libraries(celltnx_check PUBLIC celltnx_core)

add_library(celltnx_workload workload/workload.cc workload/random.cc workload/users.cc workload/tpcc.cc)
target_include_directories(celltnx_workload PUBLIC workload)
target_compile_options(celltnx_workload PUBLIC -ffp-contract=off)
target_link_libraries(celltnx_workload PUBLIC celltnx_check)

# The deterministic simulator, a port of the Go cmd/sim. Clients run as
# Boost.Context fibers.
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS context CONFIG)
add_executable(sim sim/main.cc sim/sim.cc)
target_include_directories(sim PRIVATE sim)
target_link_libraries(sim celltnx_workload Boost::context Threads::Threads)

find_package(GTest REQUIRED)
add_executable(core_test tests/txn_test.cc tests/index_test.cc tests/skiplist_test.cc)
target_link_libraries(core_test celltnx_core GTest::gtest_main Threads::Threads)

add_executable(gocompat_test tests/gocompat_test.cc)
target_include_directories(gocompat_test PRIVATE workload core)
target_compile_definitions(gocompat_test PRIVATE CELLTNX_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata")
target_link_libraries(gocompat_test GTest::gtest_main)

add_executable(check_test tests/check_test.cc)
target_link_libraries(check_test celltnx_check GTest::gtest_main)

add_executable(workload_test tests/workload_test.cc)
target_compile_definitions(workload_test PRIVATE CELLTNX_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata")
target_link_libraries(workload_test celltnx_workload GTest::gtest_main Threads::Threads)

add_executable(stress_test tests/stress_test.cc)
target_link_libraries(stress_test celltnx_workload GTest::gtest_main Threads::Threads)

# The durable store on lionturtle's C library, which
# store/lionturtle/fetch.sh builds.
set(CELLTNX_LIONTURTLE_DIR "$ENV{HOME}/.cache/celltnx/lionturtle-5077607daa24" CACHE PATH "lionturtle checkout")
if(EXISTS "${CELLTNX_LIONTURTLE_DIR}/target/release/liblionturtle_c.a")
  add_library(celltnx_lionturtle store/lionturtle/store.cc)
  target_include_directories(celltnx_lionturtle PUBLIC store/lionturtle PRIVATE "${CELLTNX_LIONTURTLE_DIR}/lionturtle-c/include")
  target_link_libraries(celltnx_lionturtle PUBLIC celltnx_core "${CELLTNX_LIONTURTLE_DIR}/target/release/liblionturtle_c.a"
                        util rt Threads::Threads m dl)
  add_executable(store_test tests/store_test.cc)
  target_link_libraries(store_test celltnx_lionturtle celltnx_workload GTest::gtest_main)
  add_test(NAME store_test COMMAND store_test)
  set_tests_properties(store_test PROPERTIES ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp die_after_fork=0")
else()
  message(STATUS "lionturtle not found at ${CELLTNX_LIONTURTLE_DIR}; run store/lionturtle/fetch.sh for the store")
endif()

enable_testing()
add_test(NAME core_test COMMAND core_test)
add_test(NAME gocompat_test COMMAND gocompat_test)
add_test(NAME check_test COMMAND check_test)
add_test(NAME workload_test COMMAND workload_test)
add_test(NAME stress_test COMMAND stress_test)
