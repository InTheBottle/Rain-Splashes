cmake_minimum_required(VERSION 3.21)

project(
	RainSplashes
	LANGUAGES CXX
	VERSION 1.0.0
	DESCRIPTION "Rain splash effects for Skyrim"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static CRT globally to match vcpkg x64-windows-static triplet
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- CommonLibSSE-NG ----
set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLib tests" FORCE)
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)

# ---- Plugin Sources ----
set(SOURCES
	src/Main.cpp
)

source_group(
	TREE "${CMAKE_CURRENT_SOURCE_DIR}"
	FILES ${SOURCES}
)

# ---- Build DLL ----
add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_compile_features(${PROJECT_NAME}
	PRIVATE
		cxx_std_23
)

target_include_directories(${PROJECT_NAME}
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(${PROJECT_NAME}
	PRIVATE
		UNICODE
		_UNICODE
		NOMINMAX
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		CommonLibSSE::CommonLibSSE
)

target_precompile_headers(${PROJECT_NAME}
	PRIVATE
		<RE/Skyrim.h>
		<SKSE/SKSE.h>
)

# ---- Output ----
if(DEFINED ENV{SKYRIM_MODS_FOLDER})
	set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/RainSplashes/SKSE/Plugins")
endif()

if(DEFINED OUTPUT_FOLDER)
	# Copy the output DLL to the configured output folder after build
	add_custom_command(
		TARGET ${PROJECT_NAME}
		POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${OUTPUT_FOLDER}"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${OUTPUT_FOLDER}"
		VERBATIM
	)

	if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
		add_custom_command(
			TARGET ${PROJECT_NAME}
			POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${OUTPUT_FOLDER}"
			VERBATIM
		)
	endif()
endif()
