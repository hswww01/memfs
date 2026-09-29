cmake_minimum_required(VERSION 3.25)

project(memfs
	VERSION 0.2.0
	LANGUAGES C RC)

if(NOT WIN32)
	message(FATAL_ERROR "memfs currently targets Windows + WinFsp only")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(unofficial-sodium CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

set(WINFSP_ROOT "" CACHE PATH "WinFsp installation root")
option(MEMFS_STATIC_WINFSP "Statically link the WinFsp user-mode runtime" ON)
set(WINFSP_SOURCE_ROOT "" CACHE PATH "WinFsp source tree containing inc/ and build/VStudio/")
set(WINFSP_STATIC_LIBRARY "" CACHE FILEPATH "Prebuilt static WinFsp user-mode library")
set(WINFSP_DRIVER_SOURCE "" CACHE FILEPATH "WinFsp driver binary to embed into memfs.exe")

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

if(MEMFS_STATIC_WINFSP AND WINFSP_SOURCE_ROOT)
	list(PREPEND _winfsp_inc_hints "${WINFSP_SOURCE_ROOT}/inc")
endif()

file(GLOB _winfsp_sxs_bin_hints LIST_DIRECTORIES true
	"C:/Program Files (x86)/WinFsp/SxS/sxs.*/bin"
	"C:/Program Files/WinFsp/SxS/sxs.*/bin")
if(_winfsp_sxs_bin_hints)
	list(SORT _winfsp_sxs_bin_hints COMPARE NATURAL ORDER DESCENDING)
	list(PREPEND _winfsp_bin_hints ${_winfsp_sxs_bin_hints})
endif()

if(MEMFS_STATIC_WINFSP AND WINFSP_SOURCE_ROOT)
	set(WINFSP_INCLUDE_DIR "${WINFSP_SOURCE_ROOT}/inc" CACHE PATH
		"WinFsp include directory" FORCE)
else()
	find_path(WINFSP_INCLUDE_DIR
		NAMES winfsp/winfsp.h
		HINTS ${_winfsp_inc_hints}
		REQUIRED)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
	set(_winfsp_lib_name winfsp-a64)
	set(_winfsp_dll_name winfsp-a64.dll)
	set(_winfsp_sys_name winfsp-a64.sys)
else()
	set(_winfsp_lib_name winfsp-x64)
	set(_winfsp_dll_name winfsp-x64.dll)
	set(_winfsp_sys_name winfsp-x64.sys)
endif()

if(MEMFS_STATIC_WINFSP)
	if(NOT WINFSP_STATIC_LIBRARY AND WINFSP_SOURCE_ROOT)
		if(CMAKE_BUILD_TYPE STREQUAL "Debug")
			set(_winfsp_static_config Debug)
		else()
			set(_winfsp_static_config Release)
		endif()

		string(REPLACE "winfsp-" "" _winfsp_static_arch "${_winfsp_lib_name}")
		set(_winfsp_static_candidate
			"${WINFSP_SOURCE_ROOT}/build/VStudio/build/${_winfsp_static_config}/winfsp-static-${_winfsp_static_arch}.lib")

		if(EXISTS "${_winfsp_static_candidate}")
			set(WINFSP_STATIC_LIBRARY "${_winfsp_static_candidate}" CACHE FILEPATH
				"Prebuilt static WinFsp user-mode library" FORCE)
		endif()
	endif()

	if(NOT WINFSP_STATIC_LIBRARY OR NOT EXISTS "${WINFSP_STATIC_LIBRARY}")
		message(FATAL_ERROR
			"MEMFS_STATIC_WINFSP=ON requires WINFSP_STATIC_LIBRARY or a built "
			"WINFSP_SOURCE_ROOT static library")
	endif()

	set(WINFSP_LIBRARY "${WINFSP_STATIC_LIBRARY}")
else()
	find_library(WINFSP_LIBRARY
		NAMES ${_winfsp_lib_name}
		HINTS ${_winfsp_lib_hints}
		REQUIRED)

	find_file(WINFSP_RUNTIME
		NAMES ${_winfsp_dll_name}
		HINTS ${_winfsp_bin_hints}
		REQUIRED)
endif()

if(WINFSP_DRIVER_SOURCE)
	if(NOT EXISTS "${WINFSP_DRIVER_SOURCE}")
		message(FATAL_ERROR "WINFSP_DRIVER_SOURCE does not exist: ${WINFSP_DRIVER_SOURCE}")
	endif()
	set(WINFSP_DRIVER "${WINFSP_DRIVER_SOURCE}" CACHE FILEPATH "WinFsp driver binary to embed" FORCE)
else()
	find_file(WINFSP_DRIVER
		NAMES ${_winfsp_sys_name}
		HINTS ${_winfsp_bin_hints}
		REQUIRED)
endif()

if(MEMFS_STATIC_WINFSP)
	set(WINFSP_RUNTIME_RESOURCE "")
else()
	file(TO_CMAKE_PATH "${WINFSP_RUNTIME}" WINFSP_RUNTIME_RC)
	set(WINFSP_RUNTIME_RESOURCE
		"IDR_MEMFS_WINFSP_DLL RCDATA \"${WINFSP_RUNTIME_RC}\"")
endif()

file(TO_CMAKE_PATH "${WINFSP_DRIVER}" WINFSP_DRIVER_RC)
configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/src/memfs_resources.rc.in
	${CMAKE_CURRENT_BINARY_DIR}/memfs_resources.rc
	@ONLY)

