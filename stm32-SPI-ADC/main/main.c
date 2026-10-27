/**
 ******************************************************************************
 * main.c  -  Liaison SPI STM32 <-> ADC + affichage UART (PuTTY)
 * Cible   : Nucleo-F446RE (HAL)
 *
 * Câblage SPI1 :
 *   PA5  -> SCLK
 *   PA6  -> MISO (DOUT de l'ADC)
 *   PA7  -> MOSI (DIN de l'ADC)
 *   PB6  -> CS   (GPIO, actif à l'état bas)
 *
 * UART2 (via USB ST-Link) : PA2 TX / PA3 RX, 115200 8N1
 *   -> PuTTY : Serial, port COMx, 115200
 *
 * Fonctionnement :
 *   1. Envoi de 6 trames de configuration (2 octets : adresse + data)
 *   2. Toutes les 500 ms : envoi d'une trame de 3 octets et lecture
 *      des 3 octets reçus en retour, affichés sur PuTTY
 ******************************************************************************
 */

#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>

/* ========================= PARAMÈTRES À ADAPTER ========================== */

#define NB_TRAMES_CONFIG   6
#define TAILLE_TRAME_CFG   2
#define TAILLE_TRAME_LECT  3
#define PERIODE_LECTURE_MS 500

/* 6 trames de configuration : { adresse, data }
 * -> Remplacer par les valeurs de la datasheet de ton ADC */
static const uint8_t trames_config[NB_TRAMES_CONFIG][TAILLE_TRAME_CFG] = {
    { 0x01, 0x00 },
    { 0x02, 0x00 },
    { 0x03, 0x00 },
    { 0x04, 0x00 },
    { 0x05, 0x00 },
    { 0x06, 0x00 },
};

/* Trame d'interrogation de l'ADC (3 octets)
 * -> Remplacer par la commande de lecture de ton ADC */
static const uint8_t trame_lecture[TAILLE_TRAME_LECT] = { 0x00, 0x00, 0x00 };

/* Broche Chip Select */
#define CS_PORT  GPIOB
#define CS_PIN   GPIO_PIN_6
#define CS_BAS() HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_RESET)
#define CS_HAUT() HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET)

/* ============================== HANDLES ================================== */

SPI_HandleTypeDef  hspi1;
UART_HandleTypeDef huart2;

/* ============================ PROTOTYPES ================================= */

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART2_UART_Init(void);
static void uart_print(const char *msg);
static void config_adc(void);
static void lecture_adc(void);
void Error_Handler(void);

/* ================================ MAIN =================================== */

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_SPI1_Init();
    MX_USART2_UART_Init();

    uart_print("\r\n=== Demarrage liaison SPI STM32 <-> ADC ===\r\n");

    HAL_Delay(10);          /* laisse le temps à l'ADC de démarrer */
    config_adc();

    uart_print("--- Lecture ADC toutes les 500 ms ---\r\n");

    uint32_t t_derniere_lecture = HAL_GetTick();

    while (1)
    {
        if (HAL_GetTick() - t_derniere_lecture >= PERIODE_LECTURE_MS)
        {
            t_derniere_lecture += PERIODE_LECTURE_MS;
            lecture_adc();
        }
    }
}

/* ======================== FONCTIONS APPLICATIVES ========================= */

/* Envoi d'une chaîne sur l'UART2 (PuTTY) */
static void uart_print(const char *msg)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
}

/* Envoi des 6 trames de configuration (2 octets chacune) */
static void config_adc(void)
{
    char buf[64];

    uart_print("--- Configuration de l'ADC ---\r\n");

    for (int i = 0; i < NB_TRAMES_CONFIG; i++)
    {
        CS_BAS();
        HAL_StatusTypeDef st = HAL_SPI_Transmit(&hspi1,
                                                (uint8_t *)trames_config[i],
                                                TAILLE_TRAME_CFG, 100);
        CS_HAUT();

        if (st == HAL_OK)
            snprintf(buf, sizeof(buf), "Trame %d : Adr 0x%02X  Data 0x%02X  OK\r\n",
                     i + 1, trames_config[i][0], trames_config[i][1]);
        else
            snprintf(buf, sizeof(buf), "Trame %d : ERREUR SPI (%d)\r\n", i + 1, st);

        uart_print(buf);
        HAL_Delay(1);       /* petit délai entre deux trames */
    }
}

