#include "app_main.h"
#include "sensors.h"
#include "battery.h"
#if USE_SENSOR_MY18B20
#include "my18b20.h"
#endif

/* Diagnostic-only: build with -DDIAG_DISABLE_PM_SLEEP=1 to skip the
 * drv_pm_lowPowerEnter() call in app_task() and isolate whether deep sleep
 * is interfering with initial network join. Remove once resolved. */
#ifndef DIAG_DISABLE_PM_SLEEP
#define DIAG_DISABLE_PM_SLEEP 0
#endif

//uint8_t resp_time = false;

app_ctx_t g_appCtx = {
        .timerFactoryReset = NULL,
        .timerLedEvt = NULL,
};

#ifdef ZCL_OTA
extern ota_callBack_t app_otaCb;

//running code firmware information
ota_preamble_t app_otaInfo = {
	.fileVer 			= FILE_VERSION,
	.imageType 			= IMAGE_TYPE,
	.manufacturerCode 	= MANUFACTURER_CODE_TELINK,
};
#endif

//Must declare the application call back function which used by ZDO layer
const zdo_appIndCb_t appCbLst = {
    bdb_zdoStartDevCnf,//start device cnf cb
    NULL,//reset cnf cb
    NULL,//device announce indication cb
    app_leaveIndHandler,//leave ind cb
    app_leaveCnfHandler,//leave cnf cb
    app_nwkUpdateIndicateHandler,//nwk update ind cb
    NULL,//permit join ind cb
    NULL,//nlme sync cnf cb
    NULL,//tc join ind cb
    NULL,//tc detects that the frame counter is near limit
};


/**
 *  @brief Definition for bdb commissioning setting
 */
bdb_commissionSetting_t g_bdbCommissionSetting = {
    .linkKey.tcLinkKey.keyType = SS_GLOBAL_LINK_KEY,
    .linkKey.tcLinkKey.key = (uint8_t *)tcLinkKeyCentralDefault,             //can use unique link key stored in NV

    .linkKey.distributeLinkKey.keyType = MASTER_KEY,
    .linkKey.distributeLinkKey.key = (uint8_t *)linkKeyDistributedMaster,    //use linkKeyDistributedCertification before testing

    .linkKey.touchLinkKey.keyType = MASTER_KEY,
    .linkKey.touchLinkKey.key = (uint8_t *)touchLinkKeyMaster,               //use touchLinkKeyCertification before testing

#if TOUCHLINK_SUPPORT
    .touchlinkEnable = 1,                                               /* enable touch-link */
#else
    .touchlinkEnable = 0,                                               /* disable touch-link */
#endif
    .touchlinkChannel = DEFAULT_CHANNEL,                                /* touch-link default operation channel for target */
    .touchlinkLqiThreshold = 0xA0,                                      /* threshold for touch-link scan req/resp command */
};

/*********************************************************************
*/
#if USE_NV_APP
// Test for compatible version of saved settings formats
void test_nv_version(void) {
	u32 ver = 0;
	if(nv_flashReadNew(1, NV_MODULE_APP, NV_ITEM_APP_DEV_VER, sizeof(ver), (u8 *)&ver) == NV_SUCC
		&& (ver & 0xFFFF) == (USE_NV_APP & 0xFFFF)
		&& ver >= USE_NV_APP_OK // compatible ?
		) {

	} else {
		ver = USE_NV_APP;
		nv_resetAll();
		nv_resetModule(NV_MODULE_APP);
		// energy_remove(); ?
		nv_flashWriteNew(1, NV_MODULE_APP, NV_ITEM_APP_DEV_VER, sizeof(ver), (u8 *)&ver);
		// SYSTEM_RESET();
	}
}
#endif
/*********************************************************************
 * @fn      stack_init
 *
 * @brief   This function initialize the ZigBee stack and related profile. If HA/ZLL profile is
 *          enabled in this application, related cluster should be registered here.
 *
 * @param   None
 *
 * @return  None
 */
void stack_init(void)
{
	/* Initialize ZB stack */
	zb_init();

	/* Register stack CB */
    zb_zdoCbRegister((zdo_appIndCb_t *)&appCbLst);
}