add_library(memfs_core STATIC
	src/memfs_core.c
	src/memfs_alloc.c
	src/memfs_vm.c
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
	src/memfs_winfsp.c
	src/memfs_driver.c
	${CMAKE_CURRENT_BINARY_DIR}/memfs_resources.rc)

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

if(MEMFS_STATIC_WINFSP)
	target_compile_definitions(memfs
		PRIVATE
			WINFSP_STATIC
			MEMFS_WINFSP_STATIC=1)

	target_link_libraries(memfs
		PRIVATE
			credui
			netapi32
			rpcrt4
			secur32
			shlwapi
			version
			wldap32)
	add_executable(memfs_winfsp_probe EXCLUDE_FROM_ALL
		tests/memfs_winfsp_probe.c
		src/memfs_winfsp.c
		src/memfs_driver.c
		${CMAKE_CURRENT_BINARY_DIR}/memfs_resources.rc)

	target_include_directories(memfs_winfsp_probe
		SYSTEM PRIVATE
			${WINFSP_INCLUDE_DIR})
	target_include_directories(memfs_winfsp_probe
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src)

	target_compile_definitions(memfs_winfsp_probe
		PRIVATE
			WINFSP_STATIC
			MEMFS_WINFSP_STATIC=1
			_CRT_SECURE_NO_WARNINGS
			UNICODE
			_UNICODE)

	target_link_libraries(memfs_winfsp_probe
		PRIVATE
			memfs_core
			${WINFSP_LIBRARY}
			advapi32
			credui
			netapi32
			rpcrt4
			secur32
			shlwapi
			version
			wldap32)
else()
	target_link_libraries(memfs PRIVATE delayimp)
	target_link_options(memfs
		PRIVATE
			"/DELAYLOAD:${_winfsp_dll_name}")
endif()

enable_testing()

add_executable(memfs_core_test
	tests/memfs_core_test.c)

target_link_libraries(memfs_core_test
	PRIVATE
		memfs_core)

add_executable(memfs_no_crt_heap_guard
	tests/memfs_no_crt_heap_guard.c)

add_executable(memfs_resource_test
	tests/memfs_resource_test.c)

add_executable(memfs_driver_test
	tests/memfs_driver_test.c
	src/memfs_driver.c)

target_include_directories(memfs_driver_test
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(memfs_driver_test
	PRIVATE
		MEMFS_DRIVER_TESTING
		_CRT_SECURE_NO_WARNINGS)

target_link_libraries(memfs_driver_test
	PRIVATE
		advapi32)
target_include_directories(memfs_resource_test
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MEMFS_STATIC_WINFSP)
	target_compile_definitions(memfs_resource_test PRIVATE MEMFS_WINFSP_STATIC=1)
endif()


target_compile_definitions(memfs_no_crt_heap_guard
	PRIVATE
		_CRT_SECURE_NO_WARNINGS)

add_executable(memfs_bench
	bench/memfs_bench.c)

add_executable(memfs_alloc_mt_bench
	bench/memfs_alloc_mt_bench.c)

add_executable(memfs_soak_bench
	bench/memfs_soak_bench.c)

target_link_libraries(memfs_alloc_mt_bench
	PRIVATE
		memfs_core)

target_link_libraries(memfs_soak_bench
	PRIVATE
		memfs_core
		psapi)

target_link_libraries(memfs_bench
	PRIVATE
		memfs_core
		psapi)
add_executable(memfs_mt_stress_test
	tests/memfs_mt_stress_test.c)

target_link_libraries(memfs_mt_stress_test
	PRIVATE
		memfs_core)

if(MSVC)
	target_compile_options(memfs_mt_stress_test
		PRIVATE
			/W4
			/utf-8)
	target_compile_definitions(memfs_mt_stress_test
		PRIVATE
			_CRT_SECURE_NO_WARNINGS)
endif()

add_test(NAME memfs_mt_stress_test COMMAND memfs_mt_stress_test)

if(MSVC)
	foreach(target memfs_core memfs memfs_core_test memfs_bench memfs_alloc_mt_bench memfs_soak_bench memfs_no_crt_heap_guard memfs_resource_test memfs_driver_test)
		target_compile_options(${target}
			PRIVATE
				/W4
				/utf-8)
	endforeach()

	target_compile_definitions(memfs_core_test
		PRIVATE
			_CRT_SECURE_NO_WARNINGS)
endif()

add_test(NAME memfs_core_test COMMAND memfs_core_test)
add_test(NAME memfs_soak_smoke COMMAND memfs_soak_bench --seconds 2 --sample-ms 500)
set_tests_properties(memfs_soak_smoke PROPERTIES TIMEOUT 15)

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

add_test(NAME memfs_driver_test COMMAND memfs_driver_test)

add_test(NAME memfs_cli_help COMMAND memfs --help)

add_test(NAME memfs_embedded_winfsp_resources
	COMMAND memfs_resource_test $<TARGET_FILE:memfs>)

install(TARGETS memfs RUNTIME DESTINATION bin)
