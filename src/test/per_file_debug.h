// Per-file debug control. For each source file with Serial output, define
// DBG_<UPPER_FILENAME> in your build or in include/readpaper.h to enable prints.
// Example: add `#define DBG_BIN_FONT_PRINT 1` to enable prints in bin_font_print.cpp

#pragma once

#include "readpaper.h"

#define DEBUGON 0
#define DBG_BIN_FONT_PRINT 0
#define DBG_BOOK_HANDLE 0
#define DBG_MEMORY_POOL 0
#define DBG_GLYPH_TIMING 0
#define DBG_WIFI_HOTSPOT 0
#define DBG_FONT_BUFFER 0
#define DBG_STATE_MACHINE_TASK 0
#define DBG_UI_DISPLAY 0
#define DBG_BIN_FONT_PRINT 0
#define DBG_TRMNL_SHOW 0


// Override the debug
#define ZH_CONV_DEBUG 0
#define DBG_TEXT_HANDLE 0  // text_handle.cpp 调试日志
#define DBG_BOOK_HANDLE 0
#define DBG_BOOKMARK 0
#define DBG_UI_IMAGE 0
#define DBG_UI_CONTROL 0
#define DBG_DEVICE_INTERRUPT_TASK 0
#define DBG_MAIN 0
#define DBG_SETUP 0
#define DBG_WIFI_HOTSPOT 1
#define DBG_FILE_MANAGER 0
#define DBG_POWERMGT 0
#define  DBG_CHUNKED_FONT_CACHE 0
#define DBG_CONFIG_MANAGER 0


// Default helper: if DBG_xxx not defined, default to 0
#ifndef DBG_MAIN
#if DEBUGON
#define DBG_MAIN 1
#else
#define DBG_MAIN 0
#endif
#endif

#ifndef DBG_BIN_FONT_PRINT
#if DEBUGON
#define DBG_BIN_FONT_PRINT 1
#else
#define DBG_BIN_FONT_PRINT 0
#endif
#endif

#ifndef DBG_UI_DISPLAY
#if DEBUGON
#define DBG_UI_DISPLAY 1
#else
#define DBG_UI_DISPLAY 0
#endif
#endif

#ifndef DBG_UI_CANVAS_UTILS
#if DEBUGON
#define DBG_UI_CANVAS_UTILS 1
#else
#define DBG_UI_CANVAS_UTILS 0
#endif
#endif

#ifndef DBG_UI_IMAGE
#if DEBUGON
#define DBG_UI_IMAGE 1
#else
#define DBG_UI_IMAGE 0
#endif
#endif

#ifndef DBG_TEXT_HANDLE
#if DEBUGON
#define DBG_TEXT_HANDLE 1
#else
#define DBG_TEXT_HANDLE 0
#endif
#endif

#ifndef DBG_GBK_UNICODE_TABLE
#if DEBUGON
#define DBG_GBK_UNICODE_TABLE 1
#else
#define DBG_GBK_UNICODE_TABLE 0
#endif
#endif

// Additional per-file debug flags
#ifndef DBG_BOOK_HANDLE
#if DEBUGON
#define DBG_BOOK_HANDLE 1
#else
#define DBG_BOOK_HANDLE 0
#endif
#endif

#ifndef DBG_FILE_MANAGER
#if DEBUGON
#define DBG_FILE_MANAGER 1
#else
#define DBG_FILE_MANAGER 0
#endif
#endif

#ifndef DBG_CHUNKED_FONT_CACHE
#if DEBUGON
#define DBG_CHUNKED_FONT_CACHE 1
#else
#define DBG_CHUNKED_FONT_CACHE 0
#endif
#endif

#ifndef DBG_FONT_DECODER
#if DEBUGON
#define DBG_FONT_DECODER 1
#else
#define DBG_FONT_DECODER 0
#endif
#endif

#ifndef DBG_MEMORY_POOL
#if DEBUGON
#define DBG_MEMORY_POOL 1
#else
#define DBG_MEMORY_POOL 0
#endif
#endif

#ifndef DBG_POWERMGT
#if DEBUGON
#define DBG_POWERMGT 1
#else
#define DBG_POWERMGT 0
#endif
#endif

#ifndef DBG_TEST_FUNCTIONS
#if DEBUGON
#define DBG_TEST_FUNCTIONS 1
#else
#define DBG_TEST_FUNCTIONS 0
#endif
#endif

#ifndef DBG_SETUP
#if DEBUGON
#define DBG_SETUP 1
#else
#define DBG_SETUP 0
#endif
#endif

#ifndef DBG_STATE_MACHINE_TASK
#if DEBUGON
#define DBG_STATE_MACHINE_TASK 1
#else
#define DBG_STATE_MACHINE_TASK 0
#endif
#endif

#ifndef DBG_TIMER_INTERRUPT_TASK
#if DEBUGON
#define DBG_TIMER_INTERRUPT_TASK 1
#else
#define DBG_TIMER_INTERRUPT_TASK 0
#endif
#endif

#ifndef DBG_DEVICE_INTERRUPT_TASK
#if DEBUGON
#define DBG_DEVICE_INTERRUPT_TASK 1
#else
#define DBG_DEVICE_INTERRUPT_TASK 0
#endif
#endif

#ifndef DBG_UI_CONTROL
#if DEBUGON
#define DBG_UI_CONTROL 1
#else
#define DBG_UI_CONTROL 0
#endif
#endif

#ifndef DBG_CONFIG_MANAGER
#if DEBUGON
#define DBG_CONFIG_MANAGER 1
#else
#define DBG_CONFIG_MANAGER 0
#endif
#endif
#ifndef DBG_SCREENSHOT
#if DEBUGON
#define DBG_SCREENSHOT 1
#else
#define DBG_SCREENSHOT 0
#endif
#endif

#ifndef DBG_TRMNL_SHOW
#if DEBUGON
#define DBG_TRMNL_SHOW 1
#else
#define DBG_TRMNL_SHOW 0
#endif
#endif
