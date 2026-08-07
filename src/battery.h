/*
 * battery.h
 *
 *  Created on: 24 янв. 2025 г.
 *      Author: pvvx
 */

#ifndef _BATTERY_H_
#define _BATTERY_H_

#if USE_IONIZER
/* Li-Ion 18650 read through an external 1:2 divider, so these are *divided*
 * millivolts as returned by get_adc_mv(); multiply by VBAT_DIVIDER_MUL to
 * get the cell voltage.
 *   MAX   2100 = 4.2 V - full
 *   LOW   1600 = 3.2 V - ~95% discharged, almost nothing left in the cell
 *   START 1650 = 3.3 V - boot guard, keeps startup off the LOW edge
 * The 2.0 V flash write floor is not the binding limit on this board: at a
 * 3.2 V cell the rail is still ~3.05 V, so the cell cutoff protects the
 * flash transitively with over a volt to spare. */
#define VBAT_DIVIDER_MUL			2
#define BATTERY_MAX_POWER			2100
#define BATTERY_LOW_POWER			1600
#define BATTERY_START_POWER			1650

/* Cell protection is done in hardware by the pack's DW01, which cuts off
 * around 2.4V - well above the 2.0V flash write floor even with the LDO in
 * dropout, so firmware does not need to sleep to protect either. What the
 * firmware manages instead is *ionizer runs*, on two thresholds:
 *
 *   MIN_RUN   3.2V - don't start a new cycle. Sampled between runs, so this
 *                    is an unloaded reading.
 *   ABORT_RUN 3.0V - stop a cycle already in progress. Sampled during a run,
 *                    so this one is under HV load and legitimately lower.
 *
 * Keeping the device awake and reporting all the way down to the DW01 cutoff
 * is worth more than the charge saved by sleeping: a device on the network
 * can be monitored and updated, one in a deep-sleep retry loop cannot. */
#define BATTERY_MIN_RUN_MV			1600	// 3.2 V unloaded
#define BATTERY_ABORT_RUN_MV		1500	// 3.0 V under load
/* Plausibility floor, 1000 = 2.0 V cell. The cell feeds the regulator that
 * powers this CPU, so a reading below this cannot be a real battery - if it
 * were, nothing would be executing. It means the divider is unpopulated or
 * disconnected, or the wrong ADC channel is selected. See battery.c for why
 * that must not be treated as "flat". */
#define BATTERY_MIN_PLAUSIBLE_MV	1000
#else
/* These boards measure VDD directly (GPIO_VBAT driven high, see app_cfg.h),
 * so the values are rail millivolts rather than cell volts: 2000 is the
 * flash minimum write voltage, and START sits 100 mV above it as startup
 * hysteresis so the device won't boot right at the edge. */
#define VBAT_DIVIDER_MUL			1
#define BATTERY_MAX_POWER			2600
#define BATTERY_LOW_POWER			2000
#define BATTERY_START_POWER			2100
/* 0 = disabled. These boards sense VDD itself rather than a separate cell,
 * and VDD genuinely can sit low (down to ~1.8 V) while the CPU still runs,
 * so a low reading here is physically meaningful and must still be acted on. */
#define BATTERY_MIN_PLAUSIBLE_MV	0
#endif
/* Retry cadence while below threshold. Each retry is a full boot + ADC read
 * (deep sleep here is without retention), so on a battery board a fast retry
 * keeps draining a cell that is already flat - ~0.6mAh/day at 30s versus
 * ~0.06mAh/day at 5min. Recovery latency after charging grows to at most one
 * interval, which is irrelevant next to charge time. Mains boards keep the
 * short retry: there a low reading is a supply fault, and coming back
 * promptly once the rail recovers is worth more than the charge saved. */
#if USE_BATTERY_PM
#define LOW_POWER_SLEEP_TIME_ms		(5*60*1000) // 5 min
#else
#define LOW_POWER_SLEEP_TIME_ms		30*1000 // 30 sec
#endif

#if ZCL_POWER_CFG_SUPPORT
// measured_battery.flag:
#define FLG_MEASURE_BAT_ADV		0x01
#define FLG_MEASURE_BAT_CC		0x02

typedef struct _measured_battery_t {
	u32 summ;		// сумматор
	u16	mv; 		// mV
	u16	average_mv; // mV
	u16 cnt;
	u8	level; 		// in 0.5% 0..200
#if USE_BLE
	u8  batVal; 	// 0..100%
#endif
	u8  flag;
} measured_battery_t;

extern measured_battery_t measured_battery;

#endif

void adc_channel_init(ADC_InputPchTypeDef p_ain); // in adc_drv.c
u16 get_adc_mv(int flg); // in adc_drv.c

void battery_detect(bool startup_flg);

#endif /* _BATTERY_H_ */
