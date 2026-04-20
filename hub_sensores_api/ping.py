import urllib.request
import json
import time
import os

print("Enviando sinal de movimento fake...")
data = json.dumps({"sensor_id": "esp32s3-hub-01:mov_entrada"}).encode()
req = urllib.request.Request('http://localhost:5080/movements', data=data, headers={'Content-Type': 'application/json'})

try:
    print(urllib.request.urlopen(req).read().decode())
    print("\n[+] Sinal recebido pelo Server. O server agora chamará o FFMPEG que travará os próximos 21s no background convertendo.")
    print("[+] Aguardando aqui tambem por 25 segundos so pra checar a pasta no fim...")
    time.sleep(25)
    print("\n[+] Arquivos:")
    for f in sorted(os.listdir("snapshots")):
        stat = os.stat(os.path.join("snapshots", f))
        print(f" -> {f} ({stat.st_size} bytes)")
except Exception as e:
    print("ERRO EXECUTANDO PING:", e)
