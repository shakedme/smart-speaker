idf_component_register(
    SRCS "get_i2s_pins.c"
    INCLUDE_DIRS "include"
    REQUIRES "driver"
) 

# Unity builds disabled due to redefinition conflicts in ESP-ADF environment
# set_target_properties(${COMPONENT_LIB} PROPERTIES
#     UNITY_BUILD ON
#     UNITY_BUILD_BATCH_SIZE 2  # Very small batch for this simple component
# ) 