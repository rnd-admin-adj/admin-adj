#include "stm32f4xx.h"
#include "w5500.h"
#include "spi2.h"

/* Socket states */
#define SOCK_ESTABLISHED  0x17
#define SOCK_LISTEN       0x14
#define SOCK_INIT         0x13
#define SOCK_CLOSE_WAIT   0x1C
#define SOCK_CLOSED       0x00
#define HTTP_SOCK   3     
#define HTTP_PORT   80     

//Network Config  
static uint8_t g_mac[6] = {0x00, 0x08, 0xDC, 0x01, 0x02, 0x03};
static uint8_t g_ip[4]  = {192, 168, 1, 200};   /* STM32 ka fixed IP - router DHCP  */
static uint8_t g_sn[4]  = {255, 255, 255, 0};   /* Subnet mask */
static uint8_t g_gw[4]  = {192, 168, 1, 1};     /* Router IP */

static uint8_t g_server_ip[4] = {192, 168, 1, 104};  /* Server PC  fixed IP */
static uint16_t g_server_port = 5000;


static char g_last_gps_data[160] = "No data yet\r\n";
static int http_server_started = 0;

static void W5500_Reset(void)
{
    GPIOB->ODR &= ~(1U << 3);   /* RSTn LOW - PB3 */
    for (volatile int i = 0; i < 100000; i++);   /* >=500us hold */

    GPIOB->ODR |= (1U << 3);    /* RSTn HIGH - PB3 */
    for (volatile int i = 0; i < 200000; i++);   /* chip stabilize hone do */
}

/* -------------------------------------------------
   Low-level SPI helpers
------------------------------------------------- */
static void W5500_Write(uint16_t addr, uint8_t block, uint8_t data)
{
    W5500_CS_LOW();
    SPI2_Transfer(addr >> 8);
    SPI2_Transfer(addr & 0xFF);
    SPI2_Transfer(block | 0x04);   // write
    SPI2_Transfer(data);
    W5500_CS_HIGH();
}

static uint8_t W5500_Read(uint16_t addr, uint8_t block)
{
    uint8_t val;
    W5500_CS_LOW();
    SPI2_Transfer(addr >> 8);
    SPI2_Transfer(addr & 0xFF);
    SPI2_Transfer(block);          // read
    val = SPI2_Transfer(0xFF);
    W5500_CS_HIGH();
    return val;
}

/* -------------------------------------------------
   Command wait 
------------------------------------------------- */
static int W5500_WaitCommand(uint8_t sock)
{
    uint8_t block = 0x08 | (sock << 5);
    uint32_t timeout = 500000;

    while (W5500_Read(0x0001, block) != 0)
    {
        if (--timeout == 0)
            return -1;   
    }
    return 0;
}


static int W5500_WaitStatus(uint8_t sock, uint8_t expected_status, uint32_t timeout)
{
    while (W5500_GetSocketStatus(sock) != expected_status)
    {
        if (--timeout == 0)
            return -1;
    }
    return 0;
}

/* -------------------------------------------------
   Basic
------------------------------------------------- */
uint8_t W5500_ReadVersion(void)
{
    return W5500_Read(0x0039, 0x00);   // VERSIONR, common reg
}

uint8_t W5500_GetPHYStatus(void)
{
    return W5500_Read(0x002E, 0x00) & 0x01;  // PHYCFGR, common reg (bit0 = link up)
}

/* -------------------------------------------------
   Network config
------------------------------------------------- */
void W5500_SetNetwork(uint8_t *mac, uint8_t *ip,
                      uint8_t *sn, uint8_t *gw)
{
    for (int i = 0; i < 6; i++) W5500_Write(0x0009 + i, 0x00, mac[i]); // SHAR
    for (int i = 0; i < 4; i++) W5500_Write(0x000F + i, 0x00, ip[i]);  // SIPR
    for (int i = 0; i < 4; i++) W5500_Write(0x0005 + i, 0x00, sn[i]);  // SUBR
    for (int i = 0; i < 4; i++) W5500_Write(0x0001 + i, 0x00, gw[i]);  // GAR
}

