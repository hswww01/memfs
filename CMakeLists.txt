cmake_minimum_required(VERSION 3.25)

project(memfs
	VERSION 0.2.0
	LANGUAGES C RC)

if(NOT WIN32)
	message(FATAL_ERROR "memfs currently targets Windows + WinFsp only")
endif()

# The product ships one 64-bit x64 user-mode executable. Windows 11 on Arm
# runs it under x64 emulation; kernel drivers are embedded separately for
# AMD64 and ARM64 and selected at runtime.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR "32-bit Windows/user-mode builds are not supported")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
	message(FATAL_ERROR
		"Native ARM64 memfs.exe is intentionally not built. Build the x64 executable; "
		"Windows 11 on Arm runs it under x64 emulation and memfs selects the ARM64 SYS at runtime.")
endif()
find_program(MEMFS_POWERSHELL_EXECUTABLE
	NAMES pwsh.exe powershell.exe
	REQUIRED)

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
set(WINFSP_DRIVER_X64_SOURCE "" CACHE FILEPATH "Signed WinFsp x64 driver to embed into memfs.exe")
set(WINFSP_DRIVER_ARM64_SOURCE "" CACHE FILEPATH "Signed WinFsp ARM64 driver to embed into memfs.exe")
set(WINFSP_EXPECTED_VERSION "" CACHE STRING
	"Expected WinFsp major.minor for static runtime and embedded driver")

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

set(_winfsp_lib_name winfsp-x64)
set(_winfsp_dll_name winfsp-x64.dll)

if(MEMFS_STATIC_WINFSP)
	if(WINFSP_SOURCE_ROOT)
		if(CMAKE_BUILD_TYPE STREQUAL "Debug")
			set(_winfsp_static_config Debug)
		else()
			set(_winfsp_static_config Release)
		endif()

		set(_winfsp_static_candidate
			"${WINFSP_SOURCE_ROOT}/build/VStudio/build/${_winfsp_static_config}/winfsp-static-x64.lib")

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

if(WINFSP_DRIVER_X64_SOURCE)
	if(NOT EXISTS "${WINFSP_DRIVER_X64_SOURCE}")
		message(FATAL_ERROR "WINFSP_DRIVER_X64_SOURCE does not exist: ${WINFSP_DRIVER_X64_SOURCE}")
	endif()
	set(WINFSP_DRIVER_X64 "${WINFSP_DRIVER_X64_SOURCE}" CACHE FILEPATH
		"WinFsp x64 driver binary to embed" FORCE)
else()
	find_file(WINFSP_DRIVER_X64
		NAMES winfsp-x64.sys
		HINTS ${_winfsp_bin_hints}
		REQUIRED)
endif()

if(WINFSP_DRIVER_ARM64_SOURCE)
	if(NOT EXISTS "${WINFSP_DRIVER_ARM64_SOURCE}")
		message(FATAL_ERROR "WINFSP_DRIVER_ARM64_SOURCE does not exist: ${WINFSP_DRIVER_ARM64_SOURCE}")
	endif()
	set(WINFSP_DRIVER_ARM64 "${WINFSP_DRIVER_ARM64_SOURCE}" CACHE FILEPATH
		"WinFsp ARM64 driver binary to embed" FORCE)
else()
	find_file(WINFSP_DRIVER_ARM64
		NAMES winfsp-a64.sys
		HINTS ${_winfsp_bin_hints}
		REQUIRED)
endif()
set(_winfsp_driver_provenance_json
	"${CMAKE_CURRENT_BINARY_DIR}/winfsp-driver-provenance.json")
execute_process(
	COMMAND "${MEMFS_POWERSHELL_EXECUTABLE}" -NoLogo -NoProfile -NonInteractive
		-ExecutionPolicy Bypass
		-File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify-winfsp-drivers.ps1"
		-X64Path "${WINFSP_DRIVER_X64}"
		-Arm64Path "${WINFSP_DRIVER_ARM64}"
		-OutputJson "${_winfsp_driver_provenance_json}"
	RESULT_VARIABLE _winfsp_provenance_result
	OUTPUT_VARIABLE _winfsp_provenance_output
	ERROR_VARIABLE _winfsp_provenance_error)
if(NOT _winfsp_provenance_result EQUAL 0)
	message(FATAL_ERROR
		"WinFsp driver provenance verification failed: "
		"${_winfsp_provenance_error}${_winfsp_provenance_output}")
