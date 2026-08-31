/* =====================================================================
   RAILWAY AXLE BOX ODOMETER FIRMWARE (FreeRTOS version)
   -----------------------------------------------------------------
   Encoder:
      Channel A          -> PA0, rising-edge pulse counting
      Pulses per revolution -> 512 (Kubler 8.5020.C811.0512)

   Wheel:
      Diameter    -> 915 mm (standard new BG coach/wagon wheel)
      Mounting    -> encoder coupled directly to the axle (1:1)

   Network:
      This device (STM32 + encoder) IP -> 192.168.1.211
      Destination server IP            -> 192.168.1.104
      TCP port                         -> 5000

   Data sent every 100 ms as:
      ENCODER,COUNT=...,KM=...,METER=...,MM=...,SPEED_MS=...,SPEED_KMH=...

   All odometer work (encoder read, distance/speed calculation, serial
   debug print, TCP send) now runs inside a single FreeRTOS task.
   ===================================================================== */

#include "stm32f4xx.h"
#include "usart_debug.h"
#include "spi2.h"
#include "w5500.h"
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

/* ================================================================
   ENCODER / WHEEL CALIBRATION
   ================================================================ */
#define PULSES_PER_REV      512.0f
#define WHEEL_DIAMETER_MM   915.0f
#define GEAR_RATIO          1.0f      /* Direct 1:1 mount on axle */

/* ================================================================
   TCP SOCKET USED FOR SENDING ENCODER DATA
   ================================================================ */
#define SENSOR_SOCKET       0

/* ================================================================
   TASK CONFIGURATION
   ================================================================ */
#define ODOMETER_TASK_STACK_SIZE   1024   /* words, not bytes */
#define ODOMETER_TASK_PRIORITY     2

/* ================================================================
   GLOBAL STATE (shared with the encoder interrupt)
   ================================================================ */
volatile uint32_t counter = 0;      /* raw encoder pulse count */

/* ================================================================
   ENCODER GPIO + EXTI SETUP
   PA0 = Encoder Channel A, rising-edge interrupt
   ================================================================ */
static void encoder_gpio_exti_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;   /* Enable GPIOA clock */
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;  /* Enable SYSCFG clock */

    GPIOA->MODER &= ~(3U << (0 * 2));      /* PA0 = input mode */

    GPIOA->PUPDR &= ~(3U << (0 * 2));      /* PA0 pull-up enabled */
    GPIOA->PUPDR |=  (1U << (0 * 2));

    SYSCFG->EXTICR[0] &= ~SYSCFG_EXTICR1_EXTI0;  /* Route EXTI0 to PA0 */

    EXTI->IMR  |= EXTI_IMR_MR0;            /* Unmask EXTI0 */
    EXTI->RTSR |= EXTI_RTSR_TR0;           /* Trigger on rising edge */
    EXTI->FTSR &= ~EXTI_FTSR_TR0;          /* Disable falling edge */

    /* Priority must be numerically >= configMAX_SYSCALL_INTERRUPT_PRIORITY
       (i.e. lower urgency) only if this ISR calls FreeRTOS API functions.
       It doesn't (just increments a counter), so this is safe as-is. */
    NVIC_SetPriority(EXTI0_IRQn, 5);
    NVIC_EnableIRQ(EXTI0_IRQn);
}

/* ================================================================
   ENCODER PULSE INTERRUPT (one call per rising edge on channel A)
   ================================================================ */
void EXTI0_IRQHandler(void)
{
    if (EXTI->PR & EXTI_PR_PR0)
    {
        EXTI->PR = EXTI_PR_PR0;   /* clear pending flag (write 1 to clear) */
        counter++;
    }
}

extern void xPortSysTickHandler(void);

void SysTick_Handler(void)
{
    xPortSysTickHandler();
}
/* ================================================================
   ODOMETER TASK
   Handles: encoder read, distance/speed calculation,
            serial debug print, TCP send to server.
   Runs once every 100 ms.
   ================================================================ */
