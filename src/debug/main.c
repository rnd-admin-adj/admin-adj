/* =====================================================================
   RAILWAY GPS + ETHERNET FIRMWARE (FreeRTOS version)
   -----------------------------------------------------------------
   GPS:
      USART1 -> NMEA data at 5Hz (configured at startup)
      RTC    -> synced from GPS time

   Network:
      W5500 Ethernet - serves live GPS data over HTTP

   Watchdog:
      IWDG, ~2.5 second timeout, fed once per task loop iteration

   All GPS + network work now runs inside a single FreeRTOS task.
   ===================================================================== */

#include "stm32f4xx.h"
#include "gps.h"
#include "usart_debug.h"
#include "spi2.h"
#include "w5500.h"

#include "FreeRTOS.h"
#include "task.h"

/* ================================================================
   CONFIGURATION
   ================================================================ */
#define FS_HZ        200   /* Sampling frequency (Hz) */
#define WINDOW_MS    100

/* ================================================================
   TASK CONFIGURATION
   ================================================================ */
#define GPS_TASK_STACK_SIZE   1024   /* words, not bytes */
#define GPS_TASK_PRIORITY     2

/* ================================================================
   SysTick Handler -> forwards to the FreeRTOS tick handler
   -----------------------------------------------------------------
   Written explicitly instead of relying on the FreeRTOSConfig.h
   macro rename, since that rename does not reliably take effect
   for this handler and leaves the weak default (infinite loop) in
   the vector table, silently hanging the system at scheduler start.
   ================================================================ */
extern void xPortSysTickHandler(void);

void SysTick_Handler(void)
{
    xPortSysTickHandler();
}

/* ================================================================
   WATCHDOG (IWDG)
   Independent hardware timer - runs regardless of FreeRTOS state,
   so it still protects against a task or scheduler lockup.
   ================================================================ */
static void watchdog_init(void)
{
    IWDG->KR  = 0x5555;   /* write access enable */
    IWDG->PR  = 4;        /* prescaler /64 -> ~1.6kHz LSI clock */
    IWDG->RLR = 4000;     /* ~2.5 second timeout */
    IWDG->KR  = 0xAAAA;   /* reload */
    IWDG->KR  = 0xCCCC;   /* start watchdog */
}

static void watchdog_feed(void)
{
    IWDG->KR = 0xAAAA;
}

/* ================================================================
   GPS + NETWORK TASK
   Handles: GPS UART init/config, RTC sync, W5500 init, and the
   continuous GPS polling + HTTP server + watchdog feed loop.
   ================================================================ */
static void vGpsTask(void *pvParameters)
{
    (void)pvParameters;

    /* ---- Debug UART ---- */
    USART2_Init();
    usart_debug("BOOT OK\r\n");

    /* ---- GPS UART ---- */
    gps_usart1_init();
    usart_debug("GPS UART READY\r\n");

    vTaskDelay(pdMS_TO_TICKS(1000));

    gps_configure();
    usart_debug("GPS CONFIGURED FOR 5Hz\r\n");

    gps_rtc_init();

    /* ---- Ethernet (W5500) ---- */
    SPI2_Init();
    int w5500_status = W5500_Init();

    if (w5500_status == 0)
        usart_debug("W5500 INIT OK\r\n");
    else if (w5500_status == -1)
        usart_debug("W5500 NOT DETECTED\r\n");
    else
        usart_debug("W5500 LINK DOWN - check cable\r\n");

    usart_debug("\r\n");

    /* ---- Watchdog ---- */
    watchdog_init();

    /* ============================================================
       TASK LOOP - continuous GPS polling + HTTP server + watchdog
       ============================================================ */
    for (;;)
    {
        gps_poll();
        W5500_HTTP_Server_Task();
        watchdog_feed();

        /* Yield briefly so the scheduler/idle task gets CPU time
           too, instead of this task spinning forever with no
           delay at all. 1 ms keeps GPS polling effectively
           continuous while still being cooperative. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ================================================================
   MAIN
   ================================================================ */
int main(void)
{
    /* CRITICAL: FreeRTOS on Cortex-M requires all priority bits
       assigned to preemption priority. Missing this causes silent
       hangs or hard faults when the scheduler starts. */
    NVIC_SetPriorityGrouping(0);

    /* ---- Create the GPS task ---- */
    BaseType_t result = xTaskCreate(vGpsTask,
                "GPS",
                GPS_TASK_STACK_SIZE,
                NULL,
                GPS_TASK_PRIORITY,
                NULL);

    if (result != pdPASS)
    {
        for (;;);   /* task creation failed - halt */
    }

    /* ---- Start the FreeRTOS scheduler (never returns) ---- */
    vTaskStartScheduler();

    /* Should never reach here */
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
   Fault handlers - print instead of silently hanging/resetting
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