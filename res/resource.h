#pragma once

#define IDI_APP             101
#define IDI_GREEN           102
#define IDI_YELLOW          103
#define IDI_RED             104

#define IDD_SETTINGS        200

#define IDC_REFRESH         1002
#define IDC_AUTOSTART       1005
#define IDC_RUNATSTARTUP    1006
#define IDC_STATUS          1007
#define IDC_SOURCE_HINT     1008
#define IDC_BUFFER          1009

// Per-output rows: control id = IDC_OUT_BASE + row * IDC_OUT_STRIDE + offset (literal ids in app.rc).
#define IDC_OUT_BASE        1100
#define IDC_OUT_STRIDE      10
#define IDC_OFS_ENABLE      0
#define IDC_OFS_NAME        1
#define IDC_OFS_TYPE        2
#define IDC_OFS_SRC_NDI     3
#define IDC_OFS_SRC_DISPLAY 4
#define IDC_OFS_DEVICE      5
#define IDC_OFS_FORMAT      6
#define IDC_OFS_GROUP       7
