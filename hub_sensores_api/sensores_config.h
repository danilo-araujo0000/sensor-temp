#pragma once

SensorConfig sensores[] = {
  // pinSecundario e opcional. Quando os dois pinos disparam juntos, conta como um unico evento.
  // LigacaoGpioGnd: sensor entre GPIO e GND, usando INPUT_PULLUP.
  { "int_canino", 8, 9, LigacaoGpioGnd, false },
  { "int_felino", 17, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false },
  { "infecto", 18, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false },
  { "sensor_4", 15, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false },
  { "alerta_int_canino", 40, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false },
  { "alerta_int_felino", 42, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false },
  { "alerta_1", 35, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false },
  { "alerta_2", 37, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false }
};
