idf_component_register(SRCS "door-control.c" "io.c" "state-machine.c" 
                    INCLUDE_DIRS "." REQUIRES "portal-io" "portal300")
