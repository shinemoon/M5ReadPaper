#pragma once
#include <M5Unified.h>
#include <vector>
#include <string>
#include "../device/file_manager.h"

// Draws a centered white rectangle and renders the 2nd-level menu onto the given canvas.
// If canvas is nullptr, the global g_canvas is used.
void show_2nd_level_menu(M5Canvas *canvas = nullptr, bool partial = false, int8_t refInd = 0);
