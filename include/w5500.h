#ifndef W5500_H
#define W5500_H

#include <stdint.h>

/* ===========================================================
   SOCKET STATUS
   =========================================================== */

#define SOCK_CLOSED        0x00
#define SOCK_INIT          0x13
#define SOCK_LISTEN        0x14
#define SOCK_ESTABLISHED   0x17
#define SOCK_CLOSE_WAIT    0x1C

/* ===========================================================
   BASIC
   =========================================================== */

uint8_t W5500_ReadVersion(void);
uint8_t W5500_GetPHYStatus(void);

/* ===========================================================
   NETWORK
   =========================================================== */

void W5500_SetNetwork(uint8_t *mac,
                      uint8_t *ip,
                      uint8_t *sn,
                      uint8_t *gw);

/* ===========================================================
   TCP
   =========================================================== */

/* NAYA: */
int W5500_TCP_Server_Init(uint8_t sock, uint16_t port);
int W5500_TCP_Client_Connect(uint8_t sock, uint8_t *server_ip, uint16_t port);
int W5500_Init(void);
int W5500_GPS_Client_Task(uint8_t sock, char *gps_line, uint16_t len);

void W5500_CloseSocket(uint8_t sock);

uint8_t W5500_GetSocketStatus(uint8_t sock);

void W5500_Set_Last_Data(char *data, uint16_t len);
void W5500_HTTP_Server_Task(void);

/* ===========================================================
   DATA
   =========================================================== */

int W5500_Send(uint8_t sock,
               uint8_t *buf,
               uint16_t len);

int W5500_Recv(uint8_t sock,
               uint8_t *buf,
               uint16_t maxlen);

               void W5500_Server_Task(uint8_t sock,uint16_t port);
/* ===========================================================
   CHIP CONTROL
   =========================================================== */

#define W5500_CS_LOW()   (GPIOB->BSRR = (1 << (12 + 16)))
#define W5500_CS_HIGH()  (GPIOB->BSRR = (1 << 12))

#define W5500_RST_LOW()  (GPIOB->BSRR = (1 << (3 + 16)))
#define W5500_RST_HIGH() (GPIOB->BSRR = (1 << 3))

#endif