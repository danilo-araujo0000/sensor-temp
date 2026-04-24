# Tabela de GPIOs - Tela Circular

Baseado no firmware da tela circular em `main.h`.

| Pino no modulo GC9A01 | Funcao real | GPIO |
|---|---:|---:|
| SCL | SCLK / clock SPI | 12 |
| SDA | MOSI / dados SPI | 11 |
| DC | Data/Command | 10 |
| CS | Chip Select | 9 |
| RST | Reset | 14 |
| BL / LED | Backlight | 3.3V ou GPIO, se controlado |
| Sensor principal de temperatura | OneWire | 4 |
| DHT11 | dados | 17 |
| Encoder rotativo | CLK | 5 |
| Encoder rotativo | DT | 6 |
| Botao do encoder | SW | 7 |
| LED integrado | status | depende da placa |
| RGB integrado | status | depende da placa |

## Resumo rapido

- TFT GC9A01: GPIOs 9, 10, 11, 12 e 14
- Sensor principal: GPIO 4
- DHT11: GPIO 17
- Encoder: GPIOs 5, 6 e 7
