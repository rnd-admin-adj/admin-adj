#include "delay.h"
#include "stm32f4xx.h"

void delay_init(void)
{
    SysTick_Config(SystemCoreClock / 1000);
}

void delay_ms(uint32_t ms)
{
    for(volatile uint32_t i = 0; i < ms * 4000; i++) {
        __asm__("nop");
    }
}

void delay_us(uint32_t us)
{
    for(volatile uint32_t i = 0; i < us * 4; i++) {
        __asm__("nop");
    }
}