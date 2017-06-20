/* UDP reporting Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "udp.h"

#include <sys/socket.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>
#include <esp_event_loop.h>	// esp_event_loop_init()
#include <rom/rtc.h>
#include <rom/uart.h>		// uart_tx_wait_idle()
#include <driver/timer.h>	// timer_get_counter_time_sec()
#include <soc/efuse_reg.h>	// needed by getChip*()

// best to provide in CFLAGS: AP_SSID AP_PASS MY_IP MY_NAME
#ifndef AP_SSID
#error undefined AP_SSID
#endif

#ifndef AP_PASS
#error undefined AP_SSID
#endif

#ifndef SVR_IP
#define SVR_IP		"192.168.2.7"	// server IP
#endif

#ifndef SVR_PORT
#define SVR_PORT	21883		// server port
#endif

#ifdef MY_IP				// client IP
#define USE_DHCPC	0		// use static IP

#ifndef MY_NM
#define MY_NM		"255.255.255.0"	// netmask
#endif	// ifndef MY_NM

#ifndef MY_GW
#define MY_GW		SVR_IP		// gateway
#endif // ifndef MY_GW

#else // ifdef MY_IP
#define USE_DHCPC	1		// use dhcp
#endif

#ifndef MY_NAME
#define MY_NAME		"test"
#endif

#define SLEEP_S		 5	// seconds
#define WIFI_GRACE_MS	50	// time to wait before deep sleep to drain wifi tx
#define WIFI_TIMEOUT_MS	4000	// time to wait for WiFi connection
#define WIFI_DISCONNECT_MS 100	// time to wait for WiFi disconnection

#define DISCONNECT	0	// 1= disconnect before deep sleep
#define PRINT_MSG	0	// 1= print sent message if logging is off

#define I2C_SCL		22	// i2c
#define I2C_SDA		21	// i2c
#define OUT_PIN		19	// OUT toggled to mark program steps
#define OW_PIN		18	// IO  ow
#define ERROR_PIN	17	// OUT indicate a failure (debug). -ve to disable
#define TOGGLE_PIN	16	// IN  pull low to disable toggle()
#define DBG_PIN		15	// IN  pull low to silence Log()

				// adc pins are 32-39
#define VDD_PIN		32	// undef to disable
#define VDD_DIVIDER	2	// 1m.1m
#define VDD_ATTEN	6	// 6db

#define BAT_PIN		33	// undef to disable
#define BAT_DIVIDER	3	// 1m.2m
#define BAT_ATTEN	6	// 6db

#define V1_PIN		34	// undef to disable
#define V1_DIVIDER	1	// resistor network
#define V1_ATTEN	6	// 6db

#define READ_TSENS	1	// read esp32 temperature sensor

#if   62 == MY_HOST	// esp-32a
#define READ_BME280	1	// enable if you have one connected
#define READ_DS18B20	1	// enable if you have one connected
#define BAT_VOLTAGE	5.0

#elif 64 == MY_HOST	// esp-32b
#define READ_BME280	0	// enable if you have one connected
#define READ_DS18B20	1	// enable if you have one connected
#define BAT_VOLTAGE	3.3
#undef  V1_PIN

#elif 65 == MY_HOST	// esp-32c
#define READ_BME280	0	// enable if you have one connected
#define READ_DS18B20	1	// enable if you have one connected
#define BAT_VOLTAGE	3.3
#undef  V1_PIN

#else
#error unknown host MY_HOST
#endif

#define READ_ADC	(defined(VDD_PIN) || defined(BAT_PIN) || defined(V1_PIN))

#if READ_ADC
#include "adc.h"
#endif

#if OUT_PIN < 0
#undef TOGGLE_PIN
#define TOGGLE_PIN	-1
#endif

#if READ_BME280
#include "bme280.h"
RTC_DATA_ATTR static int bme280_failures = 0;
#endif

#if READ_DS18B20
#include "ds18b20.h"
  #define ROM_ID	NULL	// when only one ow device connected, fastest
//#define ROM_ID	(uint8_t *)"\x28\xdc\x01\x78\x06\x00\x00\x0f"	// esp-32a
//#define ROM_ID	(uint8_t *)"\x28\xc5\x3e\x76\x06\x00\x00\x3c"	// esp-32b
//#define ROM_ID	(uint8_t *)"\x28\xa9\x7f\x78\x06\x00\x00\xb3"	// esp-32c
RTC_DATA_ATTR static int ds18b20_failures = 0;
#endif

#if READ_TSENS
#include "tsens.h"
#endif

int do_log = 1;

RTC_DATA_ATTR static int runCount = 0;
RTC_DATA_ATTR static uint64_t sleep_start_us = 0;
RTC_DATA_ATTR static uint64_t sleep_start_ticks = 0;
RTC_DATA_ATTR static uint64_t prev_app_start_us = 0;
RTC_DATA_ATTR static int failSoft = 0;
RTC_DATA_ATTR static int failHard = 0;
RTC_DATA_ATTR static int failRead = 0;
RTC_DATA_ATTR static int failReadHard = 0;
RTC_DATA_ATTR static int lastGrace = 0;
RTC_DATA_ATTR static uint64_t timeLast = 0;
RTC_DATA_ATTR static uint64_t timeTotal = 0;

static int do_toggle = 0;
static uint64_t app_start_us = 0;
static uint64_t app_start_ticks = 0;
static uint64_t time_wifi_us = 0;
static struct timeval app_start;
static int wakeup_cause;
static int reset_reason;
static int rssi = 0;
static int sent = 0;
static int retry_count = 0;

static EventGroupHandle_t event_group;
const int HAVE_WIFI = BIT0;
const int NO_WIFI = BIT1;

// The following functions are missing a prototype.
uint64_t rtc_time_get(void);		// raw RTC time in ticks
uint64_t system_get_rtc_time(void);	// adjusted RTC time

#if 001
// this is my simplified version of _gettimeofday_r(), added to time.c
uint64_t gettimeofday_us(void);		// FRC
#else
// or you can add it here:
uint64_t gettimeofday_us(void)
{
	timeval tv;

	gettimeofday (&tv);
	return tv.tv_sec*1000000 + tv.tv_usec;
}
#endif

void flush_uart (void)
{
	fflush(stdout);
	uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);
}

static uint8_t getChipRevision (void)
{
	return (REG_READ (EFUSE_BLK0_RDATA3_REG)>>EFUSE_RD_CHIP_VER_RESERVE_S)&&EFUSE_RD_CHIP_VER_RESERVE_V;
}

#if 000
// does not work
static uint64_t getChipMAC (void)
{
	uint64_t mac = 
 (REG_READ (EFUSE_BLK0_RDATA2_REG)>>EFUSE_RD_WIFI_MAC_CRC_HIGH_S)&&EFUSE_RD_WIFI_MAC_CRC_HIGH_V;
	mac = (mac << 32) |
((REG_READ (EFUSE_BLK0_RDATA1_REG)>>EFUSE_RD_WIFI_MAC_CRC_LOW_S) &&EFUSE_RD_WIFI_MAC_CRC_LOW_V);
	return mac;
}
#endif

static void toggle_setup (void)
{
#if OUT_PIN >= 0
	gpio_pad_select_gpio(OUT_PIN);
	gpio_set_direction(OUT_PIN, GPIO_MODE_OUTPUT);
	gpio_set_level(OUT_PIN, 1);	// start HIGH
#endif
}

#if USE_DELAY_BUSY
void delay_us_busy (int us)
{
	uint64_t end = gettimeofday_us() + us;
	while (gettimeofday_us() < end)
		{}
}
#endif

// pin is LOW  during sleep
// pin is HIGH at start
// toggle is 100us: 50us LOW, 50us HIGH (not on final time)
static void toggle_out (int ntimes, int us)
{
	if (do_toggle) while (ntimes-- > 0) {
		gpio_set_level(OUT_PIN, 0);
		delay_us (us);
		gpio_set_level(OUT_PIN, 1);
		if (ntimes)
			delay_us (us);
	}
}

void toggle (int ntimes) {
	toggle_out (ntimes, 50);
}

void toggle_short (int ntimes) {
	toggle_out (ntimes, 2);
}

#if ERROR_PIN >= 0
static void toggle_error (void)
{
	gpio_pad_select_gpio(ERROR_PIN);
	gpio_set_direction(ERROR_PIN, GPIO_MODE_OUTPUT);
	gpio_set_level(ERROR_PIN, 1);	// start HIGH
	delay_us (50);
	gpio_set_level(ERROR_PIN, 0);
}
#else
#define toggle_error()
#endif

void get_time_tv (struct timeval *now)
{
	gettimeofday (now, NULL);
	if (now->tv_usec < app_start.tv_usec) {
		now->tv_usec += 1000000;
		--now->tv_sec;
	}

	now->tv_sec  -= app_start.tv_sec;
	now->tv_usec -= app_start.tv_usec;
}

static uint64_t get_time_us (void) {
	return gettimeofday_us() - app_start_us;
}

#if READ_DS18B20
static esp_err_t ds18b20_temp (float *temp)
{
	esp_err_t ret;
	esp_err_t rval;		// return first failure
	uint8_t id[8];

	*temp = 0.0;
	rval = ESP_OK;

	DbgR (ds18b20_init (OW_PIN, ROM_ID));

	if (reset_reason != DEEPSLEEP_RESET) {	// cold start
		DbgR (ds18b20_read_id (id));
		Log("ROM id: %02x %02x %02x %02x %02x %02x %02x %02x",
			id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);

		DbgR (ds18b20_convert (1));
	}
	Dbg (ds18b20_read_temp (temp));
	if (ESP_OK == rval) rval = ret;
	if (reset_reason == DEEPSLEEP_RESET) {	// deep sleep wakeup
		Dbg (ds18b20_convert (0));
		if (ESP_OK == rval) rval = ret;
	}

	Dbg (ds18b20_depower ());
	if (ESP_OK == rval) rval = ret;

	return rval;
}
#endif // READ_DS18B20

#define MAX_TEMPS		3
static int ntemps = 0;
static float temps[MAX_TEMPS];
static float bat, vdd, v1;
static char weather[40] = "";
static float ds18b20_failure_reason = 0;
static uint64_t time_readings_us = 0;
static int tsens;

// save first failure in 'rval'
//
#define DbgRval(f) \
do { \
	Dbg (f); \
	if (ESP_OK == rval) rval = ret; \
} while (0)

static esp_err_t do_readings (void)
{
	esp_err_t ret;
	esp_err_t rval;		// return first failure
	float temp;

	flush_uart ();		// avoid uart interruptions

	time_readings_us = gettimeofday_us();
	ntemps = 0;
	rval = ESP_OK;

#if READ_DS18B20
	DbgRval (ds18b20_temp (&temp));
	if (ret != ESP_OK || temp >= BAD_TEMP) {
		toggle_error();		// tell DSO
		++ds18b20_failures;
		ds18b20_failure_reason = temp;
		DbgRval (ds18b20_temp (&temp));	// one retry
		if (ret != ESP_OK || temp >= BAD_TEMP) {
			toggle_error();		// tell DSO
			++failReadHard;
			temp = BAD_TEMP;
		} else
			++failRead;
	}
	if (ntemps < MAX_TEMPS) temps[ntemps++] = temp;
#endif // READ_DS18B20

#if READ_TSENS
	DbgRval (tsens_read (&tsens));
	if (ntemps < MAX_TEMPS) temps[ntemps++] = tsens;
#endif

#if READ_BME280
{
	float qfe, h, qnh;
	int fail;

	DbgRval (bme280_init(I2C_SDA, I2C_SCL, reset_reason != DEEPSLEEP_RESET));

	DbgRval (bme280_read (622, &temp, &qfe, &h, &qnh));
	if (ret != ESP_OK || temp >= BAD_TEMP) {
		toggle_error();		// tell DSO
		++failRead;
		++bme280_failures;
	}

	fail = 0;
	if (       BAD_TEMP <= temp) fail |= 0x01;
	if (BME280_BAD_QFE  == qfe)  fail |= 0x02;
	if (BME280_BAD_HUMI == h)    fail |= 0x04;

	snprintf (weather, sizeof(weather),
		" w=T%.2f,P%.3f,H%.3f,f%x",
		temp, qnh, h, fail);
	if (ntemps < MAX_TEMPS) temps[ntemps++] = temp;
}
#endif // READ_BME280

	if (0 == ntemps)
		if (ntemps < MAX_TEMPS) temps[ntemps++] = 0;

#if READ_ADC
	DbgRval (adc_init (12));
#endif

#ifdef VDD_PIN
	DbgRval (adc_read (&vdd, VDD_PIN, VDD_ATTEN, VDD_DIVIDER));
#else
	vdd = 3.3;
#endif

#ifdef BAT_PIN
	DbgRval (adc_read (&bat, BAT_PIN, BAT_ATTEN, BAT_DIVIDER));
#else
	bat = BAT_VOLTAGE;
#endif

#ifdef V1_PIN
	DbgRval (adc_read (&v1, V1_PIN, V1_ATTEN, V1_DIVIDER));
#else
	v1 = 0;
#endif

	time_readings_us = gettimeofday_us() - time_readings_us;

	return rval;
}
#undef DbgRval

#if 000
From include/rom/rtc.h:

typedef enum {
    NO_SLEEP        = 0,
    EXT_EVENT0_TRIG = BIT0,
    EXT_EVENT1_TRIG = BIT1,
    GPIO_TRIG       = BIT2,
    TIMER_EXPIRE    = BIT3,
    SDIO_TRIG       = BIT4,
    MAC_TRIG        = BIT5,
    UART0_TRIG      = BIT6,
    UART1_TRIG      = BIT7,
    TOUCH_TRIG      = BIT8,
    SAR_TRIG        = BIT9,
    BT_TRIG         = BIT10
} WAKEUP_REASON;	// from rtc_get_wakeup_cause()

// rtc_get_wakeup_cause() returns 0x385 meaning bits: 9,8 7 2,0
//	SAR_TRIG TOUCH_TRIG UART1_TRIG GPIO_TRIG EXT_EVENT0_TRIG

typedef enum {
    NO_MEAN                =  0,
    POWERON_RESET          =  1,    /**<1, Vbat power on reset*/
    SW_RESET               =  3,    /**<3, Software reset digital core*/
    OWDT_RESET             =  4,    /**<4, Legacy watch dog reset digital core*/
    DEEPSLEEP_RESET        =  5,    /**<3, Deep Sleep reset digital core*/
    SDIO_RESET             =  6,    /**<6, Reset by SLC module, reset digital core*/
    TG0WDT_SYS_RESET       =  7,    /**<7, Timer Group0 Watch dog reset digital core*/
    TG1WDT_SYS_RESET       =  8,    /**<8, Timer Group1 Watch dog reset digital core*/
    RTCWDT_SYS_RESET       =  9,    /**<9, RTC Watch dog Reset digital core*/
    INTRUSION_RESET        = 10,    /**<10, Instrusion tested to reset CPU*/
    TGWDT_CPU_RESET        = 11,    /**<11, Time Group reset CPU*/
    SW_CPU_RESET           = 12,    /**<12, Software reset CPU*/
    RTCWDT_CPU_RESET       = 13,    /**<13, RTC Watch dog Reset CPU*/
    EXT_CPU_RESET          = 14,    /**<14, for APP CPU, reseted by PRO CPU*/
    RTCWDT_BROWN_OUT_RESET = 15,    /**<15, Reset when the vdd voltage is not stable*/
    RTCWDT_RTC_RESET       = 16     /**<16, RTC Watch dog reset digital core and rtc module*/
} RESET_REASON;		// from rtc_get_reset_reason()
#endif

