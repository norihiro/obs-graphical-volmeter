cmake_minimum_required(VERSION 3.12)

project(obs-graphical-volmeter VERSION 0.1.0)

set(PLUGIN_AUTHOR "Norihiro Kamae")

set(MACOS_BUNDLEID "net.nagater.obs-graphical-volmeter")
set(ID_PREFIX "net.nagater.obs.")
set(MACOS_PACKAGE_UUID "9C198038-1187-46CD-A02F-06C065391F88")
set(MACOS_INSTALLER_UUID "6FAED857-6B03-49BD-B4DD-B53F909BE0B1")
set(PLUGIN_URL "https://github.com/norihiro/obs-graphical-volmeter/")

set(LINUX_MAINTAINER_EMAIL "norihiro@nagater.net")

option(WITH_ASSERT_THREAD "Enable thread assertion" OFF)
option(ENABLE_COVERAGE "Enable coverage option for GCC" OFF)

# In case you need C++
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
	find_package(libobs REQUIRED)
	find_package(obs-frontend-api REQUIRED)
	include(cmake/ObsPluginHelpers.cmake)
	add_library(OBS::frontend-api ALIAS OBS::obs-frontend-api)
endif()

configure_file(
	src/plugin-macros.h.in
	plugin-macros.generated.h
)

add_library(${PROJECT_NAME} MODULE
	src/plugin-main.c
	src/graphical-volmeter.c
	src/volmeter.c
	src/global-config.c
	src/util.c
)

target_link_libraries(${PROJECT_NAME}
	OBS::libobs
	OBS::frontend-api
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

if(OS_WINDOWS)
	# Enable Multicore Builds and disable FH4 (to not depend on VCRUNTIME140_1.DLL when building with VS2019)
	if (MSVC)
		add_definitions(/MP /d2FH4-)
		add_definitions("-D_USE_MATH_DEFINES")
	endif()

	target_link_libraries(${PROJECT_NAME} OBS::w32-pthreads)
endif()

if(OS_LINUX)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
	target_link_options(${PROJECT_NAME} PRIVATE -Wl,-z,defs)
	target_link_libraries(${PROJECT_NAME} m)
	if(ENABLE_COVERAGE)
		target_compile_options(${PROJECT_NAME} PRIVATE -coverage)
		target_link_options(${PROJECT_NAME} PRIVATE -coverage)
	endif()
endif()

if(APPLE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -fvisibility=default")

	set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
	set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
	set(MACOSX_PLUGIN_BUNDLE_VERSION "${PROJECT_VERSION}")
	set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")
endif()

setup_plugin_target(${PROJECT_NAME})

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
	configure_file(
		ci/ci_includes.sh.in
		ci/ci_includes.generated.sh
	)

	configure_file(
		installer/installer-macOS.pkgproj.in
		installer-macOS.generated.pkgproj
	)

	configure_file(
		installer/installer-Windows.iss.in
		installer-Windows.generated.iss
	)
endif()
