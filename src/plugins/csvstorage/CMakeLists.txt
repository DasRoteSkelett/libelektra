include (LibAddMacros)

set_source_files_properties (csvstorage.c PROPERTIES COMPILE_FLAGS "-Wno-unused-result")

add_plugin (csvstorage
	    SOURCES csvstorage.h
		    csvstorage.c
	    LINK_ELEKTRA elektra-ease
			 elektra-proposal
	    ADD_TEST
	    INSTALL_TEST_DATA
	    TEST_README
	    TEST_REQUIRED_PLUGINS directoryvalue)