static int format_message (char *message, int mlen)
{
	struct timeval now;
	uint64_t sleep_us;
	uint64_t sleep_ticks;
	float cycle_s;
	char *buf = message;
	int blen = mlen;
	int len;
	int i;

	sleep_us = (sleep_start_us > 0) ? app_start_us - sleep_start_us : 0;
	sleep_ticks = (sleep_start_ticks > 0) ? app_start_ticks - sleep_start_ticks : 0;

//Log("sleep_start=%lld app_start=%lld sleep_time=%lld",
//	sleep_start_us, app_start_us, sleep_us);

	len = snprintf (buf, blen,
		"show %s %d",
		MY_NAME, runCount);
	if (len > 0) {
		buf += len;
		blen -= len;
	}

	get_time_tv (&now);

	len = snprintf (buf, blen,
		" times=D%lld,T%lld,s%u.%06u,r%.3f,w%.3f,t%u.%06u",
		sleep_us, sleep_ticks,
		(uint)app_start.tv_sec, (uint)app_start.tv_usec,
		time_readings_us / 1000000.,
		time_wifi_us / 1000000.,
		(uint)now.tv_sec, (uint)now.tv_usec);
	if (len > 0) {
		buf += len;
		blen -= len;
	}

	cycle_s = (app_start_us - prev_app_start_us) / 1000000.;

	len = snprintf (buf, blen,
		" prev=L%.3f,T%d,c%.6f,a%.6f",
		timeLast / 1000000.,
		(uint32_t)(timeTotal / 1000000),
		cycle_s,		// last cycle  time
		cycle_s - SLEEP_S);	// last active time
	if (len > 0) {
		buf += len;
		blen -= len;
	}

    {
	uint64_t ticks = rtc_time_get();		// raw RTC time
	uint64_t RTC   = system_get_rtc_time();		// adjusted rtc_time_get()
	uint64_t FRC   = gettimeofday_us();		// FRC
	len = snprintf (buf, blen,
		" clocks=R%llu,F%llu,t%llu,g%d",
		RTC, FRC, ticks, lastGrace);
	if (len > 0) {
		buf += len;
		blen -= len;
	}
    }

#if 000
    {
	esp_err_t ret;
	double tmr_00_sec = 0;
	Dbg (timer_get_counter_time_sec (0, 0, &tmr_00_sec));
	double tmr_01_sec = 0;
	Dbg (timer_get_counter_time_sec (0, 1, &tmr_01_sec)) ;
	double tmr_10_sec = 0;
	Dbg (timer_get_counter_time_sec (1, 0, &tmr_10_sec));
	double tmr_11_sec = 0;
	Dbg (timer_get_counter_time_sec (1, 1, &tmr_11_sec));
	len = snprintf (buf, blen,
		" timers=%.6f,%.6f,%.6f,%.6f",
		tmr_00_sec, tmr_01_sec, tmr_10_sec, tmr_11_sec);
	if (len > 0) {
		buf += len;
		blen -= len;
	}
    }
#endif

	len = snprintf (buf, blen,
		" stats=fs%d,fh%d,fr%d,fR%d",
		failSoft, failHard, failRead, failReadHard);
	if (len > 0) {
		buf += len;
		blen -= len;
	}
#if READ_DS18B20
	len = snprintf (buf, blen,
		",Dc%d,Dr%.4f",
		ds18b20_failures, ds18b20_failure_reason);
	if (len > 0) {
		buf += len;
		blen -= len;
	}
#endif
#if READ_BME280
	len = snprintf (buf, blen,
		",Bc%d",
		bme280_failures);
	if (len > 0) {
		buf += len;
		blen -= len;
	}
#endif
	len = snprintf (buf, blen,
		",c%03x,r%d",
		wakeup_cause, reset_reason);
	if (len > 0) {
		buf += len;
		blen -= len;
	}

	len = snprintf (buf, blen,
		" v=%.3f,%.3f,%.3f",
		bat, vdd, v1);
	if (len > 0) {
		buf += len;
		blen -= len;
	}

	len = snprintf (buf, blen,
		" radio=s%d",
		-rssi);
	if (len > 0) {
		buf += len;
		blen -= len;
	}

	if ('\0' != weather[0]) {
		len = snprintf (buf, blen,
			"%s",
			weather);
		if (len > 0) {
			buf += len;
			blen -= len;
		}
	}

	len = snprintf (buf, blen,
		" adc=%.3f vdd=%.3f",
		bat, vdd);
	if (len > 0) {
		buf += len;
		blen -= len;
	}

	for (i = 0; i < ntemps; ++i) {
		len = snprintf (buf, blen,
			"%c%.4f",
			(i ? ',' : ' '), temps[i]);
		if (len > 0) {
			buf += len;
			blen -= len;
		}
	}

#if PRINT_MSG
	if (!do_log)
		LogF ("%s", message);
#endif

	++runCount;

	return mlen - blen;
}

