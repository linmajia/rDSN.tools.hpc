set(MY_PROJ_NAME dsn.tools.hpc)

# Source files under CURRENT project directory will be automatically included.
# You can manually set MY_PROJ_SRC to include source files under other directories.
if(APPLE)
    set(MY_PROJ_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../plugins/tools.common/native_aio_provider.posix.cpp")
else()
    set(MY_PROJ_SRC "")
endif()

# Search mode for source files under CURRENT project directory?
# "GLOB_RECURSE" for recursive search
# "GLOB" for non-recursive search
set(MY_SRC_SEARCH_MODE "GLOB")

if(APPLE)
    set(MY_PROJ_INC_PATH "../ext/concurrentqueue" "${CMAKE_CURRENT_SOURCE_DIR}/../../../plugins/tools.common")
else()
    set(MY_PROJ_INC_PATH "../ext/concurrentqueue")
endif()

set(MY_PROJ_LIBS gtest)

set(MY_PROJ_LIB_PATH "")

# Extra files that will be installed
set(MY_BINPLACES "")

dsn_add_shared_library()

file(COPY test/ DESTINATION "${CMAKE_BINARY_DIR}/test/${MY_PROJ_NAME}")
