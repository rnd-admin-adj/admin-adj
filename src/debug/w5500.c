/* =====================================================================
   W5500 ETHERNET DRIVER
   -----------------------------------------------------------------
   Low-level SPI driver for the WIZnet W5500 Ethernet controller,
   plus a simple TCP client (used to push encoder data to the server)
   and a minimal TCP-based HTTP status server (for browser monitoring).

   Timing note: this file uses xTaskGetTickCount() (FreeRTOS tick
   count) instead of a manual millisecond counter, since
   configTICK_RATE_HZ = 1000 makes 1 tick = 1 ms.
   ===================================================================== */

#include "stm32f4xx.h"
#include "w5500.h"
#include "spi2.h"
#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

/* ================================================================
   W5500 Socket Status Codes
   ================================================================ */
#define SOCK_ESTABLISHED  0x17
#define SOCK_LISTEN       0x14
#define SOCK_INIT         0x13
#define SOCK_CLOSE_WAIT   0x1C
#define SOCK_CLOSED       0x00

#define SOCK_FIN_WAIT     0x18
#define SOCK_CLOSING      0x1A
#define SOCK_TIME_WAIT    0x1B
#define SOCK_LAST_ACK     0x1D

#define HTTP_SOCK         3
#define HTTP_PORT         80

#define SENSOR_SOCK       0
#define SENSOR_PORT       5000

/* ================================================================
   Project Identification (shown on the HTTP status page)
   ================================================================ */
#define COMPANY_NAME   "ADJ Engineering Pvt Ltd"
#define PROJECT_NAME   "UABAMS - Railway Axle Box Odometer"

/* ================================================================
   NETWORK CONFIGURATION
   ================================================================ */

/* This device (STM32 + encoder) network identity */
static uint8_t g_mac[6] = { 0x00, 0x08, 0xDC, 0x01, 0x02, 0x11 };
static uint8_t g_ip[4]  = { 192, 168, 1, 211 };
static uint8_t g_sn[4]  = { 255, 255, 255, 0 };
static uint8_t g_gw[4]  = { 192, 168, 1, 1 };

/* Destination server (where encoder data is sent) */
static uint8_t  g_server_ip[4]  = { 192, 168, 1, 104 };
static uint16_t g_server_port   = 5000;

/* ================================================================
   Sensor data cache / HTTP status tracking
   ================================================================ */
static char     g_last_sensor_data[256] = "No data yet\r\n";
static int      http_server_started     = 0;
static uint32_t g_last_data_time        = 0;

/* ================================================================
   W5500 HARDWARE RESET (RSTn pin = PB3)
   ================================================================ */
static void W5500_Reset(void)
{
    GPIOB->ODR &= ~(1U << 3);            /* Hold reset low */
    for (volatile int i = 0; i < 100000; i++);

    GPIOB->ODR |= (1U << 3);             /* Release reset */
    for (volatile int i = 0; i < 200000; i++);
}

/* ================================================================
   LOW-LEVEL SPI WRITE (single byte to a W5500 register)
   ================================================================ */
static void W5500_Write(uint16_t addr, uint8_t block, uint8_t data)
{
    W5500_CS_LOW();

    SPI2_Transfer(addr >> 8);
    SPI2_Transfer(addr & 0xFF);
    SPI2_Transfer(block | 0x04);          /* 0x04 = write flag */
    SPI2_Transfer(data);

    W5500_CS_HIGH();
}

/* ================================================================
   LOW-LEVEL SPI READ (single byte from a W5500 register)
   ================================================================ */
static uint8_t W5500_Read(uint16_t addr, uint8_t block)
{
    uint8_t val;

    W5500_CS_LOW();

    SPI2_Transfer(addr >> 8);
    SPI2_Transfer(addr & 0xFF);
    SPI2_Transfer(block);                 /* read flag bit not set */
    val = SPI2_Transfer(0xFF);

    W5500_CS_HIGH();

    return val;
}

/* ================================================================
   WAIT FOR SOCKET COMMAND REGISTER TO CLEAR (command completed)
   ================================================================ */
static int W5500_WaitCommand(uint8_t sock)
{
    uint8_t  block   = 0x08 | (sock << 5);
    uint32_t timeout = 500000;

    while (W5500_Read(0x0001, block) != 0)
    {
        if (--timeout == 0)
            return -1;
    }

    return 0;
}

/* ================================================================
   WAIT FOR SOCKET TO REACH AN EXPECTED STATUS
   ================================================================ */
static int W5500_WaitStatus(uint8_t sock, uint8_t expected_status, uint32_t timeout)
{
    while (W5500_GetSocketStatus(sock) != expected_status)
    {
        if (--timeout == 0)
            return -1;
    }

    return 0;
}

