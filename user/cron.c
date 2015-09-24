#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "unix_time.h"
#include "ac_out.h"
#include "cron.h"

cron_jobs_t cron_jobs;
static volatile os_timer_t minute_timer;
char sec_drift;

ICACHE_FLASH_ATTR
void cron_init() {
	// zero out cron_jobs struct
	memset(&cron_jobs, 0, sizeof(cron_jobs));

	// check if there are any cron jobs to run every minute
	os_timer_disarm(&minute_timer);
	os_timer_setfn(&minute_timer, (os_timer_func_t *)minute_timer_func, NULL);
	os_timer_arm(&minute_timer, 60000, 1);
}

ICACHE_FLASH_ATTR
unsigned int add_cron_job_from_query(char *query) {
	char *str;
	char *key, *value;
	char key_value[KEY_VALUE_L];
	char *context_query_string, *context_key_value;
	
	// parse mqtt message for query string
	memset(&cron_jobs.cron_job_list[cron_jobs.n], 0, sizeof(cron_job_t));
	str = strtok_r(query, "&", &context_query_string);
	while (str != NULL) {
		strncpy(key_value, str, KEY_VALUE_L);
		key = strtok_r(key_value, "=", &context_key_value);
		value = strtok_r(NULL, "=", &context_key_value);
		if (strncmp(key, "minute", KEY_VALUE_L) == 0) {
			strncpy(cron_jobs.cron_job_list[cron_jobs.n].minute, value, CRON_FIELD_L);
		}
		else if (strncmp(key, "hour", KEY_VALUE_L) == 0) {
			strncpy(cron_jobs.cron_job_list[cron_jobs.n].hour, value, CRON_FIELD_L);
		}
		else if (strncmp(key, "day_of_month", KEY_VALUE_L) == 0) {
			strncpy(cron_jobs.cron_job_list[cron_jobs.n].day_of_month, value, CRON_FIELD_L);
		}
		else if (strncmp(key, "month", KEY_VALUE_L) == 0) {
			strncpy(cron_jobs.cron_job_list[cron_jobs.n].month, value, CRON_FIELD_L);
		}
		else if (strncmp(key, "day_of_week", KEY_VALUE_L) == 0) {
			strncpy(cron_jobs.cron_job_list[cron_jobs.n].day_of_week, value, CRON_FIELD_L);
		}
		else if (strncmp(key, "command", COMMAND_L) == 0) {
			strncpy(cron_jobs.cron_job_list[cron_jobs.n].command, value, COMMAND_L);
			cron_jobs.n++;
		}
		
		str = strtok_r(NULL, "&", &context_query_string);
	}
	
	return cron_jobs.n;
}

ICACHE_FLASH_ATTR void clear_cron_jobs() {
#ifdef DEBUG
	debug_cron_jobs();
#endif
	memset(&cron_jobs, 0, sizeof(cron_jobs));
#ifdef DEBUG
	os_printf("\n\rcleared all jobs\n\r");
#endif
}

