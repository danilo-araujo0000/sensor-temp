#pragma once

// Copie este arquivo para config.local.h e preencha com os dados reais.
// config.local.h fica fora do Git.
constexpr char WIFI_SSID[] = "NOME_DA_REDE";
constexpr char WIFI_PASSWORD[] = "SENHA_DA_REDE";
constexpr char DEVICE_ID[] = "hub-01";

// URL base publica da API. Exemplo: https://api.seudominio.com
constexpr char url_default[] = "https://api.seudominio.com";

// Token opcional compartilhado entre o hub e o backend.
// Se ficar vazio, o backend aceita os endpoints de device sem autenticacao.
#ifndef DEVICE_API_TOKEN
#define DEVICE_API_TOKEN ""
#endif

// Para HTTPS atras de nginx:
// 1 = aceita qualquer certificado TLS do backend (mais simples para teste)
// 0 = exige BACKEND_CA_CERT preenchido com a CA/chain correta
#ifndef BACKEND_TLS_INSECURE
#define BACKEND_TLS_INSECURE 1
#endif

// Cadeia PEM opcional da CA usada pelo nginx. Use quando BACKEND_TLS_INSECURE = 0.
#ifndef BACKEND_CA_CERT
#define BACKEND_CA_CERT ""
#endif

// Poll do canal servidor -> ESP.
#ifndef DEVICE_POLL_INTERVAL_MS
#define DEVICE_POLL_INTERVAL_MS 1500
#endif

// Em firmware single-thread, deixe 0 para nao bloquear o loop principal por longos periodos.
#ifndef DEVICE_POLL_WAIT_SECONDS
#define DEVICE_POLL_WAIT_SECONDS 0
#endif

#ifndef DEVICE_HTTP_TIMEOUT_MS
#define DEVICE_HTTP_TIMEOUT_MS 3000
#endif
