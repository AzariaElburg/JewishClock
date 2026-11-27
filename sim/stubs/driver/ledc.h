#pragma once
// PC stand-in (simulator only): the backlight PWM channel - no light on the PC, its duty is kept (below).
#include <cstdint>
typedef enum { LEDC_LOW_SPEED_MODE = 0 } ledc_mode_t;
typedef enum { LEDC_TIMER_8_BIT = 8 } ledc_timer_bit_t;
typedef enum { LEDC_TIMER_0 = 0 } ledc_timer_t;
typedef enum { LEDC_AUTO_CLK = 0 } ledc_clk_cfg_t;
typedef enum { LEDC_CHANNEL_0 = 0 } ledc_channel_t;
typedef enum { LEDC_INTR_DISABLE = 0 } ledc_intr_type_t;
typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
} ledc_timer_config_t;
typedef struct { unsigned int output_invert : 1; } ledc_channel_flags_t;
typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_intr_type_t intr_type;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
    ledc_channel_flags_t flags;
} ledc_channel_config_t;
// The channel keeps its duty as the chip does: set_duty writes the register,
// update_duty makes it the output (2026-11-11: the simulator's /state reports
// the output - "backlight" - so a test can follow brightness to the light).
inline uint32_t g_simLedcDutyPending = 0, g_simLedcDutyOut = 0, g_simLedcBits = 8;
inline int ledc_timer_config(const ledc_timer_config_t* t) { g_simLedcBits = t->duty_resolution; return 0; }
inline int ledc_channel_config(const ledc_channel_config_t* c) { g_simLedcDutyPending = g_simLedcDutyOut = c->duty; return 0; }
inline int ledc_set_duty(ledc_mode_t, ledc_channel_t, uint32_t duty) { g_simLedcDutyPending = duty; return 0; }
inline int ledc_update_duty(ledc_mode_t, ledc_channel_t) { g_simLedcDutyOut = g_simLedcDutyPending; return 0; }