ICACHE_FLASH_ATTR void minute_timer_func(void *arg) {
	struct tm *dt;
	time_t unix_time;
	unsigned char i;
	unsigned char run_command;
	
	unix_time = get_unix_time() + (2 * 60 * 60);	// BUG here - time zone support needed
	dt = localtime(&unix_time);
	
#ifdef DEBUG
	os_printf("%02d:%02d:%02d %d.%d.%d\r\n", dt->tm_hour, dt->tm_min, dt->tm_sec, dt->tm_mday, dt->tm_mon + 1, dt->tm_year + 1900);
	//os_printf("rtc: %u\n\r", get_unix_time());
#endif
	
	// sync to ntp time
	if (dt->tm_sec == 0) {
		if (sec_drift) {
			// we have run the timer with sec_drift interval, run it at normal interval again
			os_timer_disarm(&minute_timer);
			os_timer_setfn(&minute_timer, (os_timer_func_t *)minute_timer_func, NULL);
			os_timer_arm(&minute_timer, 60000, 1);
			sec_drift = 0;
#ifdef DEBUG
			//os_printf("normal\n\r");
#endif
		}
	}
	else {
		sec_drift = (60 - dt->tm_sec);
		os_timer_disarm(&minute_timer);
		os_timer_setfn(&minute_timer, (os_timer_func_t *)minute_timer_func, NULL);
		os_timer_arm(&minute_timer, (sec_drift * 1000), 0);
#ifdef DEBUG
		//os_printf("adjusting by %d s\n\r", sec_drift);
#endif
	}
	
	// check if any jobs should have run the last minute
	for (i = cron_jobs.n; i > 0; i--) {
		run_command = 0;
#ifdef DEBUG
		os_printf("j: %u\t%s:%s\t%s\t %s\t%s\tc: %s\n\r", 
			i,
			cron_jobs.cron_job_list[i - 1].hour,
			cron_jobs.cron_job_list[i - 1].minute,
			cron_jobs.cron_job_list[i - 1].day_of_month,
			cron_jobs.cron_job_list[i - 1].month,
			cron_jobs.cron_job_list[i - 1].day_of_week,
			cron_jobs.cron_job_list[i - 1].command);
#endif
		// check cron jobs hour value
		if (strncmp(cron_jobs.cron_job_list[i - 1].hour, "*", KEY_VALUE_L) == 0) {
			run_command++;
		}
		else if (dt->tm_hour == atoi(cron_jobs.cron_job_list[i - 1].hour)) {
			run_command++;
		}
		
		// check cron jobs minute value
		if (strncmp(cron_jobs.cron_job_list[i - 1].minute, "*", KEY_VALUE_L) == 0) {
			run_command++;
		}
		else if (dt->tm_min == atoi(cron_jobs.cron_job_list[i - 1].minute)) {
			run_command++;
		}
		
		// check cron jobs day_of_month value
		if (strncmp(cron_jobs.cron_job_list[i - 1].day_of_month, "*", KEY_VALUE_L) == 0) {
			run_command++;
		}
		else if (dt->tm_mday == atoi(cron_jobs.cron_job_list[i - 1].day_of_month)) {
			run_command++;
		}
		
		// check cron jobs month value
		if (strncmp(cron_jobs.cron_job_list[i - 1].month, "*", KEY_VALUE_L) == 0) {
			run_command++;
		}
		else if ((dt->tm_mon + 1) == atoi(cron_jobs.cron_job_list[i - 1].month)) {
			run_command++;
		}
		
		// check cron jobs day_of_week value
		if (strncmp(cron_jobs.cron_job_list[i - 1].day_of_week, "*", KEY_VALUE_L) == 0) {
			run_command++;
		}
		else if (dt->tm_wday == atoi(cron_jobs.cron_job_list[i - 1].day_of_week) % 7) {		// (0 or 7 is Sun therefore % 7
			run_command++;
		}

		// we need to run the command
		if (run_command == 5) {
#ifdef DEBUG
			os_printf("run: %s\n\r", cron_jobs.cron_job_list[i - 1].command);
#endif
			if (strncmp(cron_jobs.cron_job_list[i - 1].command, "open", COMMAND_L) == 0) {
				ac_motor_valve_open();
			}
			else if (strncmp(cron_jobs.cron_job_list[i - 1].command, "close", COMMAND_L) == 0) {
				ac_motor_valve_close();
			}
			else if (strncmp(cron_jobs.cron_job_list[i - 1].command, "off", COMMAND_L) == 0) {
				ac_off();
			}
			else if (strncmp(cron_jobs.cron_job_list[i - 1].command, "test", COMMAND_L) == 0) {
				ac_test();
			}
		}
	}
}

ICACHE_FLASH_ATTR void debug_cron_jobs() {
	unsigned char i;
	
	for (i = cron_jobs.n; i > 0; i--) { 
		os_printf("j: %u\t%s:%s\t%s\t %s\t%s\tc: %s\n\r", 
			i,
			cron_jobs.cron_job_list[i - 1].hour,
			cron_jobs.cron_job_list[i - 1].minute,
			cron_jobs.cron_job_list[i - 1].day_of_month,
			cron_jobs.cron_job_list[i - 1].month,
			cron_jobs.cron_job_list[i - 1].day_of_week,
			cron_jobs.cron_job_list[i - 1].command);
	}
}
