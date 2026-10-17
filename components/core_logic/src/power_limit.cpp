#include "power_limit.h"
#include "config_common.h"

float pl_module_rating_w(float vac)
{
    if (!(vac >= AC_VALID_MIN_V)) return 0.0f;   // 也擋掉 NaN
    if (vac <= AC_LOW_LINE_V)  return MODULE_POWER_LOW_W;
    if (vac >= AC_HIGH_LINE_V) return MODULE_POWER_HIGH_W;
    float t = (vac - AC_LOW_LINE_V) / (AC_HIGH_LINE_V - AC_LOW_LINE_V);
    return MODULE_POWER_LOW_W + t * (MODULE_POWER_HIGH_W - MODULE_POWER_LOW_W);
}

uint16_t pl_power_limit_w(float vac, float ac_limit_a)
{
    float p = pl_module_rating_w(vac);
    if (p <= 0.0f) return 0;
    if (ac_limit_a > 0.0f) {
        float ac = vac * ac_limit_a * MODULE_EFFICIENCY;   // AC 端給得出的 DC 功率
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
