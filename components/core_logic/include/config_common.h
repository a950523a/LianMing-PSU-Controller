#ifndef CONFIG_COMMON_H
#define CONFIG_COMMON_H

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

#define PSU_ADDRESS     1

// 硬體版本（PSU-Link $INF 的 <hw>，2026-10-28）：目前只有一種板子。之後改板、燒錄檔不能互通時
// 才加一號，到時候再決定要不要用分壓電阻讓同一份韌體自己認板子
#define NODE_HW_REV     1

#define TRANSPORT_UART    0
#define TRANSPORT_ESPNOW  1
#define SOFT_START_INITIAL_CURRENT 10.0f
#define SOFT_START_STEP_CURRENT    10.0f
#define DEFAULT_TARGET_VOLTAGE     100.0f
#define DEFAULT_TARGET_CURRENT     6.0f
#define MAX_TARGET_VOLTAGE         120.0f
// 模組額定 60 A，可調 6–66 A（額定的 10–110 %；使用者實測 5 A 也正常）。
// 也是 $CAP 回報給 TES 的電流上限
#define MAX_TARGET_CURRENT          66.0f

// ── 可用功率（2026-10-17，power_limit.cpp）──────────────────────────────────
// 模組在不同 AC 輸入下能出的 DC 功率：110 V 檔 1.3 kW、220 V 檔 6 kW（使用者提供）。
// 用檔位判斷，不內插：模組本身就分兩區工作，台灣也沒有 160 V 的插座。換型號改這裡
#define AC_VALID_MIN_V          80.0f    // 低於這個當作還沒讀到
#define AC_BAND_SPLIT_V        150.0f    // 以下 110 V 檔、以上 220 V 檔
#define AC_NOMINAL_LOW_V       110.0f    // 換算插座功率時，電壓最多算到這一檔的標稱值
#define AC_NOMINAL_HIGH_V      220.0f
#define MODULE_POWER_LOW_W    1300.0f
#define MODULE_POWER_HIGH_W   6000.0f
#define MODULE_EFFICIENCY        0.93f   // AC → DC，用來把插座電流換算成可輸出的功率（偏保守）
// 插座的電流上限（A × 10）：台灣一般插座 15 A。0 = 不限（專用迴路，只看模組）
#define OUTLET_LIMIT_DEFAULT_DA    150
#define OUTLET_LIMIT_MIN_DA         50
#define OUTLET_LIMIT_MAX_DA        630
#define AC_QUERY_PERIOD_MS       10000   // 自動讀 AC 電壓的間隔（待機）
#define AC_QUERY_PERIOD_ON_MS     2000   // 輸出中：帶載壓降出現時上限要跟著降
// 功率上限換算電流用「量到的輸出電壓」（車子電壓低就能多給電流）：每秒重算，差 0.5 A 以上才重送
#define POWER_RECHECK_MS          1000
#define POWER_RESEND_DELTA_A      0.5f
#define POWER_LIVE_V_MIN          10.0f  // 輸出低於這個電壓（還沒建立）就改用設定電壓

#endif