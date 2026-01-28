idf_component_register(
    # U8g2 的原始碼非常多，我們使用 file(GLOB ...) 自動抓取 csrc 下所有的 .c 檔
    # 注意：官方 repo 的原始碼通常在 csrc 資料夾內
    SRCS 
        "csrc/u8g2_bitmap.c"
        "csrc/u8g2_box.c"
        "csrc/u8g2_buffer.c"
        "csrc/u8g2_circle.c"
        "csrc/u8g2_cleardisplay.c"
        "csrc/u8g2_d_memory.c"
        "csrc/u8g2_d_setup.c"
        "csrc/u8g2_font.c"
        "csrc/u8g2_fonts.c"
        "csrc/u8g2_hvline.c"
        "csrc/u8g2_input_value.c"
        "csrc/u8g2_intersection.c"
        "csrc/u8g2_kerning.c"
        "csrc/u8g2_line.c"
        "csrc/u8g2_ll_hvline.c"
        "csrc/u8g2_message.c"
        "csrc/u8g2_polygon.c"
        "csrc/u8g2_selection_list.c"
        "csrc/u8g2_setup.c"
        "csrc/u8x8_8x8.c"
        "csrc/u8x8_byte.c"
        "csrc/u8x8_cad.c"
        "csrc/u8x8_capture.c"
        "csrc/u8x8_d_ssd1306_128x64_noname.c" # 確保包含你的螢幕驅動
        "csrc/u8x8_debounce.c"
        "csrc/u8x8_display.c"
        "csrc/u8x8_fonts.c"
        "csrc/u8x8_gpio.c"
        "csrc/u8x8_input_value.c"
        "csrc/u8x8_message.c"
        "csrc/u8x8_selection_list.c"
        "csrc/u8x8_setup.c"
        "csrc/u8x8_string.c"
        "csrc/u8x8_u16toa.c"
        "csrc/u8x8_u8toa.c"
    
    # 或者更懶人的做法 (不建議用於正式量產，但開發方便)：
    # SRC_DIRS "csrc" 

    INCLUDE_DIRS "csrc"
)