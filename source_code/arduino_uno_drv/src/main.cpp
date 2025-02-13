/*
    MBI5026 16x40 LED Dual-color Matrix Display
    Driver code for Arduino UNO R3

    Copyright 2025 Samyar Sadat Akhavi
    Written by Samyar Sadat Akhavi, 2025.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https: www.gnu.org/licenses/>.
*/

// Libraries
#include "Arduino.h"
#include <SPI.h>
#include "Adafruit_GFX.h"



// ---- Definitions ----

// Pins
#define ps_mux_sig_pin      PD5
#define ns_drv_grn_lat_pin  PD3
#define ns_drv_grn_en_pin   PD7
#define ns_drv_red_lat_pin  PD2
#define ns_drv_red_en_pin   PD6

// Display config
#define display_height               16    // Pixels
#define display_width                40    // Pixels
#define display_modules_chained      1
#define display_row_on_delay_us      850   // Microseconds
#define buffer_width_multiplier      6
#define x_scroll_extra_buff_padding  10    // Pixels (MAX. 15, will cause overflow)

// Scroll timer
#define scroll_timer_prescale  B00000101

// SPI config
#define spi_clk_frequency  25000000   // 25 MHz

// GFX color definitions
#define BLACK  0x0000
#define RED    0xF800
#define GREEN  0x07E0



// ---- Global variables ----

// Number of bytes for pixel data, ignoring the first empty byte.
constexpr uint8_t num_pixel_data_bytes = (display_modules_chained * 6) - display_modules_chained;
constexpr uint8_t buffer_width_bytes = num_pixel_data_bytes * buffer_width_multiplier;
constexpr uint8_t buffer_width_pixels = buffer_width_bytes * 8;

// Display buffers
uint8_t green_buffer[buffer_width_bytes][display_height] = {0};
uint8_t red_buffer[buffer_width_bytes][display_height] = {0};

// Scrolling
constexpr int16_t scroll_buffer_starting_offset = (display_width + x_scroll_extra_buff_padding) * -1;
uint8_t x_scroll_offset_max = buffer_width_pixels + x_scroll_extra_buff_padding;
int16_t x_scroll_offset = 0;
uint16_t scroll_timer_comp_val = 800;
bool scroll_enabled = false;



// ---- Buffer bit helpers ----
inline void set_buffer_bit(uint8_t (*buffer)[display_height], const uint8_t x, const uint8_t y, const bool state)
{
  const auto x_buffer_num = static_cast<uint8_t>(x / 8);

  if (state) {
    buffer[x_buffer_num][y] = buffer[x_buffer_num][y] | (1 << static_cast<uint8_t>(x % 8));
  } else {
    buffer[x_buffer_num][y] = buffer[x_buffer_num][y] & ~(1 << static_cast<uint8_t>(x % 8));
  }
}

inline bool get_buffer_bit(const uint8_t (*buffer)[display_height], const uint8_t x, const uint8_t y)
{
  return (buffer[static_cast<uint8_t>(x / 8)][y] >> static_cast<uint8_t>(x % 8)) & 1;
}



// ---- GFX class ----
class DisplayMatrix : public Adafruit_GFX
{
  public:
    DisplayMatrix() : Adafruit_GFX(buffer_width_pixels, display_height) {}

    void drawPixel(const int16_t x, const int16_t y, const uint16_t color) override
    {
      if (x < 0 || x > (buffer_width_pixels - 1) || y < 0 || y > (display_height - 1)) {
        return;
      }

      const uint8_t y_inverted = (display_height - 1) - y;

      switch (color) {
        case RED:
          set_buffer_bit(red_buffer, x, y_inverted, true);

          if (get_buffer_bit(green_buffer, x, y_inverted))
            set_buffer_bit(green_buffer, x, y_inverted, false);
          break;
        case GREEN:
          set_buffer_bit(green_buffer, x, y_inverted, true);

          if (get_buffer_bit(red_buffer, x, y_inverted))
            set_buffer_bit(red_buffer, x, y_inverted, false);
          break;
        default:
          set_buffer_bit(red_buffer, x, y_inverted, false);
          set_buffer_bit(green_buffer, x, y_inverted, false);
          break;
      }
    }

    void fillScreen(const uint16_t color) override
    {
      switch (color) {
        case RED:
          memset(red_buffer, 0xFF, sizeof(red_buffer));
          memset(green_buffer, 0, sizeof(green_buffer));
          break;
        case GREEN:
          memset(red_buffer, 0, sizeof(red_buffer));
          memset(green_buffer, 0xFF, sizeof(green_buffer));
          break;
        default:
          memset(red_buffer, 0, sizeof(red_buffer));
          memset(green_buffer, 0, sizeof(green_buffer));
          break;
      }
    }

    void clearDisplay()
    {
      setCursor(0, 0);
      fillScreen(BLACK);
    }

    void enableScroll(const bool enable)
    {
      scroll_enabled = enable;

      if (enable) {
        x_scroll_offset = scroll_buffer_starting_offset;
      } else {
        x_scroll_offset = 0;
      }
    }

    void setScrollSpeed(const uint8_t speed)
    {
      const uint16_t speed_scaled = map(speed, 0, 255, 6000, 315);
      scroll_timer_comp_val = speed_scaled;
      OCR1A = speed_scaled;
    }

    void setEndBufferIgnore(const uint8_t pixels)
    {
      if (pixels <= buffer_width_pixels + x_scroll_extra_buff_padding) {
        x_scroll_offset_max = buffer_width_pixels + x_scroll_extra_buff_padding - pixels;
      }
    }
};

