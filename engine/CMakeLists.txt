# Built inside the MySQL source tree, where bench/build-mysql.sh links this
# directory as storage/celltnx and passes CELLTNX_LIONTURTLE_DIR, a
# lionturtle checkout with its C library built. The core and the store
# compile into the plugin, and lionturtle links in statically with its
# symbols hidden.
DISABLE_MISSING_PROFILE_WARNING()
ADD_DEFINITIONS(-DMYSQL_SERVER)

get_filename_component(CELLTNX_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR} REALPATH)
set(CELLTNX_CORE ${CELLTNX_ENGINE_DIR}/../core)
set(CELLTNX_STORE ${CELLTNX_ENGINE_DIR}/../store/lionturtle)
if(NOT EXISTS "${CELLTNX_LIONTURTLE_DIR}/target/release/liblionturtle_c.a")
  message(FATAL_ERROR "celltnx: set CELLTNX_LIONTURTLE_DIR to a lionturtle checkout built by store/lionturtle/fetch.sh")
endif()

MYSQL_ADD_PLUGIN(celltnx
  ha_celltnx.cc
  ${CELLTNX_CORE}/bugs.cc
  ${CELLTNX_CORE}/db.cc
  ${CELLTNX_CORE}/txn.cc
  ${CELLTNX_CORE}/commit.cc
  ${CELLTNX_STORE}/store.cc
  STORAGE_ENGINE
  MODULE_ONLY
  LINK_LIBRARIES extra::rapidjson
  )
target_include_directories(celltnx PRIVATE ${CELLTNX_CORE} ${CELLTNX_STORE} ${CELLTNX_LIONTURTLE_DIR}/lionturtle-c/include)
target_link_libraries(celltnx ${CELLTNX_LIONTURTLE_DIR}/target/release/liblionturtle_c.a util rt pthread m dl)
target_link_options(celltnx PRIVATE -Wl,--exclude-libs,ALL)
target_compile_options(celltnx PRIVATE -Wno-missing-field-initializers)
