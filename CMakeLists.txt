cmake_minimum_required (VERSION 3.10)
project (desktop-tools VERSION 0.1.0 DESCRIPTION "Desktop tools" LANGUAGES C)

# Moar warnings
set (CMAKE_C_STANDARD 99)
set (CMAKE_C_STANDARD_REQUIRED ON)
set (CMAKE_C_EXTENSIONS OFF)

if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUCC)
	# -Wunused-function is pretty annoying here, as everything is static
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-function")
endif ()

# Dependencies
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/liberty/cmake)
include (AddThreads)

find_package (PkgConfig REQUIRED)
pkg_check_modules (x REQUIRED x11 xext xextproto)
pkg_check_modules (pulse REQUIRED libpulse)
pkg_check_modules (dbus REQUIRED dbus-1)
pkg_check_modules (gdm gdm glib-2.0 gio-2.0)
include_directories (
	${x_INCLUDE_DIRS} ${pulse_INCLUDE_DIRS} ${dbus_INCLUDE_DIRS})
link_directories (
	${x_LIBRARY_DIRS} ${pulse_LIBRARY_DIRS} ${dbus_LIBRARY_DIRS})

option (WITH_GDM "Compile with GDM support" ${gdm_FOUND})

# Generate a configuration file
configure_file (${PROJECT_SOURCE_DIR}/config.h.in
	${PROJECT_BINARY_DIR}/config.h)
include_directories (${PROJECT_BINARY_DIR})

# Build
set (targets wmstatus paswitch siprandom big-brother)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL Linux)
	# These use Linux i2c APIs, but can be made to work on macOS
	list (APPEND targets brightness input-switch)
	# Only iexec could be made to use kqueue
	list (APPEND targets fancontrol-ng priod iexec)
elseif ("${CMAKE_SYSTEM_NAME}" MATCHES BSD)
	# Need this for SIGWINCH in FreeBSD and OpenBSD respectively;
	# our POSIX version macros make it undefined
	add_definitions (-D__BSD_VISIBLE=1 -D_BSD_SOURCE=1)
elseif (APPLE)
	add_definitions (-D_DARWIN_C_SOURCE)
endif ()

foreach (name ${targets})
	add_executable (${name} ${name}.c)
endforeach ()

target_link_libraries (big-brother ${x_LIBRARIES})
target_link_libraries (paswitch ${pulse_LIBRARIES})
target_link_libraries (wmstatus
	${x_LIBRARIES} ${pulse_LIBRARIES} ${dbus_LIBRARIES})
add_threads (wmstatus)

if (WITH_GDM)
	include_directories (${gdm_INCLUDE_DIRS})
	link_directories (${gdm_LIBRARY_DIRS})
	add_executable (gdm-switch-user gdm-switch-user.c)
	target_link_libraries (gdm-switch-user ${gdm_LIBRARIES})
endif ()

# The files to be installed
include (GNUInstallDirs)

# We have to put this under /usr on Open Build Service RPM distros, no idea why
set (SYSTEMD_UNITDIR /lib/systemd/system
	CACHE PATH "Base directory for systemd unit files")

if ("${CMAKE_SYSTEM_NAME}" STREQUAL Linux)
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
endif ()

if (WITH_GDM)
	install (TARGETS gdm-switch-user DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()

list (REMOVE_ITEM targets big-brother)
install (TARGETS ${targets} DESTINATION ${CMAKE_INSTALL_BINDIR})
install (PROGRAMS shellify DESTINATION ${CMAKE_INSTALL_BINDIR})
install (FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

# CPack
set (CPACK_PACKAGE_VENDOR "Premysl Eric Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Eric Janouch <p@janouch.name>")
set (CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set (CPACK_GENERATOR "TGZ;ZIP")
set (CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set (CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}-${PROJECT_VERSION}")
set (CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set (CPACK_SOURCE_IGNORE_FILES "/\\\\.git;/build;/CMakeLists.txt.user")
set (CPACK_SOURCE_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}")

set (CPACK_SET_DESTDIR TRUE)
include (CPack)
