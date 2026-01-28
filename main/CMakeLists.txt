idf_component_register(
    SRCS 
        "main.cpp"
    
    INCLUDE_DIRS 
        "."
    
    # REQUIRES: Main 需要引用這兩個元件的標頭檔
    REQUIRES 
        core_logic 
        port_esp32 
        freertos # main.cpp 中使用了 vTaskDelay
)