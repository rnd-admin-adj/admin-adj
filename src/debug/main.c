#include "stm32f4xx.h"
#include "gps.h"
#include "usart_debug.h"
#include "spi2.h"
#include "w5500.h"


volatile uint32_t ms_ticks = 0;

void SysTick_Handler(void)
{
    ms_ticks++;
}

void systick_init(void)
{
    /* 1ms tick @ 16MHz HSI (adjust if using different SYSCLK) */
    SysTick_Config(SystemCoreClock / 1000);
}
static void delay_ms(uint32_t ms)
{
    uint32_t start = ms_ticks;
    while ((ms_ticks - start) < ms);
}
#define GPS_SOCK   0

int main(void)
{
    USART2_Init();
    usart_debug("BOOT OK\r\n");

    gps_usart1_init();
    usart_debug("GPS UART READY\r\n");

    systick_init();
    delay_ms(1000);

    gps_configure();
    usart_debug("GPS CONFIGURED FOR 5Hz\r\n");

    gps_rtc_init();

    /* W5500 init */
    SPI2_Init();   
    int w5500_status = W5500_Init();
    if (w5500_status == 0)
        usart_debug("W5500 INIT OK\r\n");
    else if (w5500_status == -1)
        usart_debug("W5500 NOT DETECTED\r\n");
    else
        usart_debug("W5500 LINK DOWN - check cable\r\n");

    usart_debug("\r\n");

    while (1)
    {
        gps_poll();   
        W5500_HTTP_Server_Task();
    }
}