DisplayMatrix gfx_display;



// ---- Functions ----

// MBI5026 data output helper
void write_to_mbi(const uint8_t num_bytes, const uint8_t *data, const uint8_t latch_pin)
{
  for (uint8_t i = 0; i < num_bytes; i++) {
    if (i % 6 == 0) {
      SPIClass::transfer(0x00);  // First byte of data sent to each display module is ignored.
    }

    SPIClass::transfer(data[(num_bytes - 1) - i]);
  }

  // Latch pin toggle
  PORTD |= (1 << latch_pin);
  PORTD &= ~(1 << latch_pin);
}

// Extract byte from buffer with bit offset
inline uint8_t extract_byte(const uint8_t (*buffer)[display_height], const uint8_t row_num, const int16_t bit_offset)
{
  int16_t byte_idx = bit_offset / 8;
  int16_t bit_shift = bit_offset % 8;

  if (bit_shift < 0) {
    bit_shift += 8;
    byte_idx -= 1;
  }

  return (((byte_idx >= 0 && byte_idx < buffer_width_bytes) ? buffer[byte_idx][row_num] : 0) >> bit_shift) |
         (((byte_idx + 1 >= 0 && byte_idx + 1 < buffer_width_bytes) ? buffer[byte_idx + 1][row_num] : 0) << (8 - bit_shift));
}

// General buffer and I/O helper
void write_row_data(const uint8_t (*buffer)[display_height], const uint8_t row_num, const uint8_t latch_pin)
{
  uint8_t tmp_row_buffer[num_pixel_data_bytes] = {0};

  if (!scroll_enabled) {
    for (uint8_t byte_num = 0; byte_num < num_pixel_data_bytes; byte_num++) {
      tmp_row_buffer[byte_num] = buffer[byte_num][row_num];
    }
  } else {
    for (uint8_t byte_num = 0; byte_num < num_pixel_data_bytes; byte_num++) {
      tmp_row_buffer[byte_num] = extract_byte(buffer, row_num, (byte_num * 8) + x_scroll_offset);
    }
  }

  write_to_mbi(num_pixel_data_bytes, tmp_row_buffer, latch_pin);
}

// Frame drawing
void draw_frame()
{
  noInterrupts();

  for (uint8_t i = 0; i < display_height; i++) {
    PORTD |= (1 << ps_mux_sig_pin);       // Mux I/O pin HIGH
    write_row_data(green_buffer, i, ns_drv_grn_lat_pin);
    write_row_data(red_buffer, i, ns_drv_red_lat_pin);
    PORTC = (PORTC & 0xF0) | (i & 0x0F);  // Set mux address pins
    PORTD &= ~(1 << ps_mux_sig_pin);      // Mux I/O pin LOW
    delayMicroseconds(display_row_on_delay_us);
  }

  interrupts();
}

// Timer1 CompA callback (scroll timer)
ISR(TIMER1_COMPA_vect)
{
  OCR1A += scroll_timer_comp_val;

  if (scroll_enabled) {
    x_scroll_offset ++;

    if (x_scroll_offset == x_scroll_offset_max) {
      x_scroll_offset = scroll_buffer_starting_offset;
    }
  }
}



// ---- Main program functions ----
void setup()
{
  // Mux address pins
  pinMode(A0, OUTPUT);
  pinMode(A1, OUTPUT);
  pinMode(A2, OUTPUT);
  pinMode(A3, OUTPUT);

  pinMode(ps_mux_sig_pin, OUTPUT);
  pinMode(ns_drv_red_lat_pin, OUTPUT);
  pinMode(ns_drv_red_en_pin, OUTPUT);
  pinMode(ns_drv_grn_lat_pin, OUTPUT);
  pinMode(ns_drv_grn_en_pin, OUTPUT);

  digitalWrite(ns_drv_red_en_pin, LOW);
  digitalWrite(ns_drv_grn_en_pin, LOW);

  SPIClass::begin();
  SPIClass::beginTransaction(SPISettings(spi_clk_frequency, MSBFIRST, SPI_MODE0));

  // Timer setup
  TCCR1A = 0;
  TCCR1B = 0;
  TCCR1B |= scroll_timer_prescale;
  OCR1A = scroll_timer_comp_val;
  TIMSK1 |= B00000010;

  gfx_display.enableScroll(true);
  gfx_display.setTextSize(2, 2);
  gfx_display.setTextWrap(false);
  gfx_display.clearDisplay();
  gfx_display.setCursor(0, 1);
  gfx_display.setTextColor(RED);
  gfx_display.print("THIS");
  gfx_display.setTextColor(GREEN);
  gfx_display.print(" IS A ");
  gfx_display.setTextColor(RED);
  gfx_display.print("TEST!");
  gfx_display.setScrollSpeed(160);

  int16_t unused_i;
  uint16_t unused_u, text_width;
  gfx_display.getTextBounds("THIS IS A TEST!", 0, 1, &unused_i, &unused_i, &text_width, &unused_u);
  gfx_display.setEndBufferIgnore(buffer_width_pixels - text_width + 1);
}

void loop()
{
  // Preferably do not put ANYTHING ELSE in the main loop.
  // draw_frame() must be called consistently, with no delays.
  // Use timer interrupts for repeating tasks. Though interrupt callbacks
  // must also not take too long to execute.
  draw_frame();
}