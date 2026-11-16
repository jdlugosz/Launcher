#pragma once

#define IDI_LAUNCHER 101
// Optional product icon group reserved for Shared Updater recovery.
#define IDI_UPDATE_RECOVERY 20000
#define IDD_ABOUT 102
#define IDW_NO_ACTION_DING 103
#define IDR_UPDATER_BOOTSTRAP 104
#define IDC_ABOUT_VERSION 1100
#define IDC_ABOUT_STATUS 1101
#define IDC_ABOUT_DETAILS 1102
#define IDC_ABOUT_COPY 1103
#define IDC_ABOUT_CHECK_NOW 1104
#define IDC_ABOUT_UPDATE_STATUS 1105

// Raw TOML data, addressed by its visible filename.
#ifdef RC_INVOKED
#define LAUNCHER_TOML_RESOURCE LAUNCHER_TOML
#else
#define LAUNCHER_TOML_RESOURCE "LAUNCHER_TOML"
#endif