/*********************************************************************
 * @fn      user_app_init
 *
 * @brief   This function initialize the application(Endpoint) information for this node.
 *
 * @param   None
 *
 * @return  None
 */
void user_app_init(void)
{
	af_nodeDescManuCodeUpdate(MANUFACTURER_CODE_TELINK);

#ifdef ZCL_POLL_CTRL
	/* Advertise that our radio isn't always listening, so the coordinator
	 * knows to expect Poll Control check-ins rather than instant delivery. */
	af_powerDescPowerModeUpdate(POWER_MODE_RECEIVER_COMES_PERIODICALLY);
#endif

	populate_date_code();

    /* Initialize ZCL layer */
	/* Register Incoming ZCL Foundation command/response messages */
    zcl_init(app_zclProcessIncomingMsg);

	/* Register endPoint */
    af_endpointRegister(APP_ENDPOINT1, (af_simple_descriptor_t *)&app_ep1_simpleDesc, zcl_rx_handler, NULL);
//    af_endpointRegister(APP_ENDPOINT2, (af_simple_descriptor_t *)&app_ep2_simpleDesc, zcl_rx_handler, NULL);

	zcl_reportingTabInit();

	load_config_on_off();
#if USE_SENSOR_MY18B20
    init_my18b20();
#endif
#if USE_METERING
    app_sensor_init();
#endif
    dev_relay_init();


	/* Register ZCL specific cluster information */
    zcl_register(APP_ENDPOINT1, APP_CB_CLUSTER_NUM1, (zcl_specClusterInfo_t *)g_appClusterList1);

#if ZCL_GP_SUPPORT
	/* Initialize GP */
	gp_init(APP_ENDPOINT1);
#endif

#if ZCL_OTA_SUPPORT
	/* Initialize OTA */
    ota_init(OTA_TYPE_CLIENT, (af_simple_descriptor_t *)&app_ep1_simpleDesc, &app_otaInfo, &app_otaCb);
#endif

#if ZCL_WWAH_SUPPORT
    /* Initialize WWAH server */
    wwah_init(WWAH_TYPE_SERVER, (af_simple_descriptor_t *)&app_simpleDesc);
#endif
}

#if PM_ENABLE
/* Settling period after joining a network, during which sleep is held off.
 * BDB_STATE_GET() reports idle as soon as the network join itself completes,
 * but the coordinator's separate interview (ZDO/ZCL attribute reads) happens
 * afterwards and is time-sensitive - give it a wide margin before the device
 * starts actually sleeping between MAC polls. */
#define PM_SLEEP_SETTLE_MS   60000

static bool g_pmSleepAllowed = false;
static ev_timer_event_t *g_pmSleepSettleTimerEvt = NULL;

static s32 app_pmSleepSettleCb(void *arg)
{
    (void)arg;
    g_pmSleepAllowed = true;
    g_pmSleepSettleTimerEvt = NULL;
    return -1;
}

void app_pmSleepSettleStart(void)
{
    g_pmSleepAllowed = false;
    /* This can be called again (e.g. a silent internal rejoin) before the
     * previous timer fires - cancel it first so we don't leak a slot from
     * the shared, finite ev_timer pool. */
    if (g_pmSleepSettleTimerEvt) {
        TL_ZB_TIMER_CANCEL(&g_pmSleepSettleTimerEvt);
    }
    g_pmSleepSettleTimerEvt = TL_ZB_TIMER_SCHEDULE(app_pmSleepSettleCb, NULL, PM_SLEEP_SETTLE_MS);
}
#endif

