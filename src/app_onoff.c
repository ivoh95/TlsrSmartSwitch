#include "app_main.h"
#if USE_SENSOR_MY18B20
#include "my18b20.h"
#endif
#if USE_IONIZER
#include "battery.h"
#endif

/* Auto-off timer backing ZCL_CMD_ON_WITH_TIMED_OFF ("On With Timed Off").
 * Any direct on/off/toggle (button, plain ZCL On/Off, restored config on
 * boot) cancels a pending timed session - it means "stay in this state
 * indefinitely" rather than honoring a previously requested timeout. */
static ev_timer_event_t *g_onWithTimedOffEvt = NULL;

static void onWithTimedOff_cancelTimer(void) {
    if (g_onWithTimedOffEvt) {
        TL_ZB_TIMER_CANCEL(&g_onWithTimedOffEvt);
    }
}

#if USE_IONIZER
/* onTime is u16 tenths in the ZCL command, so runs longer than this cannot be
 * expressed there; cap everything at the same limit for consistency. */
#define ION_RUN_DURATION_MAX_S  6000

static void onWithTimedOff_arm_ms(u32 ms);
#endif

void cmdOnOff_set(bool status) {
#if USE_IONIZER
    bool was_on = cfg_on_off_saved.onOff;
#endif
    onWithTimedOff_cancelTimer();
    if(cfg_on_off_saved.onOff != status) {
    	cfg_on_off.onOff = status;
    	if(cfg_on_off.startUpOnOff != ZCL_START_UP_ONOFF_SET_ONOFF_TO_OFF
   			&& cfg_on_off.startUpOnOff != ZCL_START_UP_ONOFF_SET_ONOFF_TO_ON) {
        	save_config_on_off();
    	} else {
    		cfg_on_off_saved.onOff = cfg_on_off.onOff;
    	}
    }
#if USE_THERMOSTAT // USE_SENSOR_MY18B20
    set_therm_relay_status(status);
#else
	set_relay_status(status);
#endif
#if USE_IONIZER
    /* On this board every "on" is a cycle, never an indefinite latch. Arming
     * here rather than at each call site is deliberate: the HV module must be
     * bounded no matter what turned it on - the HA switch entity, a scene, a
     * group command, startUpOnOff at boot, or the button - and a per-caller
     * rule only holds until someone adds a caller. Off cancels it via the
     * cancelTimer() above, so "on starts a cycle, off stops it" holds
     * everywhere.
     *
     * A repeat "on" while already running restarts the cycle (re-arming
     * below), but only a genuine off->on transition counts as a new run. */
    if (status) {
        if (!was_on)
            ionizer_run_count++;
        onWithTimedOff_arm_ms((u32)cfg_on_off.run_duration_s * 1000);
    }
#endif
}

void cmdOnOff_toggle(void) {
    cmdOnOff_set(!cfg_on_off_saved.onOff);
}

static s32 onWithTimedOff_timerCb(void *arg) {
    (void)arg;
    g_onWithTimedOffEvt = NULL;
    cmdOnOff_set(ZCL_ONOFF_STATUS_OFF);
    return -1;
}

#if USE_IONIZER
/* Single place that arms the bound on a run. Cancels first, so callers can
 * re-arm freely without leaking the previous timer's pool slot - which is
 * what would happen if cmdOnOff_set() armed a default and a caller then
 * overwrote g_onWithTimedOffEvt with its own. */
static void onWithTimedOff_arm_ms(u32 ms) {
    onWithTimedOff_cancelTimer();
    if (ms == 0)
        return;
    if (ms > (u32)ION_RUN_DURATION_MAX_S * 1000)
        ms = (u32)ION_RUN_DURATION_MAX_S * 1000;
    g_onWithTimedOffEvt = TL_ZB_TIMER_SCHEDULE(onWithTimedOff_timerCb, NULL, ms);
}
#endif

/**
 * @brief ZCL_CMD_ON_WITH_TIMED_OFF: turn on for pCmd->onTime (1/10 sec units),
 *        then automatically turn off. Receiving this again while already in
 *        a timed session restarts the countdown with the new onTime rather
 *        than stacking timers. onTime == 0 is treated as "stay on
 *        indefinitely" (same as a plain On command).
 *
 *        offWaitTime (the post-off lockout period during which further On
 *        commands should be ignored) is accepted but not yet enforced -
 *        can be added if actually needed.
 */
void cmdOnOff_onWithTimedOff(zcl_onoff_onWithTimeOffCmd_t *pCmd) {
    if (pCmd->onOffCtrl.bits.acceptOnlyWhenOn && !cfg_on_off_saved.onOff) {
        return;
    }

    cmdOnOff_set(ZCL_ONOFF_STATUS_ON);

#if USE_IONIZER
    /* An explicit onTime overrides the default cap cmdOnOff_set() just armed.
     * onTime == 0 means "stay on indefinitely" in ZCL, which this board must
     * never honour - leaving the default cap in place is the safe reading. */
    if (pCmd->onTime > 0) {
        onWithTimedOff_arm_ms((u32)pCmd->onTime * 100);
    }
#else
    if (pCmd->onTime > 0) {
        g_onWithTimedOffEvt = TL_ZB_TIMER_SCHEDULE(onWithTimedOff_timerCb, NULL, (u32)pCmd->onTime * 100);
    }
#endif
}

