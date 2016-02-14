project (desktop-tools C)
cmake_minimum_required (VERSION 2.8.11)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)
	# -Wunused-function is pretty annoying here, as everything is static
	set (CMAKE_C_FLAGS "-std=c99 -Wall -Wextra -Wno-unused-function")
endif ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)

# Version
set (project_VERSION_MAJOR "0")
set (project_VERSION_MINOR "1")
set (project_VERSION_PATCH "0")

set (project_VERSION "${project_VERSION_MAJOR}")
set (project_VERSION "${project_VERSION}.${project_VERSION_MINOR}")
set (project_VERSION "${project_VERSION}.${project_VERSION_PATCH}")

# For custom modules
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

# Dependencies
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/liberty/cmake)
include (AddThreads)

find_package (PkgConfig REQUIRED)
pkg_check_modules (dependencies REQUIRED libpulse x11)
pkg_check_modules (gdm gdm glib-2.0 gio-2.0)

option (WITH_GDM "Compile with GDM support" ${gdm_FOUND})

set (project_libraries ${dependencies_LIBRARIES})
include_directories (${dependencies_INCLUDE_DIRS})

# Generate a configuration file
configure_file (${PROJECT_SOURCE_DIR}/config.h.in ${PROJECT_BINARY_DIR}/config.h)
include_directories (${PROJECT_BINARY_DIR})

# Build
add_executable (dwmstatus dwmstatus.c)
target_link_libraries (dwmstatus ${project_libraries})
add_threads (dwmstatus)

add_executable (brightness brightness.c)
target_link_libraries (brightness ${project_libraries})

add_executable (fancontrol-ng fancontrol-ng.c)
target_link_libraries (fancontrol-ng ${project_libraries})

if (WITH_GDM)
	include_directories (${gdm_INCLUDE_DIRS})
	add_executable (gdm-switch-user gdm-switch-user.c)
	target_link_libraries (gdm-switch-user ${gdm_LIBRARIES})
endif (WITH_GDM)

add_executable (siprandom siprandom.c)
target_link_libraries (siprandom ${project_libraries})

# The files to be installed
include (GNUInstallDirs)

configure_file (${PROJECT_SOURCE_DIR}/fancontrol-ng.service.in
	${PROJECT_BINARY_DIR}/fancontrol-ng.service @ONLY)
install (FILES ${PROJECT_BINARY_DIR}/fancontrol-ng.service
	DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
install (FILES fancontrol-ng.conf.example
	DESTINATION ${CMAKE_INSTALL_DATADIR}/fancontrol-ng)

if (WITH_GDM)
	install (TARGETS gdm-switch-user DESTINATION ${CMAKE_INSTALL_BINDIR})
endif (WITH_GDM)

install (TARGETS dwmstatus brightness fancontrol-ng siprandom
	DESTINATION ${CMAKE_INSTALL_BINDIR})
install (PROGRAMS shellify DESTINATION ${CMAKE_INSTALL_BINDIR})
install (FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

# CPack
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Desktop tools")
set (CPACK_PACKAGE_VENDOR "Premysl Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Janouch <p.janouch@gmail.com>")
set (CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set (CPACK_PACKAGE_VERSION_MAJOR ${project_VERSION_MAJOR})
set (CPACK_PACKAGE_VERSION_MINOR ${project_VERSION_MINOR})
set (CPACK_PACKAGE_VERSION_PATCH ${project_VERSION_PATCH})
set (CPACK_GENERATOR "TGZ;ZIP")
set (CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${project_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set (CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}-${project_VERSION}")
set (CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set (CPACK_SOURCE_IGNORE_FILES "/\\\\.git;/build;/CMakeLists.txt.user")
set (CPACK_SOURCE_PACKAGE_FILE_NAME "${PROJECT_NAME}-${project_VERSION}")

set (CPACK_SET_DESTDIR TRUE)
include (CPack)