/* ================================================================
   READ CHIP VERSION REGISTER (should read 0x04 for W5500)
   ================================================================ */
uint8_t W5500_ReadVersion(void)
{
    return W5500_Read(0x0039, 0x00);
}

/* ================================================================
   READ PHY LINK STATUS (bit 0: 1 = link up, 0 = link down)
   ================================================================ */
uint8_t W5500_GetPHYStatus(void)
{
    return W5500_Read(0x002E, 0x00) & 0x01;
}

/* ================================================================
   WRITE NETWORK CONFIGURATION (MAC / IP / Subnet / Gateway)
   ================================================================ */
void W5500_SetNetwork(uint8_t *mac, uint8_t *ip, uint8_t *sn, uint8_t *gw)
{
    int i;

    for (i = 0; i < 6; i++) W5500_Write(0x0009 + i, 0x00, mac[i]);
    for (i = 0; i < 4; i++) W5500_Write(0x000F + i, 0x00, ip[i]);
    for (i = 0; i < 4; i++) W5500_Write(0x0005 + i, 0x00, sn[i]);
    for (i = 0; i < 4; i++) W5500_Write(0x0001 + i, 0x00, gw[i]);
}

/* ================================================================
   W5500 INITIALIZATION
   Returns: 0 = OK, -1 = chip not responding, -2 = link down
   ================================================================ */
int W5500_Init(void)
{
    uint32_t link_timeout = 2000000;

    W5500_Reset();

    /* Confirm the chip is present and responding over SPI */
    if (W5500_ReadVersion() != 0x04)
        return -1;

    /* Wait for the Ethernet cable link to come up */
    while (!W5500_GetPHYStatus())
    {
        if (--link_timeout == 0)
            return -2;
    }

    /* Apply MAC/IP/Subnet/Gateway configuration */
    W5500_SetNetwork(g_mac, g_ip, g_sn, g_gw);

    return 0;
}

/* ================================================================
   TCP CLIENT: OPEN AND CONNECT A SOCKET TO THE SERVER
   ================================================================ */
int W5500_TCP_Client_Connect(uint8_t sock, uint8_t *server_ip, uint16_t port)
{
    uint8_t  block      = 0x08 | (sock << 5);
    uint16_t local_port = 50000 + sock;

    /* Make sure the socket starts from a clean (closed) state */
    W5500_Write(0x0001, block, 0x10);   /* CLOSE command */
    W5500_WaitCommand(sock);

    /* Configure as TCP */
    W5500_Write(0x0000, block, 0x01);

    /* Local (source) port */
    W5500_Write(0x0004, block, local_port >> 8);
    W5500_Write(0x0005, block, local_port & 0xFF);

    /* Destination server IP */
    for (int i = 0; i < 4; i++)
        W5500_Write(0x000C + i, block, server_ip[i]);

    /* Destination server port */
    W5500_Write(0x0010, block, port >> 8);
    W5500_Write(0x0011, block, port & 0xFF);

    /* OPEN the socket */
    W5500_Write(0x0001, block, 0x01);
    if (W5500_WaitCommand(sock) != 0)
        return -1;

    if (W5500_WaitStatus(sock, SOCK_INIT, 500000) != 0)
        return -1;

    /* CONNECT to the server */
    W5500_Write(0x0001, block, 0x04);
    if (W5500_WaitCommand(sock) != 0)
        return -1;

    /* Wait for the TCP handshake to complete */
    if (W5500_WaitStatus(sock, SOCK_ESTABLISHED, 2000000) != 0)
        return -1;

    return 0;
}

/* ================================================================
   READ CURRENT SOCKET STATUS
   ================================================================ */
uint8_t W5500_GetSocketStatus(uint8_t sock)
{
    uint8_t block = 0x08 | (sock << 5);
    return W5500_Read(0x0003, block);
}

/* ================================================================
   CLOSE A SOCKET
   ================================================================ */
void W5500_CloseSocket(uint8_t sock)
{
    uint8_t block = 0x08 | (sock << 5);
    W5500_Write(0x0001, block, 0x10);
    W5500_WaitCommand(sock);
}

/* ================================================================
   RECEIVE DATA FROM A SOCKET'S RX BUFFER
   ================================================================ */
