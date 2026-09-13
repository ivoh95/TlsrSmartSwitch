/************************************************************************************
 * @file    zb_appCb.c
 *
 * @brief   This is the source file for zb_appCb
 *
 * @author  Zigbee Group
 * @date    2021
 *
 * @par     Copyright (c) 2021, Telink Semiconductor (Shanghai) Co., Ltd. ("TELINK")
 *			All rights reserved.
 *
 *          Licensed under the Apache License, Version 2.0 (the "License");
 *          you may not use this file except in compliance with the License.
 *          You may obtain a copy of the License at
 *
 *              http://www.apache.org/licenses/LICENSE-2.0
 *
 *          Unless required by applicable law or agreed to in writing, software
 *          distributed under the License is distributed on an "AS IS" BASIS,
 *          WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *          See the License for the specific language governing permissions and
 *          limitations under the License.
 *
 ***********************************************************************************/
/**********************************************************************
 * INCLUDES
 */
#include "app_main.h"
#include "energy_save.h"

/**********************************************************************
 * LOCAL CONSTANTS
 */
#define DEBUG_HEART     0

/**********************************************************************
 * TYPEDEFS
 */


/**********************************************************************
 * LOCAL FUNCTIONS
 */
void zb_bdbInitCb(uint8_t status, uint8_t joinedNetwork);
void zb_bdbCommissioningCb(uint8_t status, void *arg);
void zb_bdbIdentifyCb(uint8_t endpoint, uint16_t srcAddr, uint16_t identifyTime);

/**********************************************************************
 * GLOBAL VARIABLES
 */
bdb_appCb_t g_zbBdbCb = {
    zb_bdbInitCb,
    zb_bdbCommissioningCb,
    zb_bdbIdentifyCb,
    NULL
};

#ifdef ZCL_OTA
ota_callBack_t app_otaCb = {
    app_otaProcessMsgHandler,
};
#endif

/**********************************************************************
 * LOCAL VARIABLES
 */
u32 heartInterval = 0;

#if DEBUG_HEART
ev_timer_event_t *heartTimerEvt = NULL;
#endif
ev_timer_event_t *steerTimerEvt = NULL;
ev_timer_event_t *rejoinBackoffTimerEvt = NULL;

/**********************************************************************
 * FUNCTIONS
 */
#if DEBUG_HEART
static s32 heartTimerCb(void *arg)
{
    if (heartInterval == 0) {
        heartTimerEvt = NULL;
        return -1;
    }

    gpio_toggle(LED_POWER);

    return heartInterval;
}
#endif

s32 app_bdbNetworkSteerStart(void *arg)
{
    bdb_networkSteerStart();

    steerTimerEvt = NULL;
    return -1;
}

/* Persistent rejoin retry for a device that has lost contact with its parent
 * (e.g. the parent router lost power while the coordinator stayed up). Without
 * this the stack reports the loss once and nothing ever re-attempts the join,
 * leaving the device stranded off the network until it is power-cycled.
 *
 * Fires on a repeating 60s backoff and alternates secured/unsecured rejoin, so
 * a network-key rotation that happened while we were off the air can't lock us
 * out. A rejoin scans the whole channel mask and attaches to any router or the
 * coordinator that answers - it does not need the original parent. Self-cancels
 * once we are back on a network (the commissioning SUCCESS case cancels the
 * timer) or if the device was factory-reset. */
static s32 app_rejoinBackoff(void *arg)
{
    (void)arg;
    static bool rejoinMode = REJOIN_SECURITY;

    if (zb_isDeviceFactoryNew()) {
        rejoinBackoffTimerEvt = NULL;
        return -1;
    }

    zb_rejoinSecModeSet(rejoinMode);
    zb_rejoinReq(zb_apsChannelMaskGet(), g_bdbAttrs.scanDuration);

    rejoinMode = !rejoinMode;

    return 0;   // 0 = re-arm at the same 60s interval
}

#if FIND_AND_BIND_SUPPORT
s32 app_bdbFindAndBindStart(void *arg)
{
    bdb_findAndBindStart(BDB_COMMISSIONING_ROLE_TARGET);

    return -1;
}
#endif

/*********************************************************************
 * @fn      zb_bdbInitCb
 *
 * @brief   application callback for bdb initiation
 *
 * @param   status - the status of bdb init BDB_INIT_STATUS_SUCCESS or BDB_INIT_STATUS_FAILURE
 *
 * @param   joinedNetwork  - 1: node is on a network, 0: node isn't on a network
 *
 * @return  None
 */