static void send_message (char * message, int mlen)
{
	int mysocket;
	struct sockaddr_in remote_addr;

	mysocket = socket(AF_INET, SOCK_DGRAM, 0);
	remote_addr.sin_family = AF_INET;
	remote_addr.sin_port = htons(SVR_PORT);
	remote_addr.sin_addr.s_addr = inet_addr(SVR_IP);

Log ("sending '%s'", message);
	toggle(2);
	sendto(mysocket, message, mlen, 0,
		(struct sockaddr *)&remote_addr, sizeof(remote_addr));
}

static void do_grace (void)
{
	if (!sent)
		return;

#if defined(WIFI_GRACE_MS) && WIFI_GRACE_MS > 0
	toggle(3);
Log ("delay %dms", WIFI_GRACE_MS);
	uint64_t grace_us = gettimeofday_us();
	delay_ms (WIFI_GRACE_MS + do_log*5);	// time to drain wifi queue
	lastGrace = (int)(gettimeofday_us() - grace_us);
#endif
}

static void finish (void)
{
	if (retry_count > 1)
		++failHard;
	else if (retry_count > 0)
		++failSoft;

	if (sent)
		do_grace ();

#if DISCONNECT
Log ("esp_wifi_disconnect");
	DbgR (esp_wifi_disconnect());

Log ("xEventGroupWaitBits");
	xEventGroupWaitBits(event_group, NO_WIFI,
		false, false, WIFI_DISCONNECT_MS / portTICK_PERIOD_MS);
#endif

Log ("esp_deep_sleep %ds", SLEEP_S);
	flush_uart();

	toggle(4);
	sleep_start_us = gettimeofday_us();
	sleep_start_ticks = rtc_time_get ();
	prev_app_start_us = app_start_us;
	timeLast = sleep_start_us - app_start_us;
	timeTotal += timeLast;
	esp_deep_sleep(SLEEP_S*1000000);

	vTaskDelete(NULL);
}

