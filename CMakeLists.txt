project (desktop-tools C)
cmake_minimum_required (VERSION 2.8.11)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUCC)
	# -Wunused-function is pretty annoying here, as everything is static
	set (wdisabled "-Wno-unused-function")
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -Wall -Wextra ${wdisabled}")
endif ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUCC)

# Version
set (project_VERSION_MAJOR "0")
set (project_VERSION_MINOR "1")
set (project_VERSION_PATCH "0")

set (project_VERSION "${project_VERSION_MAJOR}")
set (project_VERSION "${project_VERSION}.${project_VERSION_MINOR}")
set (project_VERSION "${project_VERSION}.${project_VERSION_PATCH}")

# Dependencies
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/liberty/cmake)
include (AddThreads)

find_package (PkgConfig REQUIRED)
pkg_check_modules (dependencies REQUIRED libpulse x11 xext xextproto dbus-1)
pkg_check_modules (gdm gdm glib-2.0 gio-2.0)
include_directories (${dependencies_INCLUDE_DIRS})

option (WITH_GDM "Compile with GDM support" ${gdm_FOUND})

# Generate a configuration file
configure_file (${PROJECT_SOURCE_DIR}/config.h.in ${PROJECT_BINARY_DIR}/config.h)
include_directories (${PROJECT_BINARY_DIR})

# Build
foreach (name brightness fancontrol-ng iexec input-switch priod siprandom)
	add_executable (${name} ${name}.c)
endforeach (name)

foreach (name big-brother paswitch wmstatus)
	add_executable (${name} ${name}.c)
	target_link_libraries (${name} ${dependencies_LIBRARIES})
endforeach (name)
add_threads (wmstatus)

if (WITH_GDM)
	include_directories (${gdm_INCLUDE_DIRS})
	add_executable (gdm-switch-user gdm-switch-user.c)
	target_link_libraries (gdm-switch-user ${gdm_LIBRARIES})
endif (WITH_GDM)

# The files to be installed
include (GNUInstallDirs)

# We have to put this under /usr on Open Build Service RPM distros, no idea why
set (SYSTEMD_UNITDIR /lib/systemd/system
	CACHE PATH "Base directory for systemd unit files")

configure_file (${PROJECT_SOURCE_DIR}/fancontrol-ng.service.in
	${PROJECT_BINARY_DIR}/fancontrol-ng.service @ONLY)
install (FILES fancontrol-ng.conf.example
	DESTINATION ${CMAKE_INSTALL_DATADIR}/fancontrol-ng)

configure_file (${PROJECT_SOURCE_DIR}/priod.service.in
	${PROJECT_BINARY_DIR}/priod.service @ONLY)
install (FILES priod.conf.example
	DESTINATION ${CMAKE_INSTALL_DATADIR}/priod)

# System-wide unit files should be installed under /lib and not /usr/lib
install (FILES
	${PROJECT_BINARY_DIR}/fancontrol-ng.service
	${PROJECT_BINARY_DIR}/priod.service
	DESTINATION "${SYSTEMD_UNITDIR}")

if (WITH_GDM)
	install (TARGETS gdm-switch-user DESTINATION ${CMAKE_INSTALL_BINDIR})
endif (WITH_GDM)

install (TARGETS wmstatus paswitch brightness input-switch fancontrol-ng priod
	iexec siprandom DESTINATION ${CMAKE_INSTALL_BINDIR})
install (PROGRAMS shellify DESTINATION ${CMAKE_INSTALL_BINDIR})
install (FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

# CPack
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Desktop tools")
set (CPACK_PACKAGE_VENDOR "Premysl Eric Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Eric Janouch <p@janouch.name>")
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
