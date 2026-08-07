#ifndef SRC_INCLUDE_APP_RELAY_H_
#define SRC_INCLUDE_APP_RELAY_H_

/* USE_IONIZER changes the layout of dev_gpios_t, and that struct is
 * serialized to flash with a CRC over its bytes. A translation unit that
 * reached this header without app_cfg.h would silently evaluate the #if
 * below as 0 and disagree with the rest of the build about the struct size
 * - surfacing as mysterious flash-table CRC failures rather than a compile
 * error. Pulling it in here (idempotent, it has its own include guard)
 * makes the layout independent of include order. */
#include "app_cfg.h"

// bits dev_gpios flags:
#define GPIOS_FLG_LED1_POL	1 // not change! -> see get_led() -> return 0 or 1
#define GPIOS_FLG_LED2_POL	2
#if USE_IONIZER
#define GPIOS_FLG_BLUE1_POL	4
#define GPIOS_FLG_BLUE2_POL	8
#define GPIOS_FLG_BLUE3_POL	16
#endif

typedef struct {
    uint16_t		flg;	// LED1-2 inversion bits, ...
    GPIO_PinTypeDef rl;
    GPIO_PinTypeDef led1;
    GPIO_PinTypeDef led2;
    GPIO_PinTypeDef key;
    GPIO_PinTypeDef sw1;
    GPIO_PinTypeDef swire;
#if USE_IONIZER
    /* The 3 blue LEDs, in physical left-to-right order - the chaser walks
     * this array, so the order here has to match the board. Appended last so
     * the existing field layout is untouched. */
    GPIO_PinTypeDef blue[3];
#endif
#if USE_BL0937
    GPIO_PinTypeDef sel;
    GPIO_PinTypeDef cf;
    GPIO_PinTypeDef cf1;
#endif
#if USE_BL0942
    GPIO_PinTypeDef rx;
    GPIO_PinTypeDef tx;
#endif
} dev_gpios_t;

extern dev_gpios_t dev_gpios;
extern uint8_t relay_off, relay_state;

void gpio_input_init(GPIO_PinTypeDef pin, GPIO_PullTypeDef pulup);
void gpio_output_init(GPIO_PinTypeDef pin, uint8_t value);
void dev_gpios_init(void);

bool get_relay_status(void);
void set_relay_status(bool status);
#if USE_THERMOSTAT // USE_SENSOR_MY18B20
void set_therm_relay_status(bool status);
#endif

#if USE_CFG_GPIO
dev_gpios_t  dev_gpios_new;
void save_config_gpio(void);
#endif

void dev_relay_init(void);

#endif /* SRC_INCLUDE_APP_RELAY_H_ */