/* -------------------------------------------------
   FULL INIT — reset + PHY link check + network config
   Return: 0 = success, -1 = chip not responding, -2 = link down
------------------------------------------------- */
int W5500_Init(void)
{
    W5500_Reset();

    if (W5500_ReadVersion() != 0x04)
        return -1;

    uint32_t link_timeout = 2000000;
    while (!W5500_GetPHYStatus())
    {
        if (--link_timeout == 0)
            return -2;  
    }

    W5500_SetNetwork(g_mac, g_ip, g_sn, g_gw);

    return 0;
}

int W5500_TCP_Server_Init(uint8_t sock, uint16_t port)
{
    uint8_t block = 0x08 | (sock << 5);

    W5500_Write(0x0001, block, 0x10);      // CLOSE
    if (W5500_WaitCommand(sock) != 0) return -1;

    W5500_Write(0x0000, block, 0x01);      // TCP Mode

    W5500_Write(0x0004, block, port >> 8);
    W5500_Write(0x0005, block, port & 0xFF);

    W5500_Write(0x0001, block, 0x01);      // OPEN
    if (W5500_WaitCommand(sock) != 0) return -1;

    if (W5500_WaitStatus(sock, SOCK_INIT, 500000) != 0) return -1;

    W5500_Write(0x0001, block, 0x02);      // LISTEN
    if (W5500_WaitCommand(sock) != 0) return -1;

    if (W5500_WaitStatus(sock, SOCK_LISTEN, 500000) != 0) return -1;

    return 0;
}


int W5500_TCP_Client_Connect(uint8_t sock, uint8_t *server_ip, uint16_t port)
{
    uint8_t block = 0x08 | (sock << 5);

    W5500_Write(0x0001, block, 0x10);      
    W5500_WaitCommand(sock);

    W5500_Write(0x0000, block, 0x01);          // Sn_MR = TCP

    uint16_t local_port = 50000 + sock;
    W5500_Write(0x0004, block, local_port >> 8);
    W5500_Write(0x0005, block, local_port & 0xFF);

    for (int i = 0; i < 4; i++)
        W5500_Write(0x000C + i, block, server_ip[i]); // Sn_DIPR

    W5500_Write(0x0010, block, port >> 8);     // Sn_DPORT
    W5500_Write(0x0011, block, port & 0xFF);

    W5500_Write(0x0001, block, 0x01);          // OPEN
    if (W5500_WaitCommand(sock) != 0)
        return -1;

    if (W5500_WaitStatus(sock, SOCK_INIT, 500000) != 0)
        return -1;

    W5500_Write(0x0001, block, 0x04);          // CONNECT
    if (W5500_WaitCommand(sock) != 0)
        return -1;

    if (W5500_WaitStatus(sock, SOCK_ESTABLISHED, 2000000) != 0)
        return -1;

    return 0;   /* connected! */
}

/* -------------------------------------------------
   Socket status
------------------------------------------------- */
uint8_t W5500_GetSocketStatus(uint8_t sock)
{
    uint8_t block = 0x08 | (sock << 5);
    return W5500_Read(0x0003, block);          // Sn_SR
}

/* -------------------------------------------------
   CLOSE SOCKET
------------------------------------------------- */
void W5500_CloseSocket(uint8_t sock)
{
    uint8_t block = 0x08 | (sock << 5);
    W5500_Write(0x0001, block, 0x10);      // CLOSE Command
    W5500_WaitCommand(sock);
}

