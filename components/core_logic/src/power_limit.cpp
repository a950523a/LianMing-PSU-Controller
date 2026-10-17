#include "power_limit.h"
#include "config_common.h"

// 低壓檔（台灣 110 V）或高壓檔（220 V）。模組本身就是分兩區工作，帶載壓降（220 掉到 200）也不會換檔
static bool high_line(float vac)
{
    return vac >= AC_BAND_SPLIT_V;
}

float pl_module_rating_w(float vac)
{
    if (!(vac >= AC_VALID_MIN_V)) return 0.0f;   // 也擋掉 NaN
    return high_line(vac) ? MODULE_POWER_HIGH_W : MODULE_POWER_LOW_W;
}

uint16_t pl_power_limit_w(float vac, float ac_limit_a)
{
    float p = pl_module_rating_w(vac);
    if (p <= 0.0f) return 0;
    if (ac_limit_a > 0.0f) {
        // 斷路器看的是電流：同樣的功率，電壓掉下來電流反而變大。用實測的 AC 電壓（輸出中每 2 秒讀一次，
        // 已含帶載壓降），但不高於這一檔的標稱電壓 —— 電壓偏高時不多給，偏低時照實測少給
        float nominal = high_line(vac) ? AC_NOMINAL_HIGH_V : AC_NOMINAL_LOW_V;
        float v = vac < nominal ? vac : nominal;
        float ac = v * ac_limit_a * MODULE_EFFICIENCY;   // AC 端給得出的 DC 功率
        if (ac < p) p = ac;
    }
    if (p < 1.0f) p = 1.0f;          // 0 代表「不知道」，真的算出 0 也要回報成有上限
    if (p > 65535.0f) p = 65535.0f;
    return (uint16_t)p;
}

float pl_clamp_current(float amps, float volts, uint16_t pmax_w)
{
    if (pmax_w == 0 || !(volts >= 10.0f)) return amps;
    float lim = (float)pmax_w / volts;
    return amps > lim ? lim : amps;
}
