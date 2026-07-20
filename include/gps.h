#ifndef GPS_H
#define GPS_H

#include <stdint.h>

#define GPS_SAMPLE_HZ           5
#define GPS_SAMPLE_INTERVAL_MS  (1000 / GPS_SAMPLE_HZ)   /* 250 ms -> 4 samples/sec */

extern volatile uint32_t ms_ticks;   /* defined in main.c */
void gps_configure(void);
void gps_usart1_init(void);
void gps_poll(void); 
void gps_rtc_init(void);
void gps_print_rtc(void);
void gps_configure(void);

#endif