void app_task(void) {
	if(dev_gpios.led2) {
		gpio_write(dev_gpios.led2,
				(dev_gpios.flg & GPIOS_FLG_LED2_POL)? cfg_on_off.onOff : !cfg_on_off.onOff);
	}
    button_handler();
#if USE_BL0942
    monitoring_handler();
#endif
#if USE_SENSOR_MY18B20
	task_my18b20();
#endif
#if USE_SWITCH
    switch_handler();
    bool isIdle = bdb_isIdle() && !switch_idle();
#else
    bool isIdle = bdb_isIdle() && !button_idle();
#endif
    if (isIdle)
		report_handler();
#if PM_ENABLE && !DIAG_DISABLE_PM_SLEEP
    /* Only sleep once the stack/app are quiescent - drv_pm_lowPowerEnter()
     * itself re-checks tl_stackBusy()/zb_isTaskDone() and picks the sleep
     * duration from the nearest pending ev_timer (e.g. the poll-control
     * check-in timer or the MAC poll timer), so it wakes up exactly when
     * needed. Also require an established network - don't sleep during the
     * initial steering/join jitter window, before any parent relationship
     * exists yet.
     *
     * Also skip sleep entirely while the output is on: deep-sleep-with-
     * retention on this chip does not keep the relay GPIO actively driven
     * through the sleep portion of each cycle (only during the brief wake
     * window), so a steady "on" output would otherwise blink instead of
     * staying lit. There's no real power cost to staying awake here - the
     * load's own current while active already dwarfs anything saved by
     * sleeping the MCU during that window. */
    if (isIdle && zb_isDeviceJoinedNwk() && g_pmSleepAllowed && !cfg_on_off.onOff)
        drv_pm_lowPowerEnter();
#endif
}

static void app_sysException(void) {

#if UART_PRINTF_MODE
    printf("app_sysException, line: %d, event: %d, reset\r\n", T_evtExcept[0], T_evtExcept[1]);
#endif

#if 1
    SYSTEM_RESET();
#else
    led_on(LED_STATUS);
    while(1);
#endif
}

#if USE_BL0937
#define REPORT_TIME_MIN_DEF			(8*2)	// 16 sec
#else
#define REPORT_TIME_MIN_DEF			10		// 10 sec
#endif
#define REPORT_TIME_MAX_DEF			600		// 10 min
#define REPORT_TIME_STAT_DEF		3600	// 1 h
#define REPORT_TIME_MAX				65000

/*********************************************************************
 * @fn      user_init
 *
 * @brief   User level initialization code.
 *
 * @param   isRetention - if it is waking up with ram retention.
 *
 * @return  None
 */
//__attribute__((optimize("-Os")))
void user_init(bool isRetention)
{
#ifdef ZCL_METERING
	uint64_t reportableChange_u64;
#endif
	int32_t reportableChange_tmp;

#if USE_NV_APP
    if(!isRetention)
    	test_nv_version();
#endif

    /* Initialize GPIO led, key, relay, switch, ... */
    dev_gpios_init();

#if PM_ENABLE
    /* Register the local button (and switch input, when present) as PM
     * wakeup sources, so a local press wakes the device out of deep sleep
     * instead of waiting for the next scheduled poll/check-in. Both pins
     * are configured with a pull-up (see dev_gpios_init()), so they idle
     * high and are pulled low when actuated. */
    {
        drv_pm_pinCfg_t keyWakeupCfg[] = { { dev_gpios.key, PM_WAKEUP_LEVEL_LOW } };
        drv_pm_wakeupPinConfig(keyWakeupCfg, 1);
    }
#if USE_SWITCH
#if USE_SENSOR_MY18B20
    /* sw1 is repurposed as the 1-wire bus in this configuration - it must
     * not be treated as a switch wakeup source. */
    if (dev_gpios.sw1 != dev_gpios.swire)
#endif
    {
        drv_pm_pinCfg_t swWakeupCfg[] = { { dev_gpios.sw1, PM_WAKEUP_LEVEL_LOW } };
        drv_pm_wakeupPinConfig(swWakeupCfg, 1);
    }
#endif
#endif

#if ZBHCI_EN
    zbhciInit();
#endif

    /* Everything below manages live, stateful SDK data (network/join state,
     * endpoint & cluster tables, reporting config) that survives a
     * PM_SLEEP_MODE_DEEP_WITH_RETENTION sleep intact in retained SRAM - on a
     * retention wake the CPU still restarts through this whole boot path,
     * but re-running this would reset/corrupt that live state. Only run it
     * on a genuine cold boot; a retention wake just needs the RF hardware
     * (reset by the sleep itself) reconfigured. */
    if (!isRetention) {
        /* Initialize Stack */
        stack_init();

        /* Initialize user application */
        user_app_init();

        /* Register except handler for test */
        sys_exceptHandlerRegister(app_sysException);

        /* User's Task */
#if ZBHCI_EN
        ev_on_poll(EV_POLL_HCI, zbhciTask);
#endif
        ev_on_poll(EV_POLL_IDLE, app_task);

        /* Read the pre-install code from NV */
        if(bdb_preInstallCodeLoad(&g_appCtx.tcLinkKey.keyType, g_appCtx.tcLinkKey.key) == RET_OK){
            g_bdbCommissionSetting.linkKey.tcLinkKey.keyType = g_appCtx.tcLinkKey.keyType;
            g_bdbCommissionSetting.linkKey.tcLinkKey.key = g_appCtx.tcLinkKey.key;
        }

        /* Set default reporting configuration */
        reportableChange_tmp = 1;
#ifdef ZCL_ON_OFF
    /* OnOff */
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_GEN_ON_OFF, ZCL_ATTRID_ONOFF,
            0, REPORT_TIME_MAX, (uint8_t *)&reportableChange_tmp);
#if USE_SWITCH
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_GEN_ON_OFF, ZCL_ATTRID_RELAY_STATE,
            0, REPORT_TIME_MAX, (uint8_t *)&reportableChange_tmp);