endif()
message(STATUS "WinFsp driver PE/signature provenance gate passed")
function(_memfs_read_driver_version driver_path out_full out_canonical)
	execute_process(
		COMMAND "${MEMFS_POWERSHELL_EXECUTABLE}" -NoLogo -NoProfile -NonInteractive -Command
			"(Get-Item -LiteralPath '${driver_path}').VersionInfo.FileVersion"
		RESULT_VARIABLE _driver_version_result
		OUTPUT_VARIABLE _driver_file_version
		ERROR_VARIABLE _driver_version_error
		OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(NOT _driver_version_result EQUAL 0)
		message(FATAL_ERROR
			"Cannot read WinFsp driver version from ${driver_path}: ${_driver_version_error}")
	endif()

	string(REGEX MATCH "^([0-9]+)\\.([0-9]+)"
		_driver_version_match "${_driver_file_version}")
	if(NOT _driver_version_match)
		message(FATAL_ERROR
			"Cannot parse WinFsp driver FileVersion '${_driver_file_version}' from ${driver_path}")
	endif()

	set(${out_full} "${_driver_file_version}" PARENT_SCOPE)
	set(${out_canonical} "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

if(MEMFS_STATIC_WINFSP)
	set(_winfsp_source_version "")
	if(WINFSP_SOURCE_ROOT AND
	   EXISTS "${WINFSP_SOURCE_ROOT}/build/VStudio/build.version.props")
		file(STRINGS
			"${WINFSP_SOURCE_ROOT}/build/VStudio/build.version.props"
			_winfsp_version_line
			REGEX "<MyCanonicalVersion>[0-9]+\\.[0-9]+</MyCanonicalVersion>"
			LIMIT_COUNT 1)
		if(_winfsp_version_line)
			string(REGEX REPLACE
				".*<MyCanonicalVersion>([0-9]+\\.[0-9]+)</MyCanonicalVersion>.*"
				"\\1"
				_winfsp_source_version
				"${_winfsp_version_line}")
		endif()
	endif()

	set(_winfsp_expected_version "")
	if(WINFSP_EXPECTED_VERSION)
		if(_winfsp_source_version AND
		   NOT WINFSP_EXPECTED_VERSION STREQUAL _winfsp_source_version)
			message(FATAL_ERROR
				"WINFSP_EXPECTED_VERSION=${WINFSP_EXPECTED_VERSION} does not match "
				"WINFSP_SOURCE_ROOT canonical version ${_winfsp_source_version}")
		endif()
		set(_winfsp_expected_version "${WINFSP_EXPECTED_VERSION}")
	elseif(_winfsp_source_version)
		set(_winfsp_expected_version "${_winfsp_source_version}")
	endif()

	if(NOT _winfsp_expected_version)
		message(FATAL_ERROR
			"Static WinFsp builds must specify WINFSP_EXPECTED_VERSION or a "
			"WINFSP_SOURCE_ROOT with build/VStudio/build.version.props")
	endif()

	_memfs_read_driver_version("${WINFSP_DRIVER_X64}"
		_winfsp_driver_x64_full _winfsp_driver_x64_version)
	_memfs_read_driver_version("${WINFSP_DRIVER_ARM64}"
		_winfsp_driver_arm64_full _winfsp_driver_arm64_version)

	foreach(_driver_version IN ITEMS
		"${_winfsp_driver_x64_version}"
		"${_winfsp_driver_arm64_version}")
		if(NOT _driver_version STREQUAL _winfsp_expected_version)
			message(FATAL_ERROR
				"WinFsp version mismatch: static x64 runtime expects "
				"${_winfsp_expected_version}, but an embedded driver is ${_driver_version}")
		endif()
	endforeach()

	if(NOT _winfsp_driver_x64_full STREQUAL _winfsp_driver_arm64_full)
		message(FATAL_ERROR
			"Embedded WinFsp drivers must be the same signed release: "
			"x64=${_winfsp_driver_x64_full}, ARM64=${_winfsp_driver_arm64_full}")
	endif()

	string(REGEX MATCH "^([0-9]+)\\.([0-9]+)$"
		_winfsp_expected_match "${_winfsp_expected_version}")
	if(NOT _winfsp_expected_match)
		message(FATAL_ERROR
			"WINFSP_EXPECTED_VERSION must be major.minor, got "
			"'${_winfsp_expected_version}'")
	endif()
	set(_winfsp_expected_major "${CMAKE_MATCH_1}")
	set(_winfsp_expected_minor "${CMAKE_MATCH_2}")
	math(EXPR WINFSP_EXPECTED_VERSION_HEX
		"(${_winfsp_expected_major} << 16) | ${_winfsp_expected_minor}"
		OUTPUT_FORMAT HEXADECIMAL)

	message(STATUS
		"WinFsp static x64 runtime ${_winfsp_expected_version}; "
		"embedded drivers x64/ARM64 ${_winfsp_driver_x64_full}")
endif()

if(MEMFS_STATIC_WINFSP)
	set(WINFSP_RUNTIME_RESOURCE "")
else()
	file(TO_CMAKE_PATH "${WINFSP_RUNTIME}" WINFSP_RUNTIME_RC)
	set(WINFSP_RUNTIME_RESOURCE
		"IDR_MEMFS_WINFSP_DLL RCDATA \"${WINFSP_RUNTIME_RC}\"")
endif()

file(TO_CMAKE_PATH "${WINFSP_DRIVER_X64}" WINFSP_DRIVER_X64_RC)
file(TO_CMAKE_PATH "${WINFSP_DRIVER_ARM64}" WINFSP_DRIVER_ARM64_RC)
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
	src/memfs_cli.c
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
	add_executable(memfs_winfsp_state_test
		tests/memfs_winfsp_state_test.c
		src/memfs_winfsp.c
		src/memfs_driver.c)

	target_include_directories(memfs_winfsp_state_test
		SYSTEM PRIVATE
			${WINFSP_INCLUDE_DIR})
	target_include_directories(memfs_winfsp_state_test
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src)

	target_compile_definitions(memfs_winfsp_state_test
		PRIVATE
			WINFSP_STATIC
			MEMFS_WINFSP_STATIC=1
			MEMFS_WINFSP_TESTING
			_CRT_SECURE_NO_WARNINGS
			UNICODE
			_UNICODE)

	target_link_libraries(memfs_winfsp_state_test
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

add_executable(memfs_cli_test
	tests/memfs_cli_test.c
	src/memfs_cli.c)

target_include_directories(memfs_cli_test
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MEMFS_STATIC_WINFSP)
	add_executable(memfs_static_import_test
		tests/memfs_static_import_test.c)

	add_executable(memfs_winfsp_version_test
		tests/memfs_winfsp_version_test.c)
	target_include_directories(memfs_winfsp_version_test
		SYSTEM PRIVATE
			${WINFSP_INCLUDE_DIR})
	target_compile_definitions(memfs_winfsp_version_test
		PRIVATE
			WINFSP_STATIC
			MEMFS_EXPECTED_WINFSP_VERSION=${WINFSP_EXPECTED_VERSION_HEX})
	target_link_libraries(memfs_winfsp_version_test
		PRIVATE
			${WINFSP_LIBRARY}
			credui
			netapi32
			rpcrt4
			secur32
			shlwapi
			version
			wldap32)
endif()

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

add_executable(memfs_capacity_bench
	bench/memfs_capacity_bench.c)

target_link_libraries(memfs_capacity_bench
	PRIVATE
		memfs_core)

add_executable(memfs_compression_unit_bench
	bench/memfs_compression_unit_bench.c)

target_link_libraries(memfs_compression_unit_bench
	PRIVATE
		zstd::libzstd)

add_executable(memfs_adaptive_compression_bench
	bench/memfs_adaptive_compression_bench.c)

target_link_libraries(memfs_adaptive_compression_bench
	PRIVATE
		zstd::libzstd)

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
	foreach(target memfs_core memfs memfs_core_test memfs_bench memfs_capacity_bench memfs_compression_unit_bench memfs_adaptive_compression_bench memfs_alloc_mt_bench memfs_soak_bench memfs_no_crt_heap_guard memfs_resource_test memfs_driver_test memfs_cli_test)
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
add_test(NAME memfs_cli_test COMMAND memfs_cli_test)

if(MEMFS_STATIC_WINFSP)
	add_test(NAME memfs_static_winfsp_no_dll_import
		COMMAND memfs_static_import_test $<TARGET_FILE:memfs>)
	add_test(NAME memfs_static_winfsp_version
		COMMAND memfs_winfsp_version_test)
	add_test(NAME memfs_winfsp_dispatcher_state
		COMMAND memfs_winfsp_state_test)
endif()

add_test(NAME memfs_cli_help COMMAND memfs --help)

add_test(NAME memfs_cli_reject_negative_size
	COMMAND memfs --mount R: --size -1)
add_test(NAME memfs_cli_reject_oversized_size
	COMMAND memfs --mount R: --size 9223372036854775808)
add_test(NAME memfs_cli_reject_thread_one
	COMMAND memfs --mount R: --threads 1)
add_test(NAME memfs_cli_reject_thread_over_limit
	COMMAND memfs --mount R: --threads 65)
set_tests_properties(
	memfs_cli_reject_negative_size
	memfs_cli_reject_oversized_size
	memfs_cli_reject_thread_one
	memfs_cli_reject_thread_over_limit
	PROPERTIES WILL_FAIL TRUE)

add_test(NAME memfs_embedded_winfsp_resources
	COMMAND memfs_resource_test $<TARGET_FILE:memfs>)
add_test(NAME memfs_winfsp_driver_provenance
	COMMAND "${MEMFS_POWERSHELL_EXECUTABLE}" -NoLogo -NoProfile -NonInteractive
		-ExecutionPolicy Bypass
		-File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify-winfsp-drivers.ps1"
		-X64Path "${WINFSP_DRIVER_X64}"
		-Arm64Path "${WINFSP_DRIVER_ARM64}")

install(TARGETS memfs RUNTIME DESTINATION bin)