static esp_err_t set_ip (void)
{
#if !USE_DHCPC
Log ("tcpip_adapter_dhcpc_stop");
	DbgR (tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA));

Log ("tcpip_adapter_set_ip_info");
	tcpip_adapter_ip_info_t ip_info_new;
	memset (&ip_info_new, 0, sizeof(ip_info_new));
	ip4addr_aton(MY_IP, &ip_info_new.ip);
	ip4addr_aton(MY_NM, &ip_info_new.netmask);
	ip4addr_aton(MY_GW, &ip_info_new.gw);
	DbgR (tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info_new));
#endif	/* if !USE_DHCPC */

	return ESP_OK;
}

static esp_err_t event_handler (void *ctx, system_event_t *event)
{
Log("event_handler: SYSTEM_EVENT %d", event->event_id);
	switch(event->event_id) {
	case SYSTEM_EVENT_STA_START:
		toggle(1);
Log ("SYSTEM_EVENT_STA_START");
		break;
	case SYSTEM_EVENT_STA_CONNECTED:
		toggle(1);
		time_wifi_us = gettimeofday_us() - time_wifi_us;
{
		wifi_ap_record_t wifidata;
		DbgR (esp_wifi_sta_get_ap_info(&wifidata));
		rssi = wifidata.rssi;
Log("SYSTEM_EVENT_STA_CONNECTED rssi=%d", rssi);
}
		break;
	case SYSTEM_EVENT_STA_GOT_IP:
		toggle(1);
{
char ip[16], nm[16], gw[16];
Log ("SYSTEM_EVENT_STA_GOT_IP ip=%s nm=%s gw=%s",
	ip4addr_ntoa_r(&event->event_info.got_ip.ip_info.ip, ip, sizeof(ip)),
	ip4addr_ntoa_r(&event->event_info.got_ip.ip_info.netmask, nm, sizeof(nm)),
	ip4addr_ntoa_r(&event->event_info.got_ip.ip_info.gw, gw, sizeof(gw)));
}
Log("xEventGroupClearBits");
		xEventGroupClearBits(event_group, NO_WIFI);
Log("xEventGroupSetBits");
		xEventGroupSetBits(event_group, HAVE_WIFI);
		break;
	case SYSTEM_EVENT_STA_DISCONNECTED:
Log ("SYSTEM_EVENT_STA_DISCONNECTED");
		xEventGroupClearBits(event_group, HAVE_WIFI);

		if (sent || ++retry_count > 1)
			xEventGroupSetBits(event_group, NO_WIFI);
		else {
Log ("esp_wifi_connect");
			DbgR (esp_wifi_connect());	// try once again
		}
		break;
	default:
Log ("ignoring SYSTEM_EVENT %d", event->event_id);
		break;
	}

	return ESP_OK;
}