void zb_bdbInitCb(uint8_t status, uint8_t joinedNetwork)
{
    //printf("bdbInitCb: sta = %x, joined = %x\n", status, joinedNetwork);

    if (status == BDB_INIT_STATUS_SUCCESS) {
        /*
         * start bdb commissioning
         * */
        if (joinedNetwork) {
            heartInterval = 1000;

            g_appCtx.net_steer_start = false;

#ifdef ZCL_OTA
            ota_queryStart(OTA_PERIODIC_QUERY_INTERVAL);
#endif
            app_zclCheckInStart();
#if PM_ENABLE
            app_pmSleepSettleStart();
#endif
        } else {
            heartInterval = 500;

#if (!ZBHCI_EN)
            uint16_t jitter = 0;
            do {
                jitter = zb_random() % 0x0fff;
            } while (jitter == 0);

            if (steerTimerEvt) {
                TL_ZB_TIMER_CANCEL(&steerTimerEvt);
            }
            steerTimerEvt = TL_ZB_TIMER_SCHEDULE(app_bdbNetworkSteerStart, NULL, jitter);
#endif
        }
    } else {
        heartInterval = 200;

        /* Init failed but we are provisioned for a network (non-factory-new):
         * we booted while unable to reach it - e.g. a cold boot during a parent
         * outage. Start the persistent rejoin backoff so we recover once any
         * parent is reachable again, instead of sitting idle off the network. */
        if (joinedNetwork && !rejoinBackoffTimerEvt) {
            rejoinBackoffTimerEvt = TL_ZB_TIMER_SCHEDULE(app_rejoinBackoff, NULL, 60 * 1000);
        }
    }

#if DEBUG_HEART
    if (heartTimerEvt) {
        TL_ZB_TIMER_CANCEL(&heartTimerEvt);
    }
    heartTimerEvt = TL_ZB_TIMER_SCHEDULE(heartTimerCb, NULL, heartInterval);
#endif
}

/*********************************************************************
 * @fn      zb_bdbCommissioningCb
 *
 * @brief   application callback for bdb commissioning
 *
 * @param   status - the status of bdb commissioning
 *
 * @param   arg
 *
 * @return  None
 */
void zb_bdbCommissioningCb(uint8_t status, void *arg)
{
    //printf("bdbCommCb: sta = %x\n", status);

    switch (status) {
    case BDB_COMMISSION_STA_SUCCESS:
        heartInterval = 1000;

        g_appCtx.net_steer_start = false;

        light_blink_start(2, 200, 200);

        if (steerTimerEvt) {
            TL_ZB_TIMER_CANCEL(&steerTimerEvt);
        }

        /* Back on a network - stop any parent-loss rejoin retries. */
        if (rejoinBackoffTimerEvt) {
            TL_ZB_TIMER_CANCEL(&rejoinBackoffTimerEvt);
        }

#ifdef ZCL_OTA
        ota_queryStart(OTA_PERIODIC_QUERY_INTERVAL);
#endif
        app_zclCheckInStart();
#if PM_ENABLE
        app_pmSleepSettleStart();
#endif

#if FIND_AND_BIND_SUPPORT
        if (!gLightCtx.bdbFindBindFlg) {
            gLightCtx.bdbFindBindFlg = TRUE;
            TL_ZB_TIMER_SCHEDULE(app_bdbFindAndBindStart, NULL, 1000);
        }
#endif
        break;
    case BDB_COMMISSION_STA_IN_PROGRESS:
        break;
    case BDB_COMMISSION_STA_NOT_AA_CAPABLE:
        break;
    case BDB_COMMISSION_STA_NO_NETWORK:
    case BDB_COMMISSION_STA_TCLK_EX_FAILURE:
    case BDB_COMMISSION_STA_TARGET_FAILURE:
        {
            uint16_t jitter = 0;
            do {
                jitter = zb_random() % 0x2710;
            } while (jitter < 5000);

            if (steerTimerEvt) {
                TL_ZB_TIMER_CANCEL(&steerTimerEvt);
            }
            steerTimerEvt = TL_ZB_TIMER_SCHEDULE(app_bdbNetworkSteerStart, NULL, jitter);
        }
        break;
    case BDB_COMMISSION_STA_FORMATION_FAILURE:
        break;
    case BDB_COMMISSION_STA_NO_IDENTIFY_QUERY_RESPONSE:
        break;
    case BDB_COMMISSION_STA_BINDING_TABLE_FULL:
        break;
    case BDB_COMMISSION_STA_NO_SCAN_RESPONSE:
        break;
    case BDB_COMMISSION_STA_NOT_PERMITTED:
        break;
    case BDB_COMMISSION_STA_PARENT_LOST:
        /* Stack has determined the parent is gone (e.g. the parent router lost
         * power). Try an immediate secured rejoin - it scans the channel mask
         * and attaches to any router/coordinator that answers, keeping the
         * network key. If that first attempt fails we fall through to the
         * REJOIN_FAILURE backoff below on the next callback. Previously this
         * case was unhandled (default: no-op), so a device that lost its parent
         * never rejoined until it was power-cycled. */
        zb_rejoinSecModeSet(REJOIN_SECURITY);
        zb_rejoinReq(zb_apsChannelMaskGet(), g_bdbAttrs.scanDuration);
        break;
    case BDB_COMMISSION_STA_REJOIN_FAILURE:
        /* Keep retrying on a 60s backoff instead of giving up after one shot.
         * app_rejoinBackoff alternates secured/unsecured and self-cancels once
         * we rejoin (or if the device is factory-reset). */
        if (!rejoinBackoffTimerEvt) {
            rejoinBackoffTimerEvt = TL_ZB_TIMER_SCHEDULE(app_rejoinBackoff, NULL, 60 * 1000);
        }
        break;
    case BDB_COMMISSION_STA_FORMATION_DONE:
#ifndef ZBHCI_EN
        tl_zbMacChannelSet(DEFAULT_CHANNEL);  //set default channel
#endif
        break;
    default:
        break;
    }
}

