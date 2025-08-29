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
#define display_row_on_delay_us      400   // Microseconds, feel free to experiment with this value
#define x_scroll_extra_buff_padding  10    // Pixels (MAX. 15, will cause overflow)

// If wired as shown in README.md, you can adjust this value to daisy-chain displays horizontally. 
// Everything else will work out-of-the-box (I have tested it). If 10 is not divisible by this number,
// consider adjusting buffer_width_multiplier manually. display_modules_chained * buffer_width_multiplier
// shouldn't be higher than 10, as it may not fit into RAM, or cause a stack overflow at runtime.
#define display_modules_chained   1
#define buffer_width_multiplier   (10 / display_modules_chained)

// Scroll timer
#define scroll_timer_prescale  B00000101

// SPI config
#define spi_clk_frequency  8000000   // 8 MHz (max for ATMega328P @ 16 MHz, though MBI5026 can only do 25 MHz)

// GFX color definitions
#define BLACK   0
#define RED     1
#define GREEN   2
#define ORANGE  3


// ---- Global variables ----
constexpr uint8_t num_disp_data_bytes = display_modules_chained * 6;                     // Number of total data bytes
constexpr uint8_t num_pixel_data_bytes = num_disp_data_bytes - display_modules_chained;  // Number of bytes for pixel data, ignoring the first empty byte.
constexpr uint8_t buffer_width_bytes = num_pixel_data_bytes * buffer_width_multiplier;
constexpr uint16_t buffer_width_pixels = buffer_width_bytes * 8;

// Display buffers
uint8_t green_buffer[buffer_width_bytes][display_height] = {0};
uint8_t red_buffer[buffer_width_bytes][display_height] = {0};

// Temporary display row data buffers
uint8_t row_buffer_grn[num_disp_data_bytes] = {0};
uint8_t row_buffer_red[num_disp_data_bytes] = {0};

// Scrolling
constexpr int16_t scroll_buffer_starting_offset = ((display_width * display_modules_chained) + x_scroll_extra_buff_padding) * -1;
uint16_t x_scroll_offset_max = buffer_width_pixels + x_scroll_extra_buff_padding;
volatile int16_t x_scroll_offset = 0;
uint16_t scroll_timer_comp_val = 800;
bool scroll_enabled = false;


// ---- Buffer bit helpers ----
inline __attribute__((always_inline))
void set_buffer_bit(uint8_t (*buffer)[display_height], const uint16_t x, const uint8_t y, const bool state) {
  const uint8_t x_buffer_num = x >> 3;

  if (state) {
    buffer[x_buffer_num][y] = buffer[x_buffer_num][y] | (1u << (x & 7));
  } else {
    buffer[x_buffer_num][y] = buffer[x_buffer_num][y] & ~(1u << (x & 7));
  }
}

inline __attribute__((always_inline))
bool get_buffer_bit(const uint8_t (*buffer)[display_height], const uint16_t x, const uint8_t y) {
  return (buffer[x >> 3][y] >> (x & 7)) & 1;
}


// ---- Prototypes for buffer row reading functions ----
void read_row_data_static(const uint8_t (*buffer)[display_height], uint8_t* row_buffer, const uint8_t row_num);
void read_row_data_scrolling(const uint8_t (*buffer)[display_height], uint8_t* row_buffer, const uint8_t row_num);

// Buffer row data retrieval function
typedef void (*read_buffer_row_func_t)(const uint8_t (*buffer)[display_height], uint8_t* row_buffer, const uint8_t row_num);
read_buffer_row_func_t read_buffer_row = read_row_data_static;


// ---- GFX class ----
class DisplayMatrix : public Adafruit_GFX {
  public:
    DisplayMatrix() : Adafruit_GFX(buffer_width_pixels, display_height) {}

    void drawPixel(const int16_t x, const int16_t y, const uint16_t color) override {
      if (x < 0 || x > static_cast<int16_t>(buffer_width_pixels - 1) || y < 0 || y > (display_height - 1)) {
        return;
      }

      const uint8_t y_inverted = (display_height - 1) - y;

      switch (static_cast<uint8_t>(color)) {
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
        case ORANGE:
          set_buffer_bit(red_buffer, x, y_inverted, true);
          set_buffer_bit(green_buffer, x, y_inverted, true);
          break;
        default:
          set_buffer_bit(red_buffer, x, y_inverted, false);
          set_buffer_bit(green_buffer, x, y_inverted, false);
          break;
      }
    }

    void fillScreen(const uint16_t color) override {
      switch (static_cast<uint8_t>(color)) {
        case RED:
          memset(red_buffer, 0xFF, sizeof(red_buffer));
          memset(green_buffer, 0, sizeof(green_buffer));
          break;
        case GREEN:
          memset(red_buffer, 0, sizeof(red_buffer));
          memset(green_buffer, 0xFF, sizeof(green_buffer));
          break;
        case ORANGE:
          memset(red_buffer, 0xFF, sizeof(red_buffer));
          memset(green_buffer, 0xFF, sizeof(green_buffer));
          break;
        default:
          memset(red_buffer, 0, sizeof(red_buffer));
          memset(green_buffer, 0, sizeof(green_buffer));
          break;
      }
    }

    void clearDisplay() {
      setCursor(0, 0);
      fillScreen(BLACK);
    }

    void enableScroll(const bool enable) {
      scroll_enabled = enable;

      if (enable) {
        x_scroll_offset = scroll_buffer_starting_offset;
        read_buffer_row = read_row_data_scrolling;
      } else {
        x_scroll_offset = 0;
        read_buffer_row = read_row_data_static;
      }
    }