static esp_err_t wifi_setup(void)
{
#if 000		// no effect
Log ("esp_phy_load_cal_and_init");
	esp_phy_load_cal_and_init();	// no effect
#endif
	
Log ("tcpip_adapter_init");
	tcpip_adapter_init();

	DbgR (set_ip());

Log ("esp_event_loop_init");
	DbgR (esp_event_loop_init(event_handler, NULL));

Log ("esp_wifi_init");
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	DbgR (esp_wifi_init(&cfg));

	if (reset_reason != DEEPSLEEP_RESET) {	// this should be saved in flash
Log ("esp_wifi_set_mode");
		DbgR (esp_wifi_set_mode(WIFI_MODE_STA));

		wifi_config_t wifi_config = {
			.sta = {
				.ssid     = AP_SSID,
				.password = AP_PASS,
				.bssid_set = 0
			},
		};
Log ("esp_wifi_set_config");
		DbgR (esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
	}

Log ("esp_wifi_start");
	time_wifi_us = gettimeofday_us();
	DbgR (esp_wifi_start());

Log ("esp_wifi_connect");
	DbgR (esp_wifi_connect());

	return ESP_OK;
}

static esp_err_t app (void)
{
	EventBits_t bits;
	char message[500];
	int mlen;

Log("do_readings");
	(void)do_readings();

Log("xEventGroupWaitBits");
	xEventGroupWaitBits(event_group, HAVE_WIFI|NO_WIFI,
		false, false, WIFI_TIMEOUT_MS / portTICK_PERIOD_MS);
	bits = xEventGroupGetBits (event_group);
	if (NO_WIFI & bits) {
Log ("no WiFi, aborting");
		return ESP_FAIL;
	}
Log ("have WiFi");

// need to do this late to have wifi timing
Log ("format_message");
	mlen = format_message (message, sizeof(message));

Log ("send_message");
	send_message (message, mlen);
Log ("sent message");
	sent = 1;

	return ESP_OK;
}

static void main_task(void *param)
{
	esp_err_t ret;

Log ("xEventGroupCreate");
	event_group = xEventGroupCreate();

	Dbg (wifi_setup ());
	if (ESP_OK == ret) {
		Dbg (app());

		finish ();
	}

	while (1) vTaskDelay(5000/portTICK_PERIOD_MS);	// in 5s we will sleep anyway
}

void app_main ()
{
	app_start_us = gettimeofday_us();
	app_start_ticks = rtc_time_get ();
	app_start.tv_sec  = app_start_us / 1000000;
	app_start.tv_usec = app_start_us % 1000000;

#if TOGGLE_PIN >= 0
	gpio_pad_select_gpio(TOGGLE_PIN);
	gpio_set_direction(TOGGLE_PIN, GPIO_MODE_INPUT);
	gpio_pullup_en(TOGGLE_PIN);
#endif

#if DBG_PIN >= 0
	gpio_pad_select_gpio(DBG_PIN);
	gpio_set_direction(DBG_PIN, GPIO_MODE_INPUT);
	gpio_pullup_en(DBG_PIN);
#endif

// leave a gap between setting the pins up and reading them.

#if TOGGLE_PIN >= 0
	do_toggle = gpio_get_level(TOGGLE_PIN);
#endif
	if (do_toggle)
		toggle_setup();

#if DBG_PIN >= 0
	do_log = gpio_get_level(DBG_PIN);
#endif
// turn off internal messages below ERROR level
	if (!do_log) esp_log_level_set("*", ESP_LOG_ERROR);

	Log ("app built at %s %s", __DATE__, __TIME__);

//Log ("app_main start portTICK_PERIOD_MS=%d sizeof(int)=%d sizeof(long)=%d",
//	portTICK_PERIOD_MS, sizeof(int), sizeof(long));

//	Log ("Chip revision %d MAC %012llx", getChipRevision(), getChipMAC());
	Log ("Chip revision %d", getChipRevision());

	wakeup_cause = rtc_get_wakeup_cause();
	reset_reason = rtc_get_reset_reason(0);

// do we need a RTC_DATA_ATTR magic?

	xTaskCreate(main_task, "udp", 10240, NULL, 20, NULL);
//	xTaskCreatePinnedToCore(main_task, "udp", 10240, NULL, 20, NULL, 1);
		// or use tskNO_AFFINITY
}