static void vOdometerTask(void *pvParameters)
{
    (void)pvParameters;

    int      w5500_status;
    uint32_t lastCounter = 0;
    float    circumference;

    /* ---- Debug UART ---- */
    USART2_Init();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* ---- SPI2 (used by the W5500) ---- */
    SPI2_Init();
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Wheel circumference = pi * diameter, scaled by mount ratio.
       GEAR_RATIO = 1.0 means one encoder revolution = one wheel
       revolution (direct axle mount). */
    circumference = (3.14159265f * WHEEL_DIAMETER_MM) * GEAR_RATIO;

    /* ---- Ethernet (W5500) ---- */
    usart_debug("\r\nInitializing W5500...\r\n");

    w5500_status = W5500_Init();

    if (w5500_status == 0)
    {
        usart_debug("W5500 initialized OK\r\n");
        usart_debug("Device IP : 192.168.1.211\r\n");
        usart_debug("Server IP : 192.168.1.104\r\n");
        usart_debug("Server Port : 5000\r\n");
    }
    else
    {
        usart_debug("W5500 INIT FAILED\r\n");

        if (w5500_status == -1)
            usart_debug("W5500 CHIP NOT RESPONDING\r\n");

        if (w5500_status == -2)
            usart_debug("ETHERNET LINK DOWN\r\n");
    }

    /* ---- Startup banner ---- */
    usart_debug("\r\n\r\n========================================\r\n");
    usart_debug("  RAILWAY AXLE BOX ODOMETER (FreeRTOS)\r\n");
    usart_debug("========================================\r\n");
    usart_debug("Wheel Diameter : 915 mm\r\n");
    usart_debug("Pulses / Rev   : 512\r\n");
    usart_debug("Gear Ratio     : 1:1\r\n");
    usart_debug("Device IP      : 192.168.1.211\r\n");
    usart_debug("Server IP      : 192.168.1.104\r\n");
    usart_debug("Server Port    : 5000\r\n");
    usart_debug("========================================\r\n");
    usart_debug("Count\tKM\tMeter\tMM\tSpeed(m/s)\tSpeed(km/h)\r\n");

    /* ============================================================
       TASK LOOP - runs once every 100 ms
       ============================================================ */
    for (;;)
    {
        /* Read the pulse counter safely (interrupt can fire anytime) */
        __disable_irq();
        uint32_t currentCounter = counter;
        __enable_irq();

        uint32_t pulseDifference = currentCounter - lastCounter;
        float    time_sec        = 0.25f;   /* fixed 250 ms period (vTaskDelay below) */

        /* ---- Total distance since startup ---- */
        float distance_mm = ((float)currentCounter / PULSES_PER_REV) * circumference;

        /* Break total distance into KM / Meter / Millimeter */
        uint32_t total_mm_int = (uint32_t)distance_mm;
        uint32_t dist_km   = total_mm_int / 1000000UL;   /* 1 km = 1,000,000 mm */
        uint32_t remainder = total_mm_int % 1000000UL;
        uint32_t dist_m    = remainder / 1000UL;         /* 1 m  = 1,000 mm */
        uint32_t dist_mm   = remainder % 1000UL;

        /* ---- Speed for this 100 ms interval ---- */
        float distance_interval_mm = ((float)pulseDifference / PULSES_PER_REV) * circumference;
        float speed_mm_s = distance_interval_mm / time_sec;
        float speed_m_s  = speed_mm_s / 1000.0f;
        float speed_km_h = speed_m_s * 3.6f;

        /* ---- Build the data line to log and transmit ---- */
        char line[256];
        int len = snprintf(line, sizeof(line),
            "ENCODER,COUNT=%lu,KM=%lu,METER=%lu,MM=%lu,SPEED_MS=%.3f,SPEED_KMH=%.2f\r\n",
            (unsigned long)currentCounter,
            (unsigned long)dist_km,
            (unsigned long)dist_m,
            (unsigned long)dist_mm,
            speed_m_s,
            speed_km_h);

        usart_debug(line);

        /* ---- Send to the server over TCP (192.168.1.104:5000) ---- */
        if (w5500_status == 0)
        {
            int send_status = W5500_Sensor_Client_Task(SENSOR_SOCKET, line, (uint16_t)len);

            if (send_status == 0)
            {
                /* Cache the last successfully sent data (used by the
                   optional HTTP status page) */
                W5500_Set_Last_Data(line, (uint16_t)len);
            }
        }

        lastCounter = currentCounter;

        /* Block for 250 ms, letting other tasks (idle task, timer
           task, etc.) run in the meantime instead of busy-waiting. */
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ================================================================
   MAIN
   ================================================================ */
int main(void)
{

       NVIC_SetPriorityGrouping(0);
    /* ---- TEMPORARY DEBUG: confirm we reach main() at all ---- */
    USART2_Init();
    usart_debug("\r\n>>> BOOT: main() reached <<<\r\n");

    /* ---- Encoder input ---- */
    encoder_gpio_exti_init();

    usart_debug(">>> BOOT: creating task <<<\r\n");

    /* ---- Create the odometer task ---- */
    BaseType_t result = xTaskCreate(vOdometerTask,
                "Odometer",
                ODOMETER_TASK_STACK_SIZE,
                NULL,
                ODOMETER_TASK_PRIORITY,
                NULL);

    if (result != pdPASS)
    {
        usart_debug(">>> BOOT: xTaskCreate FAILED <<<\r\n");
        for (;;);
    }

    usart_debug(">>> BOOT: starting scheduler <<<\r\n");

    /* ---- Start the FreeRTOS scheduler (never returns) ---- */
    vTaskStartScheduler();

    /* Should never reach here */
    usart_debug(">>> BOOT: scheduler returned - FAILED <<<\r\n");
    for (;;);
}

/* ================================================================
   FreeRTOS Hook Functions
   ================================================================ */

/* Called if a task's stack overflows its allocated size. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;

    usart_debug("\r\n*** STACK OVERFLOW DETECTED ***\r\n");

    __disable_irq();
    for (;;);   /* halt - do not continue with a corrupted stack */
}

/* Called if pvPortMalloc() fails to allocate memory (heap exhausted). */
void vApplicationMallocFailedHook(void)
{
    usart_debug("\r\n*** MALLOC FAILED - HEAP EXHAUSTED ***\r\n");

    __disable_irq();
    for (;;);   /* halt - out of memory, cannot safely continue */
}

/* ================================================================
   TEMPORARY DEBUG: catch faults instead of silently hanging/resetting
   ================================================================ */
void HardFault_Handler(void)
{
    usart_debug("\r\n!!! HARD FAULT !!!\r\n");
    __disable_irq();
    for (;;);
}

void MemManage_Handler(void)
{
    usart_debug("\r\n!!! MEM MANAGE FAULT !!!\r\n");
    __disable_irq();
    for (;;);
}

void BusFault_Handler(void)
{
    usart_debug("\r\n!!! BUS FAULT !!!\r\n");
    __disable_irq();
    for (;;);
}

void UsageFault_Handler(void)
{
    usart_debug("\r\n!!! USAGE FAULT !!!\r\n");
    __disable_irq();
    for (;;);
}