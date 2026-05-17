idf_component_register(
    SRCS 
        "main.cpp"
    
    INCLUDE_DIRS 
        "."
    
    # REQUIRES: Main 需要引用這些元件的標頭檔
    REQUIRES
        core_logic
        port_esp32
        web_ctrl
        freertos
)