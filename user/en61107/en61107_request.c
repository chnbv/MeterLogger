#include <esp8266.h>
#include "driver/uart.h"

#include "unix_time.h"
#include "mqtt.h"
#include "tinyprintf.h"
#include "en61107_request.h"
#include "en61107.h"
#include "config.h"
#include "crypto/crypto.h"
#include "crypto/aes.h"
#include "utils.h"
#include "led.h"

#define QUEUE_SIZE 256

uint64_t en61107_serial = 0;
bool en61107_serial_set = false;
int8_t en61107_request_num;
//unsigned int mqtt_lwt_flag = 0;

// fifo
volatile unsigned int fifo_head, fifo_tail;
volatile unsigned char fifo_buffer[QUEUE_SIZE];

// allocate frame to send
char frame[EN61107_FRAME_L];
unsigned int frame_length;

// allocate struct for response
en61107_response_t response;

static MQTT_Client *mqtt_client = NULL;	// initialize to NULL

static os_timer_t en61107_receive_timeout_timer;
static os_timer_t en61107_delayed_uart_change_setting_timer;
static os_timer_t en61107_meter_wake_up_timer;

volatile en61107_uart_state_t en61107_uart_state;

UartDevice uart_settings;

volatile uint8_t en61107_eod;
bool en61107_etx_received;

ICACHE_FLASH_ATTR
void en61107_receive_timeout_timer_func(void *arg) {
	led_blink();	// DEBUG
	// if no reply received, retransmit

	if (en61107_request_num <= 2) {
		switch (en61107_uart_state) {
			case UART_STATE_EN61107_IDENT:
				en61107_uart_send_en61107_ident();
				break;
			case UART_STATE_EN61107:
				en61107_uart_send_en61107();
				break;
			case UART_STATE_STANDARD_DATA_1:
				en61107_uart_send_standard_data_1();
				break;
			case UART_STATE_STANDARD_DATA_2:
				en61107_uart_send_standard_data_2();
				break;
		}
	}
	else {
		// too many errors, wait for meter to be clear and start over
		os_timer_disarm(&en61107_meter_wake_up_timer);
		os_timer_setfn(&en61107_meter_wake_up_timer, (os_timer_func_t *)en61107_meter_wake_up_timer_func, NULL);
		os_timer_arm(&en61107_meter_wake_up_timer, 60000, 0);         // wait 60 seconds
	}
}

ICACHE_FLASH_ATTR
void en61107_delayed_uart_change_setting_timer_func(UartDevice *uart_settings) {
	uart_set_baudrate(UART0, uart_settings->baut_rate);
//	uart_set_word_length(UART0, SEVEN_BITS);
//	uart_set_parity(UART0, EVEN_BITS);
	uart_set_stop_bits(UART0, uart_settings->stop_bits);
}

ICACHE_FLASH_ATTR
void en61107_meter_wake_up_timer_func(void *arg) {
//	en61107_uart_state = UART_STATE_EN61107_IDENT;
	en61107_uart_send_en61107_ident();
}

