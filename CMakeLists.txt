cmake_minimum_required(VERSION 3.25)

project(memfs
	VERSION 0.2.0
	LANGUAGES C)

if(NOT WIN32)
	message(FATAL_ERROR "memfs currently targets Windows + WinFsp only")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(unofficial-sodium CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

set(WINFSP_ROOT "" CACHE PATH "WinFsp installation root")

if(WINFSP_ROOT)
	set(_winfsp_inc_hints "${WINFSP_ROOT}/inc")
	set(_winfsp_lib_hints "${WINFSP_ROOT}/lib")
	set(_winfsp_bin_hints "${WINFSP_ROOT}/bin")
else()
	set(_winfsp_inc_hints
		"C:/Program Files (x86)/WinFsp/inc"
		"C:/Program Files/WinFsp/inc")
	set(_winfsp_lib_hints
		"C:/Program Files (x86)/WinFsp/lib"
		"C:/Program Files/WinFsp/lib")
	set(_winfsp_bin_hints
		"C:/Program Files (x86)/WinFsp/bin"
		"C:/Program Files/WinFsp/bin")
endif()

find_path(WINFSP_INCLUDE_DIR
	NAMES winfsp/winfsp.h
	HINTS ${_winfsp_inc_hints}
	REQUIRED)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
	set(_winfsp_lib_name winfsp-a64)
	set(_winfsp_dll_name winfsp-a64.dll)
else()
	set(_winfsp_lib_name winfsp-x64)
	set(_winfsp_dll_name winfsp-x64.dll)
endif()

find_library(WINFSP_LIBRARY
	NAMES ${_winfsp_lib_name}
	HINTS ${_winfsp_lib_hints}
	REQUIRED)

find_file(WINFSP_RUNTIME
	NAMES ${_winfsp_dll_name}
	HINTS ${_winfsp_bin_hints})

add_library(memfs_core STATIC
	src/memfs_core.c
	src/memfs_alloc.c
	src/memfs_object.c)

target_include_directories(memfs_core
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(memfs_core
	PRIVATE
		_CRT_SECURE_NO_WARNINGS)

target_link_libraries(memfs_core
	PUBLIC
		advapi32
		unofficial-sodium::sodium
		zstd::libzstd)

add_executable(memfs
	src/main.c
	src/memfs_winfsp.c)

target_include_directories(memfs
	SYSTEM PRIVATE
		${WINFSP_INCLUDE_DIR})

target_compile_definitions(memfs
	PRIVATE
		_CRT_SECURE_NO_WARNINGS
		UNICODE
		_UNICODE)

target_link_libraries(memfs
	PRIVATE
		memfs_core
		${WINFSP_LIBRARY}
		advapi32)

enable_testing()

add_executable(memfs_core_test
	tests/memfs_core_test.c)

target_link_libraries(memfs_core_test
	PRIVATE
		memfs_core)

add_executable(memfs_no_crt_heap_guard
	tests/memfs_no_crt_heap_guard.c)

target_compile_definitions(memfs_no_crt_heap_guard
	PRIVATE
		_CRT_SECURE_NO_WARNINGS)

add_executable(memfs_bench
	bench/memfs_bench.c)

target_link_libraries(memfs_bench
	PRIVATE
		memfs_core
		psapi)

if(MSVC)
	foreach(target memfs_core memfs memfs_core_test memfs_bench memfs_no_crt_heap_guard)
		target_compile_options(${target}
			PRIVATE
				/W4
				/utf-8)
	endforeach()

	target_compile_definitions(memfs_core_test
		PRIVATE
			_CRT_SECURE_NO_WARNINGS)
endif()

if(WINFSP_RUNTIME)
	add_custom_command(TARGET memfs POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${WINFSP_RUNTIME}"
			"$<TARGET_FILE_DIR:memfs>")
endif()

add_test(NAME memfs_core_test COMMAND memfs_core_test)

add_test(NAME memfs_no_crt_heap_guard COMMAND memfs_no_crt_heap_guard)
set_tests_properties(memfs_no_crt_heap_guard PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME memfs_no_crt_heap_guard_good_fixture
	COMMAND memfs_no_crt_heap_guard ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/memfs_guard_good.c)
set_tests_properties(memfs_no_crt_heap_guard_good_fixture PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME memfs_no_crt_heap_guard_bad_fixture
	COMMAND memfs_no_crt_heap_guard ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/memfs_guard_bad.c)
set_tests_properties(memfs_no_crt_heap_guard_bad_fixture PROPERTIES
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	WILL_FAIL TRUE)

install(TARGETS memfs RUNTIME DESTINATION bin)