#endif
#endif
#ifdef ZCL_ON_OFF_SWITCH_CFG
    /* OnOffCfg */
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_GEN_ON_OFF_SWITCH_CONFIG, CUSTOM_ATTRID_DECOUPLED,
            0, REPORT_TIME_MAX, (uint8_t *)&reportableChange_tmp);
#endif
#ifdef  ZCL_MULTISTATE_INPUT
    /* MultistateInput */
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_GEN_MULTISTATE_INPUT_BASIC,
            ZCL_MULTISTATE_INPUT_ATTRID_PRESENT_VALUE, 0, REPORT_TIME_STAT_DEF, (uint8_t *)&reportableChange_tmp);
#endif
#ifdef ZCL_METERING
    //reportableChange_tmp = 1;
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_SE_METERING,
    		ZCL_ATTRID_STATUS, 0, REPORT_TIME_MAX, (uint8_t *)&reportableChange_u64);
    /* Energy */
    reportableChange_u64 = 1000; // 1Wh
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_SE_METERING,
            ZCL_ATTRID_CURRENT_SUMMATION_DELIVERD, REPORT_TIME_MIN_DEF, REPORT_TIME_STAT_DEF, (uint8_t *)&reportableChange_u64);
#endif
#ifdef ZCL_THERMOSTAT
    reportableChange_tmp = 10; // 0.1C
	bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_HAVC_THERMOSTAT,
			ZCL_ATTRID_HVAC_THERMOSTAT_LOCAL_TEMPERATURE, 10, 6000, (u8 *)&reportableChange_tmp);
    reportableChange_tmp = 1;
	bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_HAVC_THERMOSTAT,
			ZCL_ATTRID_HVAC_THERMOSTAT_PI_COOLING_DEMAND, 0, REPORT_TIME_STAT_DEF, (u8 *)&reportableChange_tmp);
	bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_HAVC_THERMOSTAT,
			ZCL_ATTRID_HVAC_THERMOSTAT_PI_HEATING_DEMAND, 0, REPORT_TIME_STAT_DEF, (u8 *)&reportableChange_tmp);
#endif
#ifdef ZCL_ELECTRICAL_MEASUREMENT
    /* Voltage */
    reportableChange_tmp = 100; // 1V
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT,
            ZCL_ATTRID_RMS_VOLTAGE, REPORT_TIME_MIN_DEF, REPORT_TIME_MAX_DEF, (uint8_t *)&reportableChange_tmp);
    /* Current */
    reportableChange_tmp = 5; // 5 mA
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT,
            ZCL_ATTRID_RMS_CURRENT, REPORT_TIME_MIN_DEF, REPORT_TIME_MAX_DEF, (uint8_t *)&reportableChange_tmp);
    /* Power */
    reportableChange_tmp = 50; // 0.05W, 0.5W, 5W
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT,
            ZCL_ATTRID_ACTIVE_POWER, REPORT_TIME_MIN_DEF, REPORT_TIME_MAX_DEF, (uint8_t *)&reportableChange_tmp);
    /* Power divisor */
    reportableChange_tmp = 1; // [1, 10, 100, 1000]
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT,
    		ZCL_ATTRID_AC_POWER_DIVISOR, 0, REPORT_TIME_STAT_DEF, (uint8_t *)&reportableChange_tmp);