// define en61107_received_task() first
ICACHE_FLASH_ATTR
static void en61107_received_task(os_event_t *events) {
	unsigned char c;
	unsigned int i;
	uint64_t current_unix_time;
	char current_unix_time_string[64];	// BUGFIX var
	char key_value[128];
	unsigned char topic[MQTT_TOPIC_L];
	unsigned char message[EN61107_FRAME_L];
	int message_l;

	// vars for aes encryption
	uint8_t cleartext[EN61107_FRAME_L];

	switch (en61107_uart_state) {
		case UART_STATE_EN61107_IDENT:
			// dont en61107_fifo_get() anything here, we get it in next state
			en61107_uart_send_en61107();
			break;
		case UART_STATE_EN61107:
			// get message buffer
			memset(message, 0, sizeof(message));
			i = 0;
			while (en61107_fifo_get(&c) && (i <= EN61107_FRAME_L)) {
				message[i++] = c;
			}
			message_l = i;

			if (parse_en61107_frame(&response, message, message_l) == false) {
				// timeout if we cant parse response
				break;
			}

			en61107_serial = (uint64_t)atoi(response.customer_no);
			en61107_serial_set = true;

			en61107_uart_send_standard_data_1();
			break;
		case UART_STATE_STANDARD_DATA_1:
			// get message buffer
			memset(message, 0, sizeof(message));
			i = 0;
			while (en61107_fifo_get(&c) && (i <= EN61107_FRAME_L)) {
				message[i++] = c;
			}
			message_l = i;
			message[message_l - 2] = 0;		// remove last two chars and null terminate

			if (parse_mc66cde_standard_data_1_frame(&response, message, message_l)) {
				// if we can parse, send next request to meter
				en61107_uart_send_standard_data_2();
				break;
			}

			break;
		case UART_STATE_STANDARD_DATA_2:
			// get message buffer
			memset(message, 0, sizeof(message));
			i = 0;
			while (en61107_fifo_get(&c) && (i <= EN61107_FRAME_L)) {
				message[i++] = c;
			}
			message_l = i;
			message[message_l - 2] = 0;		// remove last two chars and null terminate

			if (parse_mc66cde_standard_data_2_frame(&response, message, message_l)) {
				current_unix_time = (uint32)(get_unix_time());		// TODO before 2038 ,-)
				if (current_unix_time) {	// only send mqtt if we got current time via ntp
   					// format /sample/v2/serial/unix_time => val1=23&val2=val3&baz=blah
					memset(topic, 0, sizeof(topic));			// clear it
					tfp_snprintf(current_unix_time_string, 64, "%u", (uint32_t)current_unix_time);
					tfp_snprintf(topic, MQTT_TOPIC_L, "/sample/v2/%u/%s", (uint32_t)en61107_serial, current_unix_time_string);

					memset(message, 0, sizeof(message));			// clear it

					// heap size
					tfp_snprintf(key_value, MQTT_TOPIC_L, "heap=%u&", system_get_free_heap_size());
					strcat(message, key_value);

					// meter program
					tfp_snprintf(key_value, MQTT_TOPIC_L, "program=%01u%01u%03u%02u%01u%02u%02u&", 
						response.meter_program.a, 
						response.meter_program.b, 
						response.meter_program.ccc, 
						response.meter_program.dd, 
						response.meter_program.e, 
						response.meter_program.ff, 
						response.meter_program.gg
					);
					strcat(message, key_value);

					// heating meter specific
					// flow temperature
					tfp_snprintf(key_value, MQTT_TOPIC_L, "t1=%s %s&", response.t1.value, response.t1.unit);
					strcat(message, key_value);
        	
					// return flow temperature
					tfp_snprintf(key_value, MQTT_TOPIC_L, "t2=%s %s&", response.t2.value, response.t2.unit);
					strcat(message, key_value);

					// t3 temperature
					tfp_snprintf(key_value, MQTT_TOPIC_L, "t3=%s %s&", response.t3.value, response.t3.unit);
					strcat(message, key_value);

					// calculated temperature difference
					tfp_snprintf(key_value, MQTT_TOPIC_L, "tdif=%s %s&", response.tdif.value, response.tdif.unit);
					strcat(message, key_value);

					// flow
					tfp_snprintf(key_value, MQTT_TOPIC_L, "flow1=%s %s&", response.flow1.value, response.flow1.unit);
					strcat(message, key_value);

					// current power
					tfp_snprintf(key_value, MQTT_TOPIC_L, "effect1=%s %s&", response.effect1.value, response.effect1.unit);
					strcat(message, key_value);

					// hours
					tfp_snprintf(key_value, MQTT_TOPIC_L, "hr=%s %s&", response.hr.value, response.hr.unit);
					strcat(message, key_value);

					// volume
					tfp_snprintf(key_value, MQTT_TOPIC_L, "v1=%s %s&", response.v1.value, response.v1.unit);
					strcat(message, key_value);

					// power
					tfp_snprintf(key_value, MQTT_TOPIC_L, "e1=%s %s&", response.e1.value, response.e1.unit);
					strcat(message, key_value);

					memset(cleartext, 0, sizeof(cleartext));
					os_strncpy(cleartext, message, sizeof(message));	// make a copy of message for later use
					os_memset(message, 0, sizeof(message));				// ...and clear it

					// encrypt and send
					message_l = encrypt_aes_hmac_combined(message, topic, strlen(topic), cleartext, strlen(cleartext) + 1);

					// and stop retransmission timeout timer, last data from meter
					os_timer_disarm(&en61107_receive_timeout_timer);

					if (mqtt_client) {
						// if mqtt_client is initialized
						if (message_l > 1) {
							MQTT_Publish(mqtt_client, topic, message, message_l, 2, 0);	// QoS level 2
						}
					}

					// change to last state - idle state
					en61107_uart_state = UART_STATE_NONE;
					en61107_request_num = 0;
				}
			}
			break;
	}
}

