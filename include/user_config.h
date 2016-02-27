#ifndef _USER_CONFIG_H_
#define _USER_CONFIG_H_


#define CFG_HOLDER	0x00FF55A4	/* Change this value to load default configurations */
#define CFG_LOCATION	0x3C	/* Please don't change or if you know what you doing */
#define CLIENT_SSL_ENABLE

/*DEFAULT CONFIGURATIONS*/

#define AP_SSID	"KAM_%07u"
#define AP_PASSWORD	"aabbccddeeff"

#define STA_SSID "Slux"
#define STA_PASS "w1reless"
//#define STA_SSID "Loppen Public"
//#define STA_PASS ""
#define STA_FALLBACK_SSID "stofferFon"
#define STA_FALLBACK_PASS "w1reless"
#define STA_TYPE AUTH_WPA2_PSK

#define DEFAULT_SECURITY	0		// 1 for ssl, 0 for none
#define QUEUE_BUFFER_SIZE		 		10240

#define PROTOCOL_NAMEv31	/*MQTT version 3.1 compatible with Mosquitto v0.15*/
//PROTOCOL_NAMEv311			/*MQTT version 3.11 compatible with https://eclipse.org/paho/clients/testing/*/
#endif

