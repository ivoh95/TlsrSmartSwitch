#include "app_main.h"
#if USE_SENSOR_MY18B20
#include "my18b20.h"
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

void cmdOnOff_set(bool status) {
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

    if (pCmd->onTime > 0) {
        g_onWithTimedOffEvt = TL_ZB_TIMER_SCHEDULE(onWithTimedOff_timerCb, NULL, (u32)pCmd->onTime * 100);
    }
}

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
