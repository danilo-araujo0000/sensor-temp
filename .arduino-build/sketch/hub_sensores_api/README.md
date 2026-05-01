#line 1 "U:\\build\\sensor\\sensor-temp\\hub_sensores_api\\README.md"
# Hub de Sensores

Projeto separado do firmware principal.

## O que faz

- O ESP32 conecta no Wi-Fi com `SSID` e senha hardcoded.
- Monitora varios sensores de movimento.
- No boot, registra os sensores no backend.
- Depois envia apenas o `sensor_id` quando detecta movimento.
- O servidor Python resolve o resto e grava tudo em `SQLite`.
- A API aplica o cooldown de 20 segundos por `sensor_id`.

## Arquivos

- `hub_sensores_api.ino`: firmware do ESP32.
- `server.py`: API HTTP + SQLite.

## Endpoint

- `POST /sensors/register`
- `POST /movements`
- `GET /health`
- `GET /sensors?limit=50`
- `GET /movements?limit=50`

## Configuracao principal

No sketch, ajuste:

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `SENSOR_REGISTER_URL`
- `MOVEMENT_URL`
- lista `sensores[]`

## Executar o servidor

```powershell
python .\server.py
```

Servidor padrao na porta `5080`.

## Compilar o firmware

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3 --build-path '.\.arduino-build' .
```