int W5500_Recv(uint8_t sock, uint8_t *buf, uint16_t maxlen)
{
    uint16_t rx_size1, rx_size2, rx_rd, offset, i;
    uint8_t  sock_block = 0x08 | (sock << 5);
    uint8_t  rx_block   = 0x18 | (sock << 5);

    /* RX size must be read twice and match, to avoid a race with
       incoming data changing the size mid-read */
    do
    {
        rx_size1  = W5500_Read(0x0026, sock_block) << 8;
        rx_size1 |= W5500_Read(0x0027, sock_block);

        rx_size2  = W5500_Read(0x0026, sock_block) << 8;
        rx_size2 |= W5500_Read(0x0027, sock_block);
    }
    while (rx_size1 != rx_size2);

    if (rx_size1 == 0)
        return 0;

    if (rx_size1 > maxlen)
        rx_size1 = maxlen;

    /* Current RX read pointer */
    rx_rd  = W5500_Read(0x0028, sock_block) << 8;
    rx_rd |= W5500_Read(0x0029, sock_block);

    uint16_t rx_base = 0x6000 + (sock * 0x0800);

    for (i = 0; i < rx_size1; i++)
    {
        offset = (rx_rd + i) & 0x07FF;    /* circular buffer wrap */
        buf[i] = W5500_Read(rx_base + offset, rx_block);
    }

    /* Advance the RX pointer past the data we just read */
    rx_rd += rx_size1;
    W5500_Write(0x0028, sock_block, (rx_rd >> 8) & 0xFF);
    W5500_Write(0x0029, sock_block, rx_rd & 0xFF);

    /* RECV command tells the chip we're done consuming this data */
    W5500_Write(0x0001, sock_block, 0x40);
    W5500_WaitCommand(sock);

    return rx_size1;
}

/* ================================================================
   SEND DATA OUT OF A SOCKET'S TX BUFFER
   ================================================================ */
int W5500_Send(uint8_t sock, uint8_t *buf, uint16_t len)
{
    uint16_t tx_wr, offset, i;
    uint8_t  sock_block = 0x08 | (sock << 5);
    uint8_t  tx_block   = 0x10 | (sock << 5);

    /* Current TX write pointer */
    tx_wr  = W5500_Read(0x0024, sock_block) << 8;
    tx_wr |= W5500_Read(0x0025, sock_block);

    uint16_t tx_base = 0x4000 + (sock * 0x0800);

    for (i = 0; i < len; i++)
    {
        offset = (tx_wr + i) & 0x07FF;    /* circular buffer wrap */
        W5500_Write(tx_base + offset, tx_block, buf[i]);
    }

    /* Advance the TX pointer past the data we just wrote */
    tx_wr += len;
    W5500_Write(0x0024, sock_block, (tx_wr >> 8) & 0xFF);
    W5500_Write(0x0025, sock_block, tx_wr & 0xFF);

    /* SEND command actually transmits the buffered data */
    W5500_Write(0x0001, sock_block, 0x20);

    if (W5500_WaitCommand(sock) != 0)
        return -1;

    return len;
}

/* ================================================================
   SENSOR TCP CLIENT TASK
   Call this periodically (e.g. every 100 ms) with the latest
   encoder data line. Handles connect / reconnect automatically.
   Returns: 0 = sent OK, -1 = not connected / send failed

   Uses xTaskGetTickCount() for the reconnect-retry timer, since
   configTICK_RATE_HZ = 1000 makes 1 tick = 1 ms.
   ================================================================ */
int W5500_Sensor_Client_Task(uint8_t sock, char *data_line, uint16_t len)
{
    static TickType_t last_reconnect_attempt = 0;

    uint8_t status = W5500_GetSocketStatus(sock);

    switch (status)
    {
        case SOCK_ESTABLISHED:
            /* Connection is up - try to send */
            if (W5500_Send(sock, (uint8_t *)data_line, len) < 0)
            {
                /* Send failed even though status said ESTABLISHED -
                   force a clean close so the next cycle reconnects */
                W5500_CloseSocket(sock);
                return -1;
            }
            return 0;

        case SOCK_CLOSE_WAIT:
        case SOCK_FIN_WAIT:
        case SOCK_CLOSING:
        case SOCK_TIME_WAIT:
        case SOCK_LAST_ACK:
            /* Server closed the connection (or it's in the process
               of closing). Force-close our side immediately so we
               don't wait out a lingering TCP teardown state, then
               fall through to the reconnect-attempt logic below. */
            W5500_CloseSocket(sock);
            break;

        case SOCK_CLOSED:
        case SOCK_INIT:
        default:
            /* Nothing to do here except attempt to reconnect below */
            break;
    }

    /* Not connected - retry every 2 seconds, don't hammer the connect */
    if ((xTaskGetTickCount() - last_reconnect_attempt) >= pdMS_TO_TICKS(2000))
    {
        last_reconnect_attempt = xTaskGetTickCount();
        W5500_TCP_Client_Connect(sock, g_server_ip, g_server_port);
    }

    return -1;
}
/* ================================================================
   CACHE THE LAST SUCCESSFULLY SENT SENSOR DATA
   (used by the HTTP status page to show live data + freshness)
   ================================================================ */
