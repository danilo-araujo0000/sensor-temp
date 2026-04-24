#pragma once

SensorConfig sensores[] = {
  // pinSecundario e opcional. Quando os dois pinos disparam juntos, conta como um unico evento.
  // LigacaoGpioGnd: sensor entre GPIO e GND, usando INPUT_PULLUP.
  // Campos: id, pinPrincipal, pinSecundario, ligacao, enabled, showOnDashboard, lastState.
  { "4", 4, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "5", 5, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "15", 15, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "17", 17, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "18", 18, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "35", 35, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "37", 37, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "40", 40, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "42", 42, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "10", 10, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false, false, false },
  { "11", 11, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false, false, false },
  { "12", 12, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false, false, false },
  { "13", 13, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, false, false, false }
};
