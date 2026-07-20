#include "stm32f4xx.h"
#include "gps.h"
#include "usart_debug.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define GPS_PKT_SYNC1   0xAA
#define GPS_PKT_SYNC2   0x55
#define GPS_PKT_SIZE    14   /* 2 sync + 4 lat + 4 lon + 2 speed + 2 crc */

/* Extract a specific comma-separated field from an NMEA sentence
 *
 * Parameters:
 *   line   -> Pointer to full NMEA sentence (e.g. "$GPRMC,...")
 *   field  -> Field number to extract (0-based, excluding '$')
 *   out    -> Output buffer where the extracted field is stored
 *   maxlen -> Size of the output buffer
 */
static void get_nmea_field(const char *line, int field, char *out, int maxlen)
{
    /* Tracks which field number we are currently parsing */
    int current = 0;

    /* Index into the output buffer */
    int i = 0;

    /* If the sentence starts with '$', skip it
       (NMEA sentences always begin with '$') */
    if (*line == '$')
        line++;

    /* Move through the string until we reach the desired field */
    while (*line && current < field)
    {
        /* Every comma means we move to the next field */
        if (*line == ',')
            current++;

        /* Advance to next character */
        line++;
    }

    /* Copy characters of the desired field into output buffer */
    while (*line &&            // Stop if end of string
           *line != ',' &&     // Stop at next comma (end of field)
           i < maxlen - 1)     // Leave space for null terminator
    {
        out[i++] = *line++;   // Copy character and advance pointers
    }

    /* Null-terminate the output string */
    out[i] = '\0';
}




/* Convert NMEA latitude (ddmm.mmmm) to decimal degrees
 *
 * Example:
 *   NMEA latitude = "2835.09364"
 *   dd = 28 degrees
 *   mm.mmmm = 35.09364 minutes
 *
 * Decimal degrees = degrees + (minutes / 60)
 *                 = 28 + (35.09364 / 60)
 */
static double nmea_lat_to_decimal(const char *lat)
{
    /* Extract the degree portion (first two characters)
       lat[0] and lat[1] are ASCII digits */
    int deg = (lat[0] - '0') * 10 +
              (lat[1] - '0');

    /* Convert the minutes portion starting from lat[2]
       This converts "35.09364" to a floating-point value */
    double min = atof(&lat[2]);

    /* Convert minutes to fractional degrees and add to degrees */
    return deg + (min / 60.0);
}



/* Convert NMEA longitude (dddmm.mmmm) to decimal degrees
 *
 * Example:
 *   NMEA longitude = "07718.95917"
 *   ddd = 77 degrees
 *   mm.mmmm = 18.95917 minutes
 *
 * Decimal degrees = degrees + (minutes / 60)
 *                 = 77 + (18.95917 / 60)
 */
static double nmea_lon_to_decimal(const char *lon)
{
    /* Extract the degree portion (first three characters)
       lon[0], lon[1], lon[2] are ASCII digits */
    int deg = (lon[0] - '0') * 100 +
              (lon[1] - '0') * 10 +
              (lon[2] - '0');

    /* Convert the minutes portion starting from lon[3]
       This converts "18.95917" to a floating-point value */
    double min = atof(&lon[3]);

    /* Convert minutes to fractional degrees and add to degrees */
    return deg + (min / 60.0);
}

/* =========================================================
   RTC INITIALIZATION (LSI based)
   ========================================================= */

void gps_rtc_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_DBP;

    RCC->CSR |= RCC_CSR_LSION;
    while (!(RCC->CSR & RCC_CSR_LSIRDY));

    RCC->BDCR |= RCC_BDCR_RTCSEL_1;   // LSI selected
    RCC->BDCR |= RCC_BDCR_RTCEN;
}

void gps_usart1_init(void)
{
    /* GPS now wired to PC6 (TX) / PC7 (RX) -> USART6, AF8 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART6EN;

    /* PC6, PC7 AF8 */
    GPIOC->MODER &= ~((3U << (6 * 2)) | (3U << (7 * 2)));
    GPIOC->MODER |=  (2U << (6 * 2)) | (2U << (7 * 2));

    GPIOC->AFR[0] &= ~((0xF << 24) | (0xF << 28));
    GPIOC->AFR[0] |=  (8U << 24) | (8U << 28);

    /* 9600 baud @ 16 MHz APB2 */
    USART6->BRR = 0x0683;

    USART6->CR1 = USART_CR1_RE | USART_CR1_TE | USART_CR1_UE;
}

//CRC
static void put_i32_be(uint8_t *buf, int32_t val)
{
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8)  & 0xFF;
    buf[3] =  val        & 0xFF;
}

static void put_u16_be(uint8_t *buf, uint16_t val)
{
    buf[0] = (val >> 8) & 0xFF;
    buf[1] =  val        & 0xFF;
}

/* CRC16-CCITT (poly 0x1021, init 0xFFFF) — standard, EMI-heavy environments */
  static uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

static uint16_t build_gps_packet(uint8_t *out, int32_t lat_i, int32_t lon_i, uint16_t speed_i)
{
    out[0] = GPS_PKT_SYNC1;
    out[1] = GPS_PKT_SYNC2;
    put_i32_be(&out[2], lat_i);
    put_i32_be(&out[6], lon_i);
    put_u16_be(&out[10], speed_i);

    uint16_t crc = crc16_ccitt(&out[2], 10);  
    put_u16_be(&out[12], crc);

    return GPS_PKT_SIZE;
}
/* =========================================================
   GPS POLLING (call continuously)
   ========================================================= */

