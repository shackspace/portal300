idf_component_register(SRCS "door-control.c" "io.c" "state-machine.c" "mlx90393.c"
                    INCLUDE_DIRS "." REQUIRES "portal-io" "portal300")
