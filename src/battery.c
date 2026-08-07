/*
 * battery.c
 *
 *  Created on: 18 нояб. 2023 г.
 *      Author: pvvx
 */

#include "tl_common.h"
//#include "device.h"
#include "sensors.h"
#include "battery.h"
//#include "lcd.h"

#if ZCL_POWER_CFG_SUPPORT
measured_battery_t measured_battery;
#endif

#define _BAT_SPEED_CODE_SEC_ //_attribute_ram_code_sec_ // for speed

/* Diagnostic-only: build with -DDIAG_DISABLE_BATTERY_CHECK=1 to keep taking
 * the reading (and still report it over ZCL) but never act on it.
 *
 * Needed for first bring-up of a board whose divider isn't populated yet.
 * battery_detect(1) runs from drv_platform_init() (hw_drv.c) *before* the
 * radio comes up, so an out-of-range reading would deep-sleep the device on
 * a 30s loop that never starts the stack - leaving no way in over the air.
 * The runtime battery_detect(0) calls have the same failure mode against
 * BATTERY_LOW_POWER, which is why the guard sits on the sleep itself rather
 * than on the startup path.
 *
 * Remove once the divider is fitted and the thresholds are confirmed. */
#ifndef DIAG_DISABLE_BATTERY_CHECK
#define DIAG_DISABLE_BATTERY_CHECK 0
#endif

#define BAT_AVERAGE_COUNT_SHL	9 // 4,5,6,7,8,9,10,11,12 -> 16,32,64,128,256,512,1024,2048,4096

#if USE_IONIZER && ZCL_POWER_CFG_SUPPORT
/* Li-Ion discharge is a long way from linear: an 18650 spends most of its
 * capacity between 3.9 and 3.6 V, so a straight line from LOW to MAX would
 * read "full" almost to the end and then fall off a cliff. Piecewise curve
 * instead, in *divided* millivolts (cell voltage / VBAT_DIVIDER_MUL) paired
 * with the 0..200 half-percent units the ZCL attribute expects. */
typedef struct {
	u16 mv;		// divided mV, as returned by get_adc_mv()
	u16 level;	// 0..200
} bat_curve_t;

static const bat_curve_t bat_curve[] = {
	{ 1600,   0 },	// 3.20 V - cutoff
	{ 1700,  16 },	// 3.40 V
	{ 1750,  30 },	// 3.50 V
	{ 1800,  60 },	// 3.60 V
	{ 1850, 100 },	// 3.70 V - nominal
	{ 1925, 140 },	// 3.85 V
	{ 2000, 170 },	// 4.00 V
	{ 2100, 200 },	// 4.20 V - full
};

static u32 battery_level_from_mv(u16 mv) {
	if(mv <= bat_curve[0].mv)
		return 0;
	for(u32 i = 1; i < sizeof(bat_curve)/sizeof(bat_curve[0]); i++) {
		if(mv < bat_curve[i].mv) {
			const bat_curve_t *lo = &bat_curve[i-1];
			const bat_curve_t *hi = &bat_curve[i];
			return lo->level
				 + ((u32)(mv - lo->mv) * (hi->level - lo->level))
				   / (hi->mv - lo->mv);
		}
	}
	return 200;
}
#endif

