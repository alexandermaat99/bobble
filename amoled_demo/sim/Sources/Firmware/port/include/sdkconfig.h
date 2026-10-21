#pragma once
// The few build settings the firmware's code reads, as the board is built (the JPEG decoder from source, not ROM)
#define CONFIG_FREERTOS_HZ 1000
#define CONFIG_LOG_DEFAULT_LEVEL 3
#define CONFIG_HTTPD_MAX_REQ_HDR_LEN 1024
#define CONFIG_HTTPD_MAX_URI_LEN 512
#define CONFIG_JD_USE_ROM 0
#define CONFIG_JD_SZBUF 512
#define CONFIG_JD_FORMAT 0
#define CONFIG_JD_USE_SCALE 1
#define CONFIG_JD_TBLCLIP 1
#define CONFIG_JD_FASTDECODE 1
#define CONFIG_JD_DEFAULT_HUFFMAN 1
