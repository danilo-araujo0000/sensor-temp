# API Externa

Esta API foi criada para integracoes externas sem alterar o login normal do painel.

Base URL:

- Ambiente com `docker-compose`: `http://SEU_HOST:8081`
- As rotas da API ficam sob `/api`

## Autenticacao

Crie uma chave no painel:

- Menu `API Externa`
- Clique em `Criar chave`
- Guarde a chave completa quando ela aparecer

Envie a chave no header:

```http
x-api-key: SUA_CHAVE
```

Tambem funciona com:

```http
Authorization: Bearer SUA_CHAVE
```

## Rotas

### POST `/api/external/send-message`

Envia uma mensagem de texto para um chat do WhatsApp.

Body JSON:

```json
{
  "chatId": "120363403568204860@g.us",
  "text": "Mensagem enviada por sistema externo"
}
```

Campos:

- `chatId`: obrigatorio
- `text`: obrigatorio

Formatos de `chatId`:

- Grupo: `120363403568204860@g.us`
- Contato: `5511999999999@c.us`

## Exemplo com curl

```bash
curl -X POST "http://SEU_HOST:8081/api/external/send-message" \
  -H "Content-Type: application/json" \
  -H "x-api-key: SUA_CHAVE" \
  -d '{
    "chatId": "120363403568204860@g.us",
    "text": "Mensagem de teste"
  }'
```

## Exemplo com JavaScript

```js
const response = await fetch('http://SEU_HOST:8081/api/external/send-message', {
  method: 'POST',
  headers: {
    'Content-Type': 'application/json',
    'x-api-key': 'SUA_CHAVE'
  },
  body: JSON.stringify({
    chatId: '120363403568204860@g.us',
    text: 'Mensagem enviada por integracao'
  })
});

const data = await response.json();
console.log(data);
```

## Respostas

Sucesso:

```json
{
  "success": true,
  "data": {
    "messageId": "true_5511999999999@c.us_ABC123",
    "chatId": "120363403568204860@g.us",
    "text": "Mensagem de teste"
  }
}
```

Erros comuns:

- `401`: chave ausente, invalida ou revogada
- `409`: WhatsApp nao conectado
- `400`: body invalido
- `500`: falha interna ao enviar

## Observacoes

- O painel continua usando login por sessao normalmente.
- As chaves da API externa podem ser revogadas a qualquer momento.
- Depois de revogada, a chave para de funcionar imediatamente.
