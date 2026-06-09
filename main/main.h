#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Firmware Metadata — update these on each release
// ============================================================
#define FW_VERSION      "1.1.0"
#define FW_DATE         "June 5th, 2026"
#define FW_AUTHOR       "Mike Zhao (EtonTech)"

// ============================================================
// OTA Default Download URL
// Change this to the HTTP address of your local firmware server
// ============================================================
#define OTA_DEFAULT_URL "http://10.0.0.134:8000/esp32c3_ir_cloner.bin"

#ifdef __cplusplus
}
#endif