ICACHE_FLASH_ATTR
void en61107_request_init() {
	fifo_head = 0;
	fifo_tail = 0;

	memset(&response, 0, sizeof(response));	// zero response before use

	en61107_uart_state = UART_STATE_NONE;

	en61107_request_num = 0;

	en61107_etx_received = false;
	
	system_os_task(en61107_received_task, en61107_received_task_prio, en61107_received_task_queue, en61107_received_task_queue_length);
}

// helper function to pass mqtt client struct from user_main.c to here
ICACHE_FLASH_ATTR
void en61107_set_mqtt_client(MQTT_Client* client) {
	mqtt_client = client;
}

// helper function to pass received kmp_serial to user_main.c
ICACHE_FLASH_ATTR
uint32_t en61107_get_received_serial() {
	return (uint32_t)en61107_serial;
}

//ICACHE_FLASH_ATTR
inline bool en61107_is_eod_char(uint8_t c) {
	if (en61107_eod == 0x03) {
		// we need to get the next char as well for this protocol
		if (c == en61107_eod) {
			en61107_etx_received = true;
			return false;
		}
		else if (en61107_etx_received) {
//			en61107_etx_received = false;	// DEBUG: should really be here
			return true;
		}
	}
	else {
		if (c == en61107_eod) {
			return true;
		}
		else {
			return false;
		}
	}
	return false;
}

ICACHE_FLASH_ATTR
void en61107_request_send() {
#ifndef DEBUG_NO_METER
	unsigned char c;
	unsigned int i;

	// clear message buffer
	while (en61107_fifo_get(&c) && (i <= EN61107_FRAME_L)) {
		i++;
	}

	en61107_request_num++;
	if (en61107_uart_state == UART_STATE_NONE) {
		// give the meter some time between retransmissions
		os_timer_disarm(&en61107_meter_wake_up_timer);
		os_timer_setfn(&en61107_meter_wake_up_timer, (os_timer_func_t *)en61107_meter_wake_up_timer_func, NULL);
		os_timer_arm(&en61107_meter_wake_up_timer, 6000, 0);         // 6 seconds for meter to wake up
	}

#else
	unsigned char topic[128];
	unsigned char cleartext[EN61107_FRAME_L];
	unsigned char message[EN61107_FRAME_L];
	int topic_l;
	int message_l;
	
	// fake serial for testing without meter
	en61107_serial = 4615611;
	
	topic_l = os_sprintf(topic, "/sample/v2/%u/%u", en61107_serial, get_unix_time());
	message_l = os_sprintf(message, "heap=20000&t1=25.00 C&t2=15.00 C&tdif=10.00 K&flow1=0 l/h&effect1=0.0 kW&hr=0 h&v1=0.00 m3&e1=0 kWh&");
	memset(cleartext, 0, sizeof(cleartext));
	os_strncpy(cleartext, message, sizeof(message));	// make a copy of message for later use
	os_memset(message, 0, sizeof(message));				// ...and clear it

	message_l = encrypt_aes_hmac_combined(message, topic, strlen(topic), cleartext, strlen(cleartext) + 1);

	if (mqtt_client) {
		// if mqtt_client is initialized
		if (message_l > 1) {
			MQTT_Publish(mqtt_client, topic, message, message_l, 2, 0);	// QoS level 2
		}
	}

#endif
}

