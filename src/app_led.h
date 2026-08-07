#ifndef SRC_INCLUDE_APP_LED_H_
#define SRC_INCLUDE_APP_LED_H_

void led_on(void);
void led_off(void);

void light_on(void);
void light_off(void);

void led_set_control(void);

void light_blink_start(uint8_t times, uint16_t ledOnTime, uint16_t ledOffTime);
void light_blink_stop(void);

#if USE_IONIZER
/* Blue trio (3 LEDs in a line): run chaser and battery gauge.
 * mask bit0..bit2 = left..right, matching dev_gpios.blue[]. */
void blue_set_mask(uint8_t mask);
void blue_chaser_start(void);
void blue_chaser_stop(void);
void blue_gauge_show(uint8_t level);    // level = ZCL 0..200 half-percent
/* Red: fault / low battery only. */
void red_set(bool on);
/* Brief red flash - use this for periodic warnings, never a steady red. */
void red_pulse(uint16_t ms);
/* Identify: drive red in antiphase with green for the blink session. */
void led_identify_mode(uint8_t on);
/* True while an indicator is mid-display - sleep must be held off, or deep
 * sleep drops the output drive and truncates it to nothing. */
uint8_t led_display_busy(void);
#endif



#endif /* SRC_INCLUDE_APP_LED_H_ */
