# STM32 – Liaison SPI avec un ADC + affichage UART

Projet pour **Nucleo-F446RE** (HAL).

## Fonctionnement
1. Au démarrage, la STM32 envoie **6 trames de configuration** de 2 octets (adresse + data) à l'ADC.
2. Ensuite, elle interroge l'ADC **toutes les 500 ms** avec une trame de 3 octets.
3. Les octets envoyés et reçus s'affichent sur **PuTTY** par l'UART.

## Câblage

| Signal | Broche STM32 |
|--------|--------------|
| SCLK   | PA5          |
| MISO   | PA6          |
| MOSI   | PA7          |
| CS     | PB6          |
| UART TX / RX | PA2 / PA3 (USB ST-Link) |

## PuTTY
Choisir Serial, le port COM du ST-Link, 115200 bauds, 8N1.

## À adapter
- Les trames `trames_config[]` et `trame_lecture[]` dans `main.c`, selon la datasheet de l'ADC
- Le mode SPI (actuellement mode 0) et le prescaler (environ 1,3 MHz)