ICACHE_FLASH_ATTR
void en61107_uart_send_en61107_ident() {
	en61107_eod = '\n';
	en61107_etx_received = false;	// DEBUG: should be in en61107_is_eod_char() but does not work

	// 300 bps
	uart_set_baudrate(UART0, BIT_RATE_300);
	uart_set_word_length(UART0, SEVEN_BITS);
	uart_set_parity(UART0, EVEN_BITS);
	uart_set_stop_bits(UART0, TWO_STOP_BIT);

	// change to next state
	en61107_uart_state = UART_STATE_EN61107_IDENT;

	// and start retransmission timeout timer
	os_timer_disarm(&en61107_receive_timeout_timer);
	os_timer_setfn(&en61107_receive_timeout_timer, (os_timer_func_t *)en61107_receive_timeout_timer_func, NULL);
	os_timer_arm(&en61107_receive_timeout_timer, 2000, 0);         // 2 seconds for UART_STATE_EN61107_IDENT

	strcpy(frame, "/?!\r\n");
	frame_length = strlen(frame);
	uart0_tx_buffer(frame, frame_length);     // send request

	// reply is 300 bps, 7e1, not 7e2 as stated in standard
	uart_settings.baut_rate = BIT_RATE_300;
	uart_settings.stop_bits = ONE_STOP_BIT;
	// change uart settings after the data has been sent
	os_timer_disarm(&en61107_delayed_uart_change_setting_timer);
	os_timer_setfn(&en61107_delayed_uart_change_setting_timer, (os_timer_func_t *)en61107_delayed_uart_change_setting_timer_func, &uart_settings);
	os_timer_arm(&en61107_delayed_uart_change_setting_timer, 400, 0);	// 400 mS
}

ICACHE_FLASH_ATTR
void en61107_uart_send_en61107() {
	en61107_eod = 0x03;
	en61107_etx_received = false;	// DEBUG: should be in en61107_is_eod_char() but does not work

	// 300 bps
	uart_set_baudrate(UART0, BIT_RATE_300);
	uart_set_word_length(UART0, SEVEN_BITS);
	uart_set_parity(UART0, EVEN_BITS);
	uart_set_stop_bits(UART0, TWO_STOP_BIT);

	// change to next state
	en61107_uart_state = UART_STATE_EN61107;

	// and start retransmission timeout timer
	os_timer_disarm(&en61107_receive_timeout_timer);
	os_timer_setfn(&en61107_receive_timeout_timer, (os_timer_func_t *)en61107_receive_timeout_timer_func, NULL);
	os_timer_arm(&en61107_receive_timeout_timer, 8000, 0);         // 8 seconds for UART_STATE_EN61107

	strcpy(frame, "\006000\r\n");
	frame_length = strlen(frame);
	uart0_tx_buffer(frame, frame_length);     // send request

	// reply is 300 bps, 7e1, not 7e2 as stated in standard
	uart_settings.baut_rate = BIT_RATE_300;
	uart_settings.stop_bits = ONE_STOP_BIT;
	// change uart settings after the data has been sent
	os_timer_disarm(&en61107_delayed_uart_change_setting_timer);
	os_timer_setfn(&en61107_delayed_uart_change_setting_timer, (os_timer_func_t *)en61107_delayed_uart_change_setting_timer_func, &uart_settings);
	os_timer_arm(&en61107_delayed_uart_change_setting_timer, 400, 0);	// 400 mS
}