    void setScrollSpeed(const uint8_t speed) {
      const uint16_t speed_scaled = map(speed, 0, 255, 6000, 315);
      scroll_timer_comp_val = speed_scaled;
      OCR1A = speed_scaled;
    }

    void setEndBufferIgnore(const uint16_t pixels) {
      if (pixels <= buffer_width_pixels + x_scroll_extra_buff_padding) {
        x_scroll_offset_max = buffer_width_pixels + x_scroll_extra_buff_padding - pixels;
      }
    }
};

DisplayMatrix gfx_display;


// ---- Functions ----
// Extract byte from buffer with bit offset
inline __attribute__((always_inline))
uint8_t extract_byte(const uint8_t (*buffer)[display_height], const uint8_t row_num, const int16_t bit_offset) {
  const int16_t byte_idx = bit_offset >> 3;
  const uint8_t bit_shift = static_cast<uint8_t>(bit_offset & 7);

  const uint8_t byte_lo = (byte_idx >= 0 && byte_idx < buffer_width_bytes) ? buffer[byte_idx][row_num] : 0;
  if (bit_shift == 0) return byte_lo;

  const uint8_t byte_hi = (byte_idx + 1 >= 0 && byte_idx + 1 < buffer_width_bytes) ? buffer[byte_idx + 1][row_num] : 0;
  return static_cast<uint8_t>((byte_lo >> bit_shift) | (byte_hi << (8 - bit_shift)));
}

// General buffer and I/O helper
inline __attribute__((always_inline))
void read_row_data_scrolling(const uint8_t (*buffer)[display_height], uint8_t* row_buffer, const uint8_t row_num) {
  uint8_t byte_num = num_pixel_data_bytes;
  uint8_t i = 0;

  while (i < num_disp_data_bytes) {
    row_buffer[i++] = 0x00;  // First byte of data sent to each display module is ignored.

    for (uint8_t j = 0; j < 5; j++, i++) {
      row_buffer[i] = extract_byte(buffer, row_num, (--byte_num << 3) + x_scroll_offset);
    }
  }
}

// Non-scrolling variant of read_row_data(). Slightly faster.
inline __attribute__((always_inline))
void read_row_data_static(const uint8_t (*buffer)[display_height], uint8_t* row_buffer, const uint8_t row_num) {
  uint8_t byte_num = num_pixel_data_bytes;
  uint8_t i = 0;

  while (i < num_disp_data_bytes) {
    row_buffer[i++] = 0x00;  // First byte of data sent to each display module is ignored.

    for (uint8_t j = 0; j < 5; j++, i++) {
      row_buffer[i] = buffer[--byte_num][row_num];
    }
  }
}

// Frame drawing
void draw_frame() {
  noInterrupts();

  for (uint8_t i = 0; i < display_height; i++) {
    read_buffer_row(green_buffer, row_buffer_grn, i);
    read_buffer_row(red_buffer, row_buffer_red, i);

    PIND = (1 << ns_drv_grn_en_pin);      // Disable both colors
    PIND = (1 << ns_drv_red_en_pin);
    PORTC = (PORTC & 0xF0) | (i & 0x0F);  // Set mux address pins

    // Send green data
    SPIClass::transfer(row_buffer_grn, num_disp_data_bytes);
    PIND = (1 << ns_drv_grn_lat_pin);
    PIND = (1 << ns_drv_grn_lat_pin);
    PIND = (1 << ns_drv_grn_en_pin);   // Enable green channel

    // Send red data
    SPIClass::transfer(row_buffer_red, num_disp_data_bytes);
    PIND = (1 << ns_drv_red_lat_pin);
    PIND = (1 << ns_drv_red_lat_pin);
    PIND = (1 << ns_drv_red_en_pin);   // Enable red channel

    delayMicroseconds(display_row_on_delay_us);
  }

  interrupts();
}

// Timer1 CompA callback (scroll timer)
ISR(TIMER1_COMPA_vect) {
  OCR1A += scroll_timer_comp_val;

  if (scroll_enabled) {
    x_scroll_offset ++;

    if (x_scroll_offset == static_cast<int16_t>(x_scroll_offset_max)) {
      x_scroll_offset = scroll_buffer_starting_offset;
    }
  }
}


// ---- Main program functions ----
void setup() {
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

  digitalWrite(ps_mux_sig_pin, LOW);
  digitalWrite(ns_drv_red_lat_pin, LOW);
  digitalWrite(ns_drv_red_en_pin, LOW);
  digitalWrite(ns_drv_grn_lat_pin, LOW);
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
  gfx_display.print("THIS IS A VERY LONG ");
  gfx_display.setTextColor(GREEN);
  gfx_display.print("TEST MESSAGE!");
  gfx_display.setScrollSpeed(255);

  int16_t unused_i;
  uint16_t unused_u, text_width;
  gfx_display.getTextBounds("THIS IS A VERY LONG TEST MESSAGE!", 0, 1, &unused_i, &unused_i, &text_width, &unused_u);
  gfx_display.setEndBufferIgnore(buffer_width_pixels - text_width + 1);
}

void loop() {
  // Preferably do not put ANYTHING ELSE in the main loop.
  // draw_frame() must be called consistently, with no delays.
  // Use timer interrupts for repeating tasks. Though interrupt callbacks
  // must also not take too long to execute.
  draw_frame();
}