void zb_bdbIdentifyCb(uint8_t endpoint, uint16_t srcAddr, uint16_t identifyTime)
{
#if FIND_AND_BIND_SUPPORT
    extern void app_zclIdentifyCmdHandler(uint8_t endpoint, uint16_t srcAddr, uint16_t identifyTime);
    app_zclIdentifyCmdHandler(endpoint, srcAddr, identifyTime);
#endif
}

#ifdef ZCL_OTA
void app_otaProcessMsgHandler(uint8_t evt, uint8_t status)
{
    if (evt == OTA_EVT_START) {
        if (status == ZCL_STA_SUCCESS) {

        } else {

        }
    } else if (evt == OTA_EVT_COMPLETE) {
        if (status == ZCL_STA_SUCCESS) {
            ota_mcuReboot();
        } else {
            ota_queryStart(OTA_PERIODIC_QUERY_INTERVAL);
        }
    }
}
#endif

s32 app_softReset(void *arg)
{
    SYSTEM_RESET();

    return -1;
}

/*********************************************************************
 * @fn      app_leaveCnfHandler
 *
 * @brief   Handler for ZDO Leave Confirm message.
 *
 * @param   pRsp - parameter of leave confirm
 *
 * @return  None
 */
void app_leaveCnfHandler(nlme_leave_cnf_t *pLeaveCnf)
{

//    printf("app_leaveCnfHandler\r\n");

    if(pLeaveCnf->status == SUCCESS) {

        //relay_settints_default();
#if USE_METERING
        energy_remove();
#endif
        zb_deviceFactoryNewSet(true);

        heartInterval = 500;

#if (!ZBHCI_EN)
        uint16_t jitter = 0;
        do {
            jitter = zb_random() % 0x0fff;
        } while (jitter == 0);

        if (steerTimerEvt) {
            TL_ZB_TIMER_CANCEL(&steerTimerEvt);
        }
        steerTimerEvt = TL_ZB_TIMER_SCHEDULE(app_bdbNetworkSteerStart, NULL, jitter);
#endif

        if (!g_appCtx.net_steer_start) light_blink_start(90, 250, 750);
    }
}

/*********************************************************************
 * @fn      app_leaveIndHandler
 *
 * @brief   Handler for ZDO leave indication message.
 *
 * @param   pInd - parameter of leave indication
 *
 * @return  None
 */
void app_leaveIndHandler(nlme_leave_ind_t *pLeaveInd)
{

}

bool app_nwkUpdateIndicateHandler(nwkCmd_nwkUpdate_t *pNwkUpdate){
    return FAILURE;
}