void gps_poll(void)
{
    static char line[128];
    static uint8_t idx = 0;
    static uint32_t last_print_ms = 0; 
    if (USART6->SR & USART_SR_RXNE)
    {
        char c = USART6->DR;

        if (c == '\n')
        {
            line[idx] = '\0';
            idx = 0;

            if (strstr(line, "RMC,") != NULL)
            {
                char status[2] = {0};
                char lat[16] = {0};
                char lon[16] = {0};
                char ns[2]  = {0};
                char ew[2]  = {0};
                char speed_knots[16] = {0};

                get_nmea_field(line, 2, status, sizeof(status));
                get_nmea_field(line, 3, lat, sizeof(lat));
                get_nmea_field(line, 4, ns, sizeof(ns));
                get_nmea_field(line, 5, lon, sizeof(lon));
                get_nmea_field(line, 6, ew, sizeof(ew));
                get_nmea_field(line, 7, speed_knots, sizeof(speed_knots));

                if (status[0] == 'A' && lat[0] && lon[0])
                {
                    char tx_buf[128];
                    if ((ms_ticks - last_print_ms) >= GPS_SAMPLE_INTERVAL_MS)
                    {
                        last_print_ms = ms_ticks;

                    double dlat = nmea_lat_to_decimal(lat);
                    double dlon = nmea_lon_to_decimal(lon);

                    int lat_i = (int)(dlat * 1000000);
                    int lon_i = (int)(dlon * 1000000);

                    double speed_kmph = atof(speed_knots) * 1.852;
                    int speed_int = (int)speed_kmph;
                    int speed_dec = (int)((speed_kmph - speed_int) * 100);
                    if (speed_dec < 0)
                        speed_dec = -speed_dec;

                    int tx_len = snprintf(tx_buf, sizeof(tx_buf),
                                "LAT: %d.%06d %s LAT: %d.%06d %s Speed: %d.%02d km/h\r\n",
                                lat_i / 1000000, lat_i % 1000000, ns,
                                lon_i / 1000000, lon_i % 1000000, ew,
                            speed_int, speed_dec);

                    /* Debug print (USART2) */
                    usart_debug("%s", tx_buf);
                    
                    uint16_t speed_i = (uint16_t)(speed_kmph * 100);
                    uint8_t bin_pkt[GPS_PKT_SIZE];
                    uint16_t pkt_len = build_gps_packet(bin_pkt, lat_i, lon_i, speed_i);
                    char hex_dbg[64];
                    int hpos = 0;
                    for (int b = 0; b < pkt_len; b++)
                        hpos += snprintf(&hex_dbg[hpos], sizeof(hex_dbg) - hpos, "%02X ", bin_pkt[b]);
                    usart_debug("BIN: %s\r\n", hex_dbg);

                    W5500_GPS_Client_Task(0, (char *)bin_pkt, pkt_len);
                    W5500_Set_Last_Data(tx_buf, tx_len);  
                    }
                }
            }
        }
        else if (idx < sizeof(line) - 1)
        {
            line[idx++] = c;
        }
    }
}

/* ---- Low-level UART send helper ---- */
static void gps_uart_send(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++)
    {
        while (!(USART6->SR & USART_SR_TXE));
        USART6->DR = data[i];
    }
}

static void gps_uart_set_baud(uint16_t brr_value)
{
    USART6->CR1 &= ~USART_CR1_UE;
    USART6->BRR = brr_value;
    USART6->CR1 |= USART_CR1_UE;
}

static const uint8_t ubx_set_baud_38400[] = {
    0xB5, 0x62, 0x06, 0x00, 0x14, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0xD0, 0x08, 0x00, 0x00,
    0x00, 0x96, 0x00, 0x00,
    0x07, 0x00,
    0x03, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x93, 0x90
};

static const uint8_t ubx_disable_gga[] = {0xB5,0x62,0x06,0x01,0x03,0x00,0xF0,0x00,0x00,0xFA,0x0F};
static const uint8_t ubx_disable_gsa[] = {0xB5,0x62,0x06,0x01,0x03,0x00,0xF0,0x02,0x00,0xFC,0x13};
static const uint8_t ubx_disable_gsv[] = {0xB5,0x62,0x06,0x01,0x03,0x00,0xF0,0x03,0x00,0xFD,0x15};
static const uint8_t ubx_disable_vtg[] = {0xB5,0x62,0x06,0x01,0x03,0x00,0xF0,0x05,0x00,0xFF,0x19};

static const uint8_t ubx_rate_5hz[] = {
    0xB5, 0x62, 0x06, 0x08, 0x06, 0x00,
    0xC8, 0x00,
    0x01, 0x00,
    0x01, 0x00,
    0xDE, 0x6A
};

void gps_configure(void)
{
    gps_uart_send(ubx_set_baud_38400, sizeof(ubx_set_baud_38400));
    for (volatile int i = 0; i < 50000; i++);

    gps_uart_set_baud(0x1A1);
    for (volatile int i = 0; i < 50000; i++);

    gps_uart_send(ubx_disable_gga, sizeof(ubx_disable_gga));
    gps_uart_send(ubx_disable_gsa, sizeof(ubx_disable_gsa));
    gps_uart_send(ubx_disable_gsv, sizeof(ubx_disable_gsv));
    gps_uart_send(ubx_disable_vtg, sizeof(ubx_disable_vtg));
    gps_uart_send(ubx_rate_5hz, sizeof(ubx_rate_5hz));
}