ICACHE_FLASH_ATTR
void en61107_uart_send_standard_data_1() {
	en61107_eod = '\r';

	// 300 bps
	uart_set_baudrate(UART0, BIT_RATE_300);
	uart_set_word_length(UART0, SEVEN_BITS);
	uart_set_parity(UART0, EVEN_BITS);
	uart_set_stop_bits(UART0, TWO_STOP_BIT);
	
	// change to next state
	en61107_uart_state = UART_STATE_STANDARD_DATA_1;

	// and start retransmission timeout timer
	os_timer_disarm(&en61107_receive_timeout_timer);
	os_timer_setfn(&en61107_receive_timeout_timer, (os_timer_func_t *)en61107_receive_timeout_timer_func, NULL);
	os_timer_arm(&en61107_receive_timeout_timer, 3000, 0);         // 3 seconds for UART_STATE_STANDARD_DATA_1

	strcpy(frame, "/#1\r");
	frame_length = strlen(frame);
	uart0_tx_buffer(frame, frame_length);     // send request

	// reply is 1200 bps, 7e1, not 7e2 as stated in standard
	uart_settings.baut_rate = BIT_RATE_1200;
	uart_settings.stop_bits = ONE_STOP_BIT;
	// change uart settings after the data has been sent
	os_timer_disarm(&en61107_delayed_uart_change_setting_timer);
	os_timer_setfn(&en61107_delayed_uart_change_setting_timer, (os_timer_func_t *)en61107_delayed_uart_change_setting_timer_func, &uart_settings);
	os_timer_arm(&en61107_delayed_uart_change_setting_timer, 220, 0);	// 220 mS
}

ICACHE_FLASH_ATTR
void en61107_uart_send_standard_data_2() {
	en61107_eod = '\r';

	// 300 bps
	uart_set_baudrate(UART0, BIT_RATE_300);
	uart_set_word_length(UART0, SEVEN_BITS);
	uart_set_parity(UART0, EVEN_BITS);
	uart_set_stop_bits(UART0, TWO_STOP_BIT);
	
	// change to next state
	en61107_uart_state = UART_STATE_STANDARD_DATA_2;

	// and start retransmission timeout timer
	os_timer_disarm(&en61107_receive_timeout_timer);
	os_timer_setfn(&en61107_receive_timeout_timer, (os_timer_func_t *)en61107_receive_timeout_timer_func, NULL);
	os_timer_arm(&en61107_receive_timeout_timer, 3000, 0);         // 3 seconds for UART_STATE_STANDARD_DATA_2

	strcpy(frame, "/#2\r");
	frame_length = strlen(frame);
	uart0_tx_buffer(frame, frame_length);     // send request

	// reply is 1200 bps, 7e1, not 7e2 as stated in standard
	uart_settings.baut_rate = BIT_RATE_1200;
	uart_settings.stop_bits = ONE_STOP_BIT;
	// change uart settings after the data has been sent
	os_timer_disarm(&en61107_delayed_uart_change_setting_timer);
	os_timer_setfn(&en61107_delayed_uart_change_setting_timer, (os_timer_func_t *)en61107_delayed_uart_change_setting_timer_func, &uart_settings);
	os_timer_arm(&en61107_delayed_uart_change_setting_timer, 220, 0);	// 220 mS
}



// fifo
ICACHE_FLASH_ATTR
unsigned int en61107_fifo_in_use() {
	return fifo_head - fifo_tail;
}

//ICACHE_FLASH_ATTR
inline unsigned char en61107_fifo_put(unsigned char c) {
	if (en61107_fifo_in_use() != QUEUE_SIZE) {
		fifo_buffer[fifo_head++ % QUEUE_SIZE] = c;
		// wrap
		if (fifo_head == QUEUE_SIZE) {
			fifo_head = 0;
		}
		return 1;
	}
	else {
		return 0;
	}
}

ICACHE_FLASH_ATTR
unsigned char en61107_fifo_get(unsigned char *c) {
	if (en61107_fifo_in_use() != 0) {
		*c = fifo_buffer[fifo_tail++ % QUEUE_SIZE];
		// wrap
		if (fifo_tail == QUEUE_SIZE) {
			fifo_tail = 0;
		}
		return 1;
	}
	else {
		return 0;
	}
}

ICACHE_FLASH_ATTR
unsigned char en61107_fifo_snoop(unsigned char *c, unsigned int pos) {
	if (en61107_fifo_in_use() > (pos)) {
        *c = fifo_buffer[(fifo_tail + pos) % QUEUE_SIZE];
		return 1;
	}
	else {
		return 0;
	}
}

