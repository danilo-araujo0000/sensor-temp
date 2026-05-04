# Mapa de GPIOs do Hub

## Sensores de movimento

| ID do sensor | GPIO | Ativo | Dashboard |
|---|---:|---:|---:|
| 35 | GPIO 35 | Sim | Sim |
| 36 | GPIO 36 | Sim | Sim |
| 40 | GPIO 40 | Sim | Sim |
| 42 | GPIO 42 | Sim | Sim |
| 47 | GPIO 47 | Sim | Sim |
| 48 | GPIO 48 | Sim | Sim |

## Gatilhos locais independentes

Entradas em GND que disparam regras locais no proprio ESP, sem backend no momento do evento:

- GPIO 4
- GPIO 5
- GPIO 6
- GPIO 7

## Saidas locais dos gatilhos independentes

- GPIO 10
- GPIO 11
- GPIO 12
- GPIO 13

## Saidas locais da automacao dos sensores

Usadas pela automacao por sensor (`movimento` / `sem movimento por X tempo`):

- GPIO 15
- GPIO 16
- GPIO 17
- GPIO 18