void W5500_Set_Last_Data(char *data, uint16_t len)
{
    if (len >= sizeof(g_last_sensor_data))
        len = sizeof(g_last_sensor_data) - 1;

    memcpy(g_last_sensor_data, data, len);
    g_last_sensor_data[len] = '\0';

    g_last_data_time = xTaskGetTickCount();
}

/* ================================================================
   MINIMAL HTTP STATUS SERVER
   Serves a single auto-refreshing page showing the latest encoder
   data and a live/stale health indicator. Call periodically from
   a task loop.
   ================================================================ */
void W5500_HTTP_Server_Task(void)
{
    if (!http_server_started)
    {
        if (W5500_TCP_Server_Init(HTTP_SOCK, HTTP_PORT) == 0)
            http_server_started = 1;
        else
            return;
    }

    uint8_t status = W5500_GetSocketStatus(HTTP_SOCK);

    if (status == SOCK_ESTABLISHED)
    {
        uint8_t rxbuf[256];
        W5500_Recv(HTTP_SOCK, rxbuf, sizeof(rxbuf) - 1);

        /* Reformat the CSV-style sensor data into readable lines */
        char display_buf[300];
        int  j = 0;

        for (int i = 0; g_last_sensor_data[i] != '\0' && j < (int)sizeof(display_buf) - 1; i++)
        {
            char ch = g_last_sensor_data[i];

            if (ch == ',')
                display_buf[j++] = '\n';
            else if (ch == '\r' || ch == '\n')
                continue;
            else
                display_buf[j++] = ch;
        }
        display_buf[j] = '\0';

        /* Determine health based on how recently data was received */
        uint32_t data_age = xTaskGetTickCount() - g_last_data_time;
        const char *health_text;
        const char *health_color;

        if (g_last_data_time == 0)
        {
            health_text  = "NO DATA RECEIVED YET";
            health_color = "red";
        }
        else if (data_age < pdMS_TO_TICKS(2000))
        {
            health_text  = "ENCODER OK (LIVE)";
            health_color = "green";
        }
        else
        {
            health_text  = "ENCODER NOT RESPONDING";
            health_color = "red";
        }

        char http_response[900];
        int resp_len = snprintf(http_response, sizeof(http_response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<html><body style='font-family:monospace;font-size:18px;"
            "background:#111;color:#eee;padding:20px;'>"
            "<h2>%s</h2><h4>%s</h4>"
            "<p style='color:%s;font-weight:bold;'>Status: %s</p>"
            "<hr>"
            "<pre style='font-size:20px;line-height:1.6;'>%s</pre>"
            "<meta http-equiv='refresh' content='2'>"
            "</body></html>",
            COMPANY_NAME, PROJECT_NAME, health_color, health_text, display_buf);

        W5500_Send(HTTP_SOCK, (uint8_t *)http_response, resp_len);

        /* Small delay so the client has time to read before we close */
        for (volatile int i = 0; i < 20000; i++);

        W5500_CloseSocket(HTTP_SOCK);
        http_server_started = 0;
    }
    else if (status == SOCK_CLOSE_WAIT)
    {
        W5500_CloseSocket(HTTP_SOCK);
        http_server_started = 0;
    }
}

/* ================================================================
   TCP SERVER: OPEN A SOCKET AND PUT IT INTO LISTEN MODE
   Used by the HTTP status server so a browser can connect to us.
   Returns: 0 = listening OK, -1 = failed to open/listen
   ================================================================ */
int W5500_TCP_Server_Init(uint8_t sock, uint16_t port)
{
    uint8_t block = 0x08 | (sock << 5);

    /* Make sure the socket starts from a clean (closed) state */
    W5500_Write(0x0001, block, 0x10);   /* CLOSE command */
    W5500_WaitCommand(sock);

    /* Configure as TCP */
    W5500_Write(0x0000, block, 0x01);

    /* Local (listening) port */
    W5500_Write(0x0004, block, port >> 8);
    W5500_Write(0x0005, block, port & 0xFF);

    /* OPEN the socket */
    W5500_Write(0x0001, block, 0x01);
    if (W5500_WaitCommand(sock) != 0)
        return -1;

    if (W5500_WaitStatus(sock, SOCK_INIT, 500000) != 0)
        return -1;

    /* LISTEN - wait for an incoming client connection */
    W5500_Write(0x0001, block, 0x02);
    if (W5500_WaitCommand(sock) != 0)
        return -1;

    if (W5500_WaitStatus(sock, SOCK_LISTEN, 500000) != 0)
        return -1;

    return 0;
}