/* Interrogation de l'ADC : envoi de 3 octets, réception simultanée de 3 octets */
static void lecture_adc(void)
{
    uint8_t rx[TAILLE_TRAME_LECT] = { 0 };
    char buf[96];

    CS_BAS();
    HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi1,
                                                   (uint8_t *)trame_lecture, rx,
                                                   TAILLE_TRAME_LECT, 100);
    CS_HAUT();

    if (st != HAL_OK)
    {
        snprintf(buf, sizeof(buf), "Lecture : ERREUR SPI (%d)\r\n", st);
        uart_print(buf);
        return;
    }

    /* Valeur brute sur 24 bits (à adapter selon le format de ton ADC,
     * ex. si le 1er octet est un statut : valeur = (rx[1] << 8) | rx[2]) */
    uint32_t valeur = ((uint32_t)rx[0] << 16) | ((uint32_t)rx[1] << 8) | rx[2];

    snprintf(buf, sizeof(buf),
             "[%7lu ms] TX: %02X %02X %02X  |  RX: %02X %02X %02X  |  Valeur: %lu\r\n",
             HAL_GetTick(),
             trame_lecture[0], trame_lecture[1], trame_lecture[2],
             rx[0], rx[1], rx[2], valeur);
    uart_print(buf);
}

/* ======================== INITIALISATIONS ================================ */

/* Horloge : HSI 16 MHz -> PLL -> SYSCLK 84 MHz (APB1 42 MHz, APB2 84 MHz) */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
    RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM            = 16;
    RCC_OscInitStruct.PLL.PLLN            = 336;
    RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV4;
    RCC_OscInitStruct.PLL.PLLQ            = 2;
    RCC_OscInitStruct.PLL.PLLR            = 2;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) Error_Handler();
}

/* SPI1 maître, 8 bits, MSB first, mode 0 (CPOL=0, CPHA=1Edge)
 * 84 MHz / 64 = ~1,3 MHz -> à adapter selon la vitesse max de l'ADC */
static void MX_SPI1_Init(void)
{
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;   /* CPOL = 0 */
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;    /* CPHA = 0 */
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 10;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) Error_Handler();
}

/* USART2 : 115200 8N1 (port COM virtuel du ST-Link) */
static void MX_USART2_UART_Init(void)
{
    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = 115200;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

/* GPIO : CS en sortie, à l'état haut au repos */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = { 0 };

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET);

    GPIO_InitStruct.Pin   = CS_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(CS_PORT, &GPIO_InitStruct);
}

/* ============== MSP : configuration bas niveau des broches =============== */
/* (si projet CubeMX, ces fonctions sont déjà dans stm32f4xx_hal_msp.c :
 *  dans ce cas, supprimer celles-ci) */

void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
    GPIO_InitTypeDef GPIO_InitStruct = { 0 };
    if (hspi->Instance == SPI1)
    {
        __HAL_RCC_SPI1_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        /* PA5 SCK, PA6 MISO, PA7 MOSI */
        GPIO_InitStruct.Pin       = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    }
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef GPIO_InitStruct = { 0 };
    if (huart->Instance == USART2)
    {
        __HAL_RCC_USART2_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        /* PA2 TX, PA3 RX */
        GPIO_InitStruct.Pin       = GPIO_PIN_2 | GPIO_PIN_3;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    }
}

/* SysTick nécessaire à HAL_Delay / HAL_GetTick
 * (déjà présent dans stm32f4xx_it.c sur un projet CubeMX -> supprimer ici) */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) { }
}