_BAT_SPEED_CODE_SEC_
//__attribute__((optimize("-Os")))
void battery_detect(bool startup_flg)
{
	u32 battery_level = BATTERY_LOW_POWER;
	if(startup_flg)
		battery_level = BATTERY_START_POWER;
	adc_channel_init(SHL_ADC_VBAT);
	/* Read once into a local: the reading is still wanted (and still
	 * reported) even when the low-battery action below is compiled out. */
	u16 adc_mv = get_adc_mv(0);
#if ZCL_POWER_CFG_SUPPORT
	measured_battery.mv = adc_mv;
#endif
#if DIAG_DISABLE_BATTERY_CHECK || USE_IONIZER
	/* USE_IONIZER: no low-power sleep at all. The pack's DW01 handles cell
	 * protection in hardware, and its ~2.4V cutoff still leaves the rail
	 * above the 2.0V flash floor, so there is nothing here for firmware to
	 * protect. Sleeping would only cost visibility - the check runs before
	 * the radio comes up, so a device in the retry loop is unreachable for
	 * monitoring or recovery. Run limits are enforced per-cycle instead, on
	 * BATTERY_MIN_RUN_MV / BATTERY_ABORT_RUN_MV (see app_onoff.c). */
	(void)battery_level;
#else
	bool plausible = true;
#if BATTERY_MIN_PLAUSIBLE_MV
	/* An implausibly low reading is a sensor fault, not a flat cell - the
	 * cell powers the regulator feeding this CPU, so at that voltage nothing
	 * would be running. Acting on it would be the worst possible response:
	 * battery_detect(1) runs before the radio starts, so the device would
	 * enter a 30s deep-sleep loop that never brings the stack up, making the
	 * fault impossible to see or fix over the air. Keep running instead and
	 * let the reported voltage expose it. */
	plausible = (adc_mv >= BATTERY_MIN_PLAUSIBLE_MV);
#endif
	if(plausible && adc_mv < battery_level)
	{
#if PM_ENABLE
 #if USE_DISPLAY
		display_off();
 #endif
 #if USE_SENSOR_TH
		sensor_go_sleep();
 #endif
		drv_pm_longSleep(PM_SLEEP_MODE_DEEPSLEEP, PM_WAKEUP_SRC_TIMER, LOW_POWER_SLEEP_TIME_ms);
#else
		drv_pm_longSleep(PM_SLEEP_MODE_DEEPSLEEP, PM_WAKEUP_SRC_TIMER, LOW_POWER_SLEEP_TIME_ms);
#endif
	}
#endif
#if ZCL_POWER_CFG_SUPPORT
	measured_battery.summ += measured_battery.mv;
	measured_battery.cnt++;
	if(measured_battery.cnt >= (1<<BAT_AVERAGE_COUNT_SHL)) {
		measured_battery.average_mv = measured_battery.summ >> BAT_AVERAGE_COUNT_SHL;
		measured_battery.summ -= measured_battery.average_mv;
		measured_battery.cnt--;
	} else {
		measured_battery.average_mv = measured_battery.summ / measured_battery.cnt;
	}
#if USE_IONIZER
	battery_level = battery_level_from_mv(measured_battery.average_mv);
#else
	if(measured_battery.average_mv > BATTERY_LOW_POWER) {
		// Linear mapping: 2000mV (0%) to 2600mV (100%)
		// battery_level ranges from 0-200 (0.5% increments for 0-100%)
		battery_level = ((measured_battery.average_mv - BATTERY_LOW_POWER) * 200) / (BATTERY_MAX_POWER - BATTERY_LOW_POWER);
		if(battery_level > 200)
			battery_level = 200;
	} else
		battery_level = 0;
#endif
    measured_battery.level = (u8)battery_level;
#if USE_BLE
    measured_battery.batVal = (u8)(battery_level >> 1);
#endif
    measured_battery.flag = 0xff;
    
    /* Update ZCL Power Config attributes */
    extern uint8_t g_zcl_batteryPercentage;
    extern uint8_t g_zcl_batteryVoltage;
    /* Full-resolution diagnostics, deliberately the *instantaneous* reading
     * rather than the running average the percentage uses: these exist to be
     * compared against a meter at the moment of sampling, and the average is
     * smoothed over up to 512 samples so it would lag a probe test badly. */
    extern uint16_t g_zcl_batteryRawMv;
    extern uint16_t g_zcl_batteryCellMv;
    g_zcl_batteryRawMv  = measured_battery.mv;
    g_zcl_batteryCellMv = (u16)(measured_battery.mv * VBAT_DIVIDER_MUL);
    g_zcl_batteryPercentage = (u8)battery_level;  // 0-200 is the ZigBee spec for battery_percentage_remaining
    /* average_mv is in divided millivolts on boards that sense the pack
     * through an external divider - scale back to the real cell voltage so
     * the reported attribute matches what a meter across the cell shows.
     * VBAT_DIVIDER_MUL is 1 on boards that measure VDD directly. */
    g_zcl_batteryVoltage = (u8)((measured_battery.average_mv * VBAT_DIVIDER_MUL) / 100);  // mV to 0.1V units (decivolts)
#endif
}
