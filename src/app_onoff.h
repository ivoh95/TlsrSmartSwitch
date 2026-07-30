#ifndef SRC_INCLUDE_APP_ONOFF_H_
#define SRC_INCLUDE_APP_ONOFF_H_

void cmdOnOff_set(bool status);

inline void cmdOnOff_on(void) {
	cmdOnOff_set(ZCL_ONOFF_STATUS_ON);
}
inline void cmdOnOff_off(void){
	cmdOnOff_set(ZCL_ONOFF_STATUS_OFF);
}
void cmdOnOff_toggle(void);
void cmdOnOff_onWithTimedOff(zcl_onoff_onWithTimeOffCmd_t *pCmd);

void remoteCmdOnOff(uint8_t cmd);

#endif /* SRC_INCLUDE_APP_ONOFF_H_ */
