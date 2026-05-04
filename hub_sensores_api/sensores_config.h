#pragma once

SensorConfig sensores[] = {
  // pinSecundario e opcional. Quando os dois pinos disparam juntos, conta como um unico evento.
  // LigacaoGpioGnd: sensor entre GPIO e GND, usando INPUT_PULLUP.
  // Campos: id, pinPrincipal, pinSecundario, ligacao, enabled, showOnDashboard, lastState.
  { "35", 35, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "36", 36, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "40", 40, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "42", 42, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "47", 47, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
  { "48", 48, PINO_SEM_SECUNDARIO, LigacaoGpioGnd, true, true, false },
};
