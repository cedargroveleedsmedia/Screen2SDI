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
#define IDC_ROWS            1010
#define IDC_ADD             1011
#define IDC_ADD_ALL         1012
#define IDC_COUNT_TEXT      1013
#define IDC_VMON_COUNT      1014
#define IDC_VMON_SPIN       1015
#define IDC_VMON_APPLY      1016
#define IDC_VMON_INFO       1017

// Per-output rows are created at run time inside the IDC_ROWS panel:
// control id = IDC_OUT_BASE + row * IDC_OUT_STRIDE + offset (up to kMaxOutputs rows).
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
#define IDC_OFS_REMOVE      8
#define IDC_OFS_STATUS      9