/* -------------------------------------------------
   RECV / SEND 
------------------------------------------------- */
int W5500_Recv(uint8_t sock, uint8_t *buf, uint16_t maxlen)
{
    uint16_t rx_size1, rx_size2;
    uint16_t rx_rd;
    uint16_t offset;
    uint16_t i;

    uint8_t sock_block = 0x08 | (sock << 5);
    uint8_t rx_block   = 0x18 | (sock << 5);

    do {
        rx_size1  = W5500_Read(0x0026, sock_block) << 8;
        rx_size1 |= W5500_Read(0x0027, sock_block);

        rx_size2  = W5500_Read(0x0026, sock_block) << 8;
        rx_size2 |= W5500_Read(0x0027, sock_block);
    } while (rx_size1 != rx_size2);

    if (rx_size1 == 0)
        return 0;

    if (rx_size1 > maxlen)
        rx_size1 = maxlen;

    rx_rd  = W5500_Read(0x0028, sock_block) << 8;
    rx_rd |= W5500_Read(0x0029, sock_block);

    uint16_t rx_base = 0x6000 + (sock * 0x0800);

    for (i = 0; i < rx_size1; i++)
    {
        offset = (rx_rd + i) & 0x07FF;
        buf[i] = W5500_Read(rx_base + offset, rx_block);
    }

    rx_rd += rx_size1;
    W5500_Write(0x0028, sock_block, (rx_rd >> 8) & 0xFF);
    W5500_Write(0x0029, sock_block,  rx_rd & 0xFF);

    W5500_Write(0x0001, sock_block, 0x40);
    W5500_WaitCommand(sock);

    return rx_size1;
}

int W5500_Send(uint8_t sock, uint8_t *buf, uint16_t len)
{
    uint16_t tx_wr;
    uint16_t offset;
    uint16_t i;

    uint8_t sock_block = 0x08 | (sock << 5);
    uint8_t tx_block   = 0x10 | (sock << 5);

    tx_wr  = W5500_Read(0x0024, sock_block) << 8;
    tx_wr |= W5500_Read(0x0025, sock_block);

    uint16_t tx_base = 0x4000 + (sock * 0x0800);

    for (i = 0; i < len; i++)
    {
        offset = (tx_wr + i) & 0x07FF;
        W5500_Write(tx_base + offset, tx_block, buf[i]);
    }

    tx_wr += len;
    W5500_Write(0x0024, sock_block, (tx_wr >> 8) & 0xFF);
    W5500_Write(0x0025, sock_block,  tx_wr & 0xFF);

    W5500_Write(0x0001, sock_block, 0x20);
    if (W5500_WaitCommand(sock) != 0)
        return -1;

    return len;
}


   //MAIN TASK — GPS client 
int W5500_GPS_Client_Task(uint8_t sock, char *gps_line, uint16_t len)
{
    extern volatile uint32_t ms_ticks;
    static uint32_t last_reconnect_attempt = 0;

    uint8_t status = W5500_GetSocketStatus(sock);

    if (status == SOCK_ESTABLISHED)
    {
        if (W5500_Send(sock, (uint8_t *)gps_line, len) < 0)
        {
            W5500_CloseSocket(sock);
            return -1;
        }
        return 0;
    }
    else
    {
        /* Sirf har 2 second me ek baar reconnect try karo, har GPS sample par nahi */
        if ((ms_ticks - last_reconnect_attempt) >= 2000)
        {
            last_reconnect_attempt = ms_ticks;
            W5500_TCP_Client_Connect(sock, g_server_ip, g_server_port);
        }
        return -1;
    }
}



void W5500_Set_Last_Data(char *data, uint16_t len)
{
    if (len >= sizeof(g_last_gps_data))
        len = sizeof(g_last_gps_data) - 1;

    memcpy(g_last_gps_data, data, len);
    g_last_gps_data[len] = '\0';
}
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

        char http_response[400];
        int resp_len = snprintf(http_response, sizeof(http_response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<html><body style='font-family:monospace;font-size:20px;'>"
            "<h2>GPS Live Data</h2><pre>%s</pre>"
            "<meta http-equiv='refresh' content='3'>"
            "</body></html>",
            g_last_gps_data);

        W5500_Send(HTTP_SOCK, (uint8_t *)http_response, resp_len);

        /* NAYA: Busy-wait wala graceful close hataya — seedha abrupt close,
           lekin thoda delay diya taaki send complete ho jaye pehle */
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