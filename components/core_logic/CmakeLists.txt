idf_component_register(
    SRCS 
        "src/psu_protocol.cpp"
        "src/app_ui.cpp"
        "src/serial_cmd.cpp"
    
    INCLUDE_DIRS 
        "include"
        
    # REQUIRES: Core Logic 不依賴硬體或 ESP API，可以隨時移植到 STM32 或其他 MCU。
    # psu_link（與 TES 控制板共用的連線協定，PSU-Link 子模組）同樣是零相依的純 C99，不影響這一點。
    REQUIRES psu_link
)