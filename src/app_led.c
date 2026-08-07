
#include "app_main.h"

void led_on(void)
{
	gpio_write(dev_gpios.led1,
			(dev_gpios.flg & GPIOS_FLG_LED1_POL)? LED_OFF : LED_ON);
}

void led_off(void)
{
	gpio_write(dev_gpios.led1,
			(dev_gpios.flg & GPIOS_FLG_LED1_POL)? LED_ON : LED_OFF);
}

bool get_led(void) {
#if LED_ON
	bool ret = gpio_read(dev_gpios.led1)? 0 : 1;
#else
	bool ret = gpio_read(dev_gpios.led1)? 1 : 0;
#endif
	ret ^= (uint8_t)dev_gpios.flg;
	return ret & GPIOS_FLG_LED1_POL; // return only 0 or 1 !
}

void led_set_control(void) {

    switch(cfg_on_off.led_control) {
        case CONTROL_LED_OFF:
            led_off();
            break;
        case CONTROL_LED_ON:
            led_on();
            break;
        case CONTROL_LED_ON_OFF:
            if (get_relay_status())
            	led_on();
            else
            	led_off();
            break;
        default:
            break;
    }
}

void light_on(void)
{
    if(!g_appCtx.timerLedEvt && cfg_on_off.led_control != CONTROL_LED_OFF)
    	led_on();
}

void light_off(void)
{
    if(!g_appCtx.timerLedEvt && cfg_on_off.led_control != CONTROL_LED_ON)
    	led_off();
}


#if USE_IONIZER
/* Identify drives red in antiphase with green. Green alone would be
 * ambiguous on this board - it is also the join blink and the cycle-start
 * marker - whereas alternating two colours is unmistakable across a room and
 * cannot be confused with either. Only set during an identify session, so
 * ordinary blinks stay single-colour. */
static uint8_t g_identifyMode = 0;

void led_identify_mode(uint8_t on)
{
    g_identifyMode = on;
    if(!on)
        red_set(0);
}
#endif

int32_t zclLightTimerCb(void *arg)
{
    int32_t interval = 0;

    if(g_appCtx.sta == g_appCtx.oriSta) {
        g_appCtx.times--;
        if(g_appCtx.times <= 0){
            g_appCtx.timerLedEvt = NULL;
#if USE_IONIZER
            /* Identify usually ends by running out of blinks rather than by
             * an explicit stop - clear here too, or red is left latched on. */
            led_identify_mode(0);
#endif
            led_set_control();
            return -1;
        }
    }
    if(g_appCtx.sta){
    	g_appCtx.sta = 0;
        led_on();
#if USE_IONIZER
        if(g_identifyMode) red_set(0);
#endif
        interval = g_appCtx.ledOnTime;
    } else {
    	g_appCtx.sta = 1;
		led_off();
#if USE_IONIZER
        if(g_identifyMode) red_set(1);
#endif
        interval = g_appCtx.ledOffTime;
    }

    return interval;
}

void light_blink_start(uint8_t times, uint16_t ledOnTime, uint16_t ledOffTime)
{
    uint32_t interval = 0;
    g_appCtx.times = times;

    if(!g_appCtx.timerLedEvt) {
    	g_appCtx.oriSta = get_led();
    	if(g_appCtx.oriSta) { // LED_ON
            g_appCtx.sta = 0;
    		led_off();
            interval = ledOffTime;
        } else {
            g_appCtx.sta = 1;
        	led_on();
            interval = ledOnTime;
        }
        g_appCtx.ledOnTime = ledOnTime;
        g_appCtx.ledOffTime = ledOffTime;

        g_appCtx.timerLedEvt = TL_ZB_TIMER_SCHEDULE(zclLightTimerCb, NULL, interval);
    }
}

void light_blink_stop(void)
{
    if(g_appCtx.timerLedEvt){
        TL_ZB_TIMER_CANCEL(&g_appCtx.timerLedEvt);
        g_appCtx.times = 0;
#if USE_IONIZER
        led_identify_mode(0);
#endif
        led_set_control();
    }
}

#if USE_IONIZER

/* The three blue LEDs sit in a line, which makes them the only indicator on
 * this board with a spatial dimension - so they carry the two things that
 * have position or magnitude: a chaser while the ionizer runs, and a 1-3 bar
 * battery gauge on demand. The two never overlap, so there's no ambiguity
 * about what the line is showing.
 *
 * Deliberately pulsed rather than held on: one LED lit for BLUE_PULSE_ON_MS
 * out of every BLUE_STEP_MS, which costs a small fraction of what three
 * steady LEDs would burn over the life of the cell.
 *
 * BLUE_PULSE_ON_MS is set at the knee of Bloch's law - below roughly 100ms
 * the eye integrates a flash, so perceived brightness tracks duration and a
 * short pulse looks dim no matter how much current it draws. Past ~100ms
 * perception saturates toward the steady-on brightness, so going longer
 * costs energy linearly for almost no visible gain. At 2mA and this duty
 * the chaser runs ~15mAh/year, against ~2400mAh of usable 18650 - i.e. still
 * far below the cell's own self-discharge, so brightness is the free axis
 * here, not power. Raise BLUE_PULSE_ON_MS further only if the diffuser eats
 * more than expected. */
#define BLUE_PULSE_ON_MS    100
#define BLUE_STEP_MS        1500
#define BLUE_GAUGE_MS       2000

/* The chaser and the gauge share one timer handle, so they also need a shared
 * notion of who owns the line - otherwise each just defers to whoever armed
 * the timer first, and a 2-second gauge can suppress a 3-minute chaser with
 * nothing left to restart it once the gauge expires. Priority is explicit:
 * a run always outranks the gauge. */
#define BLUE_MODE_IDLE      0
#define BLUE_MODE_CHASER    1
#define BLUE_MODE_GAUGE     2

static ev_timer_event_t *blueTimerEvt = NULL;
static uint8_t blueMode = BLUE_MODE_IDLE;
static uint8_t bluePos = 0;      // which LED the chaser is on
static uint8_t blueLit = 0;      // 1 while the current pulse is lit

static void blue_write(uint32_t i, bool on)
{
    if(!dev_gpios.blue[i])
        return;
    bool inv = (dev_gpios.flg & (GPIOS_FLG_BLUE1_POL << i)) != 0;
    gpio_write(dev_gpios.blue[i], (on != inv)? LED_ON : LED_OFF);
}

void blue_set_mask(uint8_t mask)
{
    for(uint32_t i = 0; i < 3; i++)
        blue_write(i, (mask >> i) & 1);
}

static int32_t blueChaserCb(void *arg)
{
    (void)arg;
    if(blueLit) {
        blue_set_mask(0);
        blueLit = 0;
        bluePos = (bluePos + 1) % 3;
        return BLUE_STEP_MS - BLUE_PULSE_ON_MS;
    }
    blue_set_mask(BIT(bluePos));
    blueLit = 1;
    return BLUE_PULSE_ON_MS;
}

void blue_chaser_start(void)
{
    if(blueMode == BLUE_MODE_CHASER)
        return;                 // already chasing - don't restart mid-run
    /* Preempt a gauge rather than defer to it: the run is the more important
     * thing to show, and it lasts far longer than the 2s the gauge wanted. */
    if(blueTimerEvt)
        TL_ZB_TIMER_CANCEL(&blueTimerEvt);
    blueMode = BLUE_MODE_CHASER;
    /* Light the first step here rather than scheduling a 1ms timer to do it
     * - immediate feedback when the run starts, and no sub-tick interval. */
    bluePos = 0;
    blue_set_mask(BIT(bluePos));
    blueLit = 1;
    blueTimerEvt = TL_ZB_TIMER_SCHEDULE(blueChaserCb, NULL, BLUE_PULSE_ON_MS);
}

void blue_chaser_stop(void)
{
    if(blueMode != BLUE_MODE_CHASER)
        return;                 // a gauge owns the line - leave it running
    if(blueTimerEvt)
        TL_ZB_TIMER_CANCEL(&blueTimerEvt);
    blueMode = BLUE_MODE_IDLE;
    blueLit = 0;
    blue_set_mask(0);
}

static int32_t blueGaugeCb(void *arg)
{
    (void)arg;
    blueTimerEvt = NULL;
    blueMode = BLUE_MODE_IDLE;
    blue_set_mask(0);
    return -1;
}

/* level is the ZCL battery_percentage_remaining value, 0..200 in half-percent
 * units. Thresholds are coarse on purpose - three bars can only ever say
 * "plenty / some / nearly out", and pretending otherwise would be noise. */
void blue_gauge_show(uint8_t level)
{
    if(blueMode != BLUE_MODE_IDLE)
        return;     // a run owns the line, or a gauge is already showing
    uint8_t mask;
    if(level >= 134)        // > 67%
        mask = 0x07;
    else if(level >= 67)    // > 33%
        mask = 0x03;
    else
        mask = 0x01;
    blueMode = BLUE_MODE_GAUGE;
    blue_set_mask(mask);
    blueTimerEvt = TL_ZB_TIMER_SCHEDULE(blueGaugeCb, NULL, BLUE_GAUGE_MS);
}

/* Red is purely event-driven on this board: it means fault or low battery,
 * never "output is on". Kept off the g_appCtx blink timer, which belongs to
 * the green status LED. */
void red_set(bool on)
{
    if(!dev_gpios.led2)
        return;
    bool inv = (dev_gpios.flg & GPIOS_FLG_LED2_POL) != 0;
    gpio_write(dev_gpios.led2, (on != inv)? LED_ON : LED_OFF);
}

static ev_timer_event_t *redPulseEvt = NULL;

static int32_t redPulseCb(void *arg)
{
    (void)arg;
    redPulseEvt = NULL;
    red_set(0);
    return -1;
}

/* Low battery must never be signalled by holding red on. Steady red draws
 * ~2mA, about 48mAh/day - it would flatten the very cell it is warning
 * about. A short pulse once per tick is ~0.1% duty and reads just as
 * clearly as a periodic warning. */
void red_pulse(uint16_t ms)
{
    if(redPulseEvt || g_identifyMode)
        return;             // identify owns red while it runs
    red_set(1);
    redPulseEvt = TL_ZB_TIMER_SCHEDULE(redPulseCb, NULL, ms);
}

/* True while any indicator is mid-display.
 *
 * Deep sleep does not retain the GPIO output drive, so an indicator set just
 * before drv_pm_lowPowerEnter() is blanked almost immediately - a 2s gauge
 * ends up visible for about a millisecond. Polling faster does not help: that
 * changes how often the device wakes, not whether it stops driving the pins.
 * The sleep has to be held off instead.
 *
 * Every case below is short and self-limiting (gauge 2s, red pulse 50ms,
 * blink a few hundred ms), and the chaser only runs while the output is on,
 * which already blocks sleep - so this cannot hold the device awake
 * indefinitely. Cost is a few mAh per year at any realistic press rate. */
uint8_t led_display_busy(void)
{
    if(g_appCtx.timerLedEvt)            // green blink, identify
        return 1;
    if(blueMode != BLUE_MODE_IDLE)      // chaser or battery gauge
        return 1;
    if(redPulseEvt)                     // low-battery flash
        return 1;
    return 0;
}

#endif // USE_IONIZER
