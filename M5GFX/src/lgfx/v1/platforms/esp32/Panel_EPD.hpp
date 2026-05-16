/*----------------------------------------------------------------------------/
  Lovyan GFX - Graphics library for embedded devices.

Original Source:
 https://github.com/lovyan03/LovyanGFX/

Licence:
 [FreeBSD](https://github.com/lovyan03/LovyanGFX/blob/master/license.txt)

Author:
 [lovyan03](https://twitter.com/lovyan03)

Contributors:
 [ciniml](https://github.com/ciniml)
 [mongonta0716](https://github.com/mongonta0716)
 [tobozo](https://github.com/tobozo)
/----------------------------------------------------------------------------*/
#pragma once

#include "Bus_EPD.h"

#if SOC_LCD_I80_SUPPORTED

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "lgfx/v1/panel/Panel_HasBuffer.hpp"
#include "lgfx/v1/misc/range.hpp"

namespace lgfx
{
 inline namespace v1
 {
//----------------------------------------------------------------------------

  struct Panel_EPD : public Panel_HasBuffer
  {
    Panel_EPD(void);
    virtual ~Panel_EPD(void);

    struct config_detail_t {
      enum class page_wipe_direction_t : int8_t {
        no_wipe = 0,
        left_to_right = 1,
        right_to_left = 2,
        top_to_bottom = 3,
        bottom_to_top = 4,
      };

      const uint32_t *lut_quality = nullptr;
      const uint32_t *lut_text = nullptr;
      const uint32_t *lut_fast = nullptr;
      const uint32_t *lut_fastest = nullptr;
      size_t lut_quality_step = 0;
      size_t lut_text_step = 0;
      size_t lut_fast_step = 0;
      size_t lut_fastest_step = 0;
      uint8_t line_padding = 0;

      /// background epd writer task priority
      uint8_t task_priority = 2;

      /// background epd writer task pinned core. (APP_CPU_NUM or PRO_CPU_NUM)
      uint8_t task_pinned_core = -1;

      /// Enable horizontal page wipe effect by splitting one update into vertical strips.
      bool page_wipe_enable = true;

      /// Legacy option: false left->right, true right->left.
      /// If page_wipe_direction is set, this is ignored.
      bool page_wipe_right_to_left = false;

      /// Preferred wipe direction in logical screen coordinates.
      /// no_wipe disables strip-split wipe and sends one normal update.
      page_wipe_direction_t page_wipe_direction = page_wipe_direction_t::no_wipe;

      /// If true, logical directions are mapped through display rotation.
      bool page_wipe_follow_rotation = true;

      /// Strip width in pixels for the page wipe effect. Must be >= 2, even-aligned internally.
      uint16_t page_wipe_step_px = 96;

      /// Optional delay between strips in milliseconds. 0 means no delay.
      uint8_t page_wipe_step_delay_ms = 0;
    };

    const config_detail_t& config_detail(void) const { return _config_detail; }
    void config_detail(const config_detail_t& config_detail) { _config_detail = config_detail; }

    void beginTransaction(void) override;
    void endTransaction(void) override;

    bool init(bool use_reset) override;

    void waitDisplay(void) override;
    bool displayBusy(void) override;
    color_depth_t setColorDepth(color_depth_t depth) override;

    void setInvert(bool invert) override;
    void setSleep(bool flg) override;
    void setPowerSave(bool flg) override;
    void setEpdWipeDirection(int_fast8_t dir, bool oneshot = true) override;

    void display(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h) override;

    void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t rawcolor) override;
    void writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t* param, bool use_dma) override;
    void writePixels(pixelcopy_t* param, uint32_t len, bool use_dma) override;

    uint32_t readCommand(uint_fast16_t, uint_fast8_t, uint_fast8_t) override { return 0; }
    uint32_t readData(uint_fast8_t, uint_fast8_t) override { return 0; }

    void readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void* dst, pixelcopy_t* param) override;

  private:
    config_detail_t _config_detail;

    size_t _get_buffer_length(void) const override { return 0; }
    uint8_t _read_pixel(uint_fast16_t x, uint_fast16_t y);
    void _draw_pixels(uint_fast16_t x, uint_fast16_t y, const grayscale_t* values, size_t len);
    void _update_transferred_rect(uint_fast16_t &xs, uint_fast16_t &ys, uint_fast16_t &xe, uint_fast16_t &ye);

    bool init_intenal(void);

    Bus_EPD* getBusEPD(void) { return (Bus_EPD*)_bus; }

    struct update_data_t {
      uint16_t x;
      uint16_t y;
      uint16_t w;
      uint16_t h;
      epd_mode_t mode;
      uint8_t force_white_band;
      bool constexpr operator==(const update_data_t& other) const {
        return x == other.x && y == other.y && w == other.w && h == other.h && mode == other.mode
            && force_white_band == other.force_white_band;
      }
      bool constexpr operator!=(const update_data_t& other) const {
        return !(*this == other);
      }
    };

    static void task_update(Panel_EPD* me);

    TaskHandle_t _task_update_handle = nullptr;
    QueueHandle_t _update_queue_handle = nullptr;
  
    uint8_t* _dma_bufs[2] = { 0, 0 };
    uint16_t* _step_framebuf = nullptr;
    uint16_t* _step_table = nullptr;
    uint8_t* _lut_2pixel = nullptr;
  
    uint8_t _lut_offset_table[6] = { 0, };
    uint8_t _lut_remain_table[6] = { 0, };
    volatile bool _display_busy = false;
    int8_t _wipe_direction_override = 0;
    bool _wipe_direction_override_oneshot = true;
  };

//----------------------------------------------------------------------------
 }
}
#endif