#if USE_BL0942
    /* Freq */
    reportableChange_tmp = 10; // 0.1Hz
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT,
            ZCL_ATTRID_AC_FREQUENCY, REPORT_TIME_MIN_DEF, REPORT_TIME_MAX_DEF, (uint8_t *)&reportableChange_tmp);
#endif
    /* Alarm */
    reportableChange_tmp = 1;
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT,
    		ZCL_ATTRID_ALARM_EVENTS, 0, REPORT_TIME_STAT_DEF, (uint8_t *)&reportableChange_tmp);
#endif // ZCL_ELECTRICAL_MEASUREMENT

#ifdef ZCL_TEMPERATURE_MEASUREMENT
    reportableChange_tmp = 10; // 0.1C
	bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_MS_TEMPERATURE_MEASUREMENT,
		ZCL_TEMPERATURE_MEASUREMENT_ATTRID_MEASUREDVALUE, 10, 6000, (u8 *)&reportableChange_tmp);
#endif

        /* Initialize BDB */
        bdb_init((af_simple_descriptor_t *)&app_ep1_simpleDesc, &g_bdbCommissionSetting, &g_zbBdbCb, 1);

        rf_setTxPower(ZB_TX_POWER_IDX_DEF);
    } else {
        /* Re-config phy when system recovery from deep sleep with retention */
        mac_phyReconfig();
    }
}

#ifdef ZCL_POLL_CTRL
static ev_timer_event_t *g_pollCtrl_checkInTimerEvt = NULL;
static ev_timer_event_t *g_pollCtrl_fastPollTimerEvt = NULL;

/**
 * @brief Periodic timer callback: sends the Poll Control Check-In command
 *        to the coordinator every g_pollCtrl_checkInInterval (QS). Reads
 *        the interval fresh on every firing (via the return value) so a
 *        remote write to CheckInInterval takes effect on the next tick.
 *        Returning 0 disables the check-in until re-armed by
 *        app_zclCheckInStart().
 */
static s32 app_pollCtrl_checkInTimerCb(void *arg)
{
    (void)arg;

    if (g_pollCtrl_checkInInterval == 0) {
        g_pollCtrl_checkInTimerEvt = NULL;
        return -1;
    }

    if (zb_isDeviceJoinedNwk()) {
        epInfo_t dstEpInfo;
        TL_SETSTRUCTCONTENT(dstEpInfo, 0);
        dstEpInfo.profileId = HA_PROFILE_ID;
        dstEpInfo.dstAddrMode = APS_SHORT_DSTADDR_WITHEP;
        dstEpInfo.dstAddr.shortAddr = 0x0000; /* Coordinator / Trust Center */
        dstEpInfo.dstEp = 1;

        zcl_pollCtrl_checkInCmd(APP_ENDPOINT1, &dstEpInfo, TRUE);
    }

    /* Refresh battery so it's current if the coordinator opens a fast-poll
     * window and reads it right after this check-in. */
    battery_detect(0);

    return (s32)(g_pollCtrl_checkInInterval * 250);
}

/**
 * @brief One-shot timer ending a fast-poll window opened by a Check-In
 *        Response (or by our own default fastPollTimeout), restoring the
 *        normal long-poll rate.
 */
static s32 app_pollCtrl_fastPollTimeoutCb(void *arg)
{
    (void)arg;

    zb_setPollRate(g_pollCtrl_longPollInterval * 250);
    g_pollCtrl_fastPollTimerEvt = NULL;
    return -1;
}
#endif /* ZCL_POLL_CTRL */

