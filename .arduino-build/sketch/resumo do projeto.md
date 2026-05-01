#line 1 "U:\\build\\sensor\\sensor-temp\\resumo do projeto.md"
🛡️ O Projeto: "Sentinel Core"
O objetivo é criar um dispositivo de mesa ou parede que sirva como a "ponte" entre o mundo físico (temperatura e IR) e o seu ecossistema digital (Zabbix, n8n, API).

1. Interface e Experiência do Usuário (UX)
Visual Premium: Uso de uma tela IPS Redonda para exibir informações de forma estética e moderna, fugindo do visual "quadradinho" comum.

Navegação Tátil: Controle total através de um Encoder Rotativo (girar para navegar em menus circulares, clicar para selecionar) e um botão físico de "Voltar".

Auto-Ajuste: Sensor de luz ambiente para que a tela não ilumine o quarto inteiro à noite (ajuste automático de brilho).

2. Monitoramento de Alta Confiabilidade
Tripla Redundância (Fallback): O sistema lê múltiplos sensores simultaneamente. Se o sensor principal falhar ou retornar um valor impossível, o software chaveia automaticamente para o secundário (ou para o sensor de ambiente integrado), garantindo que seu Zabbix nunca fique sem dados.

Dados Ambientais: Além da temperatura, o sistema monitora umidade e pressão atmosférica para uma análise climática completa.

3. Automação e Controle IR
Clonagem de Comandos: O dispositivo possui um "Modo Aprendizado" onde ele escuta sinais de controles remotos reais e os armazena na memória interna.

Atuador Remoto: Através da interface física ou de chamadas via API, o dispositivo dispara sinais infravermelhos para controlar dispositivos "burros" (Ar-condicionado, TV, Som) como se fosse o controle original.

4. Inteligência e Conectividade
Comunicação API: Envio periódico de dados em formato JSON para o seu servidor. O dispositivo é inteligente o suficiente para reportar não só a temperatura, mas também o status da bateria e qual sensor de fallback está ativo.

Persistência de Dados: Todas as configurações (Wi-Fi, senhas da API e códigos IR aprendidos) são gravadas na memória Flash do microcontrolador, sendo imunes a reinicializações ou falta de energia.

5. Resiliência Energética (Modo UPS)
Operação Ininterrupta: O sistema vive na tomada, mas possui uma bateria de backup interna.

Detecção de Queda: Ao perder a energia da rede, o sensor entra em modo de economia, reduz o brilho da tela e envia um alerta imediato para o seu servidor avisando que está operando em modo de emergência.

O que torna esse projeto especial?
A maioria dos sensores de prateleira falha em dois pontos: ou eles são feios, ou eles "travam" e param de enviar dados. O seu projeto resolve isso com a Interface Redonda (estética) e o Algoritmo de Fallback (confiabilidade).

Agora que temos o resumo consolidado, você quer que eu comece a escrever a estrutura lógica do código (em C++) para gerenciar esse sistema de menus e sensores?