#if USE_IONIZER

uint32_t ionizer_run_count = 0;
uint32_t ionizer_elapsed_s = 0;

static ev_timer_event_t *g_ionizerTickEvt = NULL;

/* Start one bounded run. Deliberately routed through the same timed-off
 * timer the ZCL "On With Timed Off" command uses, so there is exactly one
 * mechanism that turns the HV module off again - whether the run was started
 * by the schedule, by Home Assistant, or by the button. A run can never
 * outlive its duration because a radio message went missing. */
/* One bounded run. Now just a plain "on" - cmdOnOff_set() arms the cap from
 * run_duration_s and counts the run, so the scheduler, the button and a
 * command from HA all go through identical code. A duration of 0 means the
 * feature is switched off, so refuse rather than latch the output on. */
void ionizer_run_start(void) {
    if (cfg_on_off.run_duration_s == 0)
        return;
    cmdOnOff_set(ZCL_ONOFF_STATUS_ON);
}

static s32 ionizer_tick_cb(void *arg) {
    (void)arg;

    /* Take a fresh reading rather than trusting measured_battery.average_mv:
     * the average is smoothed over up to 512 samples, so it lags badly when
     * the cell sags under HV load - exactly the case the abort below has to
     * catch. This also refreshes the reported ZCL attributes. */
    battery_detect(0);
    u16 mv = measured_battery.mv;

    /* Only act on a believable reading. Below the plausibility floor the
     * divider is faulty rather than the cell flat, and inhibiting on that
     * would silently disable the product. The pack's DW01 protects the cell
     * regardless, so running on is the safer failure mode - and the reported
     * voltage still exposes the fault. */
    bool bat_valid = (mv >= BATTERY_MIN_PLAUSIBLE_MV);

    /* Abort a cycle already in progress. This reading is under HV load, so
     * it uses the lower of the two thresholds. cmdOnOff_off() also cancels
     * the pending timed-off session, leaving no orphaned timer behind. */
    if (bat_valid && get_relay_status() && mv < BATTERY_ABORT_RUN_MV) {
        cmdOnOff_off();
        red_pulse(50);
        return ION_TICK_MS;
    }

    if (ionizer_elapsed_s < 0xFFFFFFFF - (ION_TICK_MS / 1000)) {
        ionizer_elapsed_s += ION_TICK_MS / 1000;
    }

    if (cfg_on_off.run_interval_s
        && ionizer_elapsed_s >= cfg_on_off.run_interval_s
        && cfg_on_off.run_duration_s
        && !get_relay_status()) {   // never stack a cycle on a running one

        ionizer_elapsed_s = 0;

        /* Don't start a cycle the cell can't sustain. Sampled between runs,
         * so this is an unloaded reading and uses the higher threshold.
         * Elapsed is reset either way, so it retries at the next interval
         * rather than re-firing every tick while voltage hovers at the line. */
        if (bat_valid && mv < BATTERY_MIN_RUN_MV) {
            red_pulse(50);
            return ION_TICK_MS;
        }
        ionizer_run_start();
    }

    return ION_TICK_MS;     // periodic
}

void ionizer_schedule_stop(void) {
    if (g_ionizerTickEvt) {
        TL_ZB_TIMER_CANCEL(&g_ionizerTickEvt);
    }
}

void ionizer_schedule_start(void) {
    ionizer_schedule_stop();
    /* The tick runs even with run_interval_s == 0 so that ZCL_ATTRID_RUN_ELAPSED
     * stays meaningful and a later write to the interval takes effect without
     * needing a reboot. The interval is re-read every tick, so changing it
     * over the air applies from the next tick. */
    g_ionizerTickEvt = TL_ZB_TIMER_SCHEDULE(ionizer_tick_cb, NULL, ION_TICK_MS);
}

#endif // USE_IONIZER

void remoteCmdOnOff(uint8_t cmd) {
    epInfo_t dstEpInfo;
    TL_SETSTRUCTCONTENT(dstEpInfo, 0);

    dstEpInfo.profileId = HA_PROFILE_ID;

    dstEpInfo.dstAddrMode = APS_DSTADDR_EP_NOTPRESETNT;

    /* command 0x00 - off, 0x01 - on, 0x02 - toggle */

    switch(cmd) {
        case ZCL_CMD_ONOFF_OFF:
            zcl_onOff_offCmd(APP_ENDPOINT1, &dstEpInfo, FALSE);
            break;
        case ZCL_CMD_ONOFF_ON:
            zcl_onOff_onCmd(APP_ENDPOINT1, &dstEpInfo, FALSE);
            break;
        case ZCL_CMD_ONOFF_TOGGLE:
            zcl_onOff_toggleCmd(APP_ENDPOINT1, &dstEpInfo, FALSE);
            break;
        default:
            break;
    }
}