/**
 * @brief Bring up Poll Control: set the baseline (long) MAC poll rate and
 *        (re)start the periodic Check-In timer. Called once after the
 *        device joins the network (see zb_appCb.c) and again whenever the
 *        coordinator writes the CheckInInterval attribute (see
 *        zcl_appCb.c), so re-enabling a previously-disabled interval takes
 *        effect immediately instead of waiting for the next reboot.
 */
void app_zclCheckInStart(void)
{
    battery_detect(0);

#ifdef ZCL_POLL_CTRL
    zb_setPollRate(g_pollCtrl_longPollInterval * 250);

    if (g_pollCtrl_checkInInterval && !g_pollCtrl_checkInTimerEvt) {
        g_pollCtrl_checkInTimerEvt = TL_ZB_TIMER_SCHEDULE(app_pollCtrl_checkInTimerCb, NULL,
                g_pollCtrl_checkInInterval * 250);
    }
#endif
}

#ifdef ZCL_POLL_CTRL
/**
 * @brief ZCL Poll Control cluster server-side command callback - handles
 *        client-generated commands received from the coordinator.
 *
 *  ZCL_CMD_CHK_IN_RSP      - if the coordinator requests fast polling, switch
 *                             to the short poll rate for fastPollTimeout (or
 *                             our own default if it sent 0).
 *  ZCL_CMD_FAST_POLL_STOP  - end any fast-poll window early and restore the
 *                             long poll rate.
 *  SET_LONG_POLL_INTERVAL  - validate against the minimum and, if we are not
 *                             currently in a fast-poll window, apply it now.
 *  SET_SHORT_POLL_INTERVAL - store it; applied next time a fast-poll window
 *                             opens.
 */
status_t app_pollCtrlCb(zclIncomingAddrInfo_t *pAddrInfo, uint8_t cmdId, void *cmdPayload)
{
    (void)pAddrInfo;

    switch (cmdId) {
        case ZCL_CMD_CHK_IN_RSP: {
            zcl_chkInRsp_t *p = (zcl_chkInRsp_t *)cmdPayload;
            if (p && p->startFastPolling) {
                uint16_t timeout = p->fastPollTimeout ? p->fastPollTimeout : g_pollCtrl_fastPollTimeout;

                zb_setPollRate(g_pollCtrl_shortPollInterval * 250);

                if (g_pollCtrl_fastPollTimerEvt) {
                    TL_ZB_TIMER_CANCEL(&g_pollCtrl_fastPollTimerEvt);
                }
                g_pollCtrl_fastPollTimerEvt = TL_ZB_TIMER_SCHEDULE(app_pollCtrl_fastPollTimeoutCb, NULL,
                        (u32)timeout * 250);
            }
            break;
        }
        case ZCL_CMD_FAST_POLL_STOP:
            if (g_pollCtrl_fastPollTimerEvt) {
                TL_ZB_TIMER_CANCEL(&g_pollCtrl_fastPollTimerEvt);
            }
            zb_setPollRate(g_pollCtrl_longPollInterval * 250);
            break;
        case ZCL_CMD_SET_LONG_POLL_INTERVAL: {
            zcl_setLongPollInterval_t *p = (zcl_setLongPollInterval_t *)cmdPayload;
            if (p) {
                if (p->newLongPollInterval < g_pollCtrl_longPollIntervalMin)
                    return ZCL_STA_INVALID_VALUE;
                g_pollCtrl_longPollInterval = p->newLongPollInterval;
                if (!g_pollCtrl_fastPollTimerEvt)
                    zb_setPollRate(g_pollCtrl_longPollInterval * 250);
            }
            break;
        }
        case ZCL_CMD_SET_SHORT_POLL_INTERVAL: {
            zcl_setShortPollInterval_t *p = (zcl_setShortPollInterval_t *)cmdPayload;
            if (p) {
                g_pollCtrl_shortPollInterval = p->newShortPollInterval;
            }
            break;
        }
        default:
            return ZCL_STA_UNSUP_CLUSTER_COMMAND;
    }
    return ZCL_STA_SUCCESS;
}
#endif /* ZCL_POLL_CTRL */
