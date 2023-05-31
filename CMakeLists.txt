cmake_minimum_required(VERSION 3.10)

include(GNUInstallDirs)

project(execvars
	VERSION 0.1
    DESCRIPTION "Server to execute predefined command sequences associated with system variables"
)

find_library( LIB_VARSERVER varserver REQUIRED )
find_library( LIB_TJSON tjson REQUIRED )

add_executable( ${PROJECT_NAME}
	src/execvars.c
)

target_include_directories( ${PROJECT_NAME}
	PRIVATE inc
)

target_link_libraries( ${PROJECT_NAME}
	${CMAKE_THREAD_LIBS_INIT}
	rt
	${LIB_VARSERVER}
    ${LIB_TJSON}
    ${LIB_VARTEMPLATE}
)

install(TARGETS ${PROJECT_NAME}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
