#ifndef POWER_LIMIT_H
#define POWER_LIMIT_H

#include <stdint.h>

// 這台現在能給多少功率（2026-10-17）。純函式、不碰硬體，可在電腦上測。
//
// 兩個上限取小的：
//   1. 模組本身：依檔位，110 V 檔 MODULE_POWER_LOW_W、220 V 檔 MODULE_POWER_HIGH_W
//      （數字在 config_common.h；帶載壓降不會換檔）
//   2. AC 端：插座（使用者設定的電流上限）或之後的 AC 充電樁（J1772／Type-2 pilot）
//      能給的電流 × min(實測 AC 電壓, 這一檔的標稱電壓) × 效率。斷路器看電流，電壓掉下來
//      同樣功率的電流反而變大，所以實測偏低就照實測，偏高也不多給。
//      台灣 110 V 插座多半 15 A，220 V 冷氣插座也常是 15–20 A，模組額定 6 kW 時 AC 端常常才是真正的上限
// 算出來的值用 PSU-Link 的 $PWR 告訴 TES，送給模組的電流也一律先過這個上限。

// AC 電壓下的模組額定 DC 輸出（W）。AC 讀不到（低於 AC_VALID_MIN_V）回傳 0 = 不知道
float pl_module_rating_w(float vac);

// 目前能給的最大 DC 輸出（W），0 = 不知道（AC 還沒讀到）—— 呼叫端照舊不限。
// ac_limit_a：AC 端的電流上限；0 = 不限（例如專用迴路，只看模組）
uint16_t pl_power_limit_w(float vac, float ac_limit_a);

// 在這個輸出電壓下，電流不能超過 pmax_w / volts。pmax_w 為 0、或電壓太低算不準時原樣回傳
float pl_clamp_current(float amps, float volts, uint16_t pmax_w);

#endif
