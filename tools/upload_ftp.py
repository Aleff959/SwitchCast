import ftplib
import os
import sys
import time

SWITCH_IP = "192.168.1.68"
PORTS = [5000, 21]
LAUNCHER_PATH = "launcher/SwitchCast.nro"
CORE_PATH = "SwitchCastCore.nro"

if not os.path.exists(LAUNCHER_PATH) and not os.path.exists(CORE_PATH):
    print("Erro: Nenhum executavel (.nro) encontrado para envio!")
    sys.exit(1)

print(f"Tentando conectar ao Switch ({SWITCH_IP}) via FTP...")

ftp = None
deadline = time.time() + 120
print(f"Aguardando conexao FTP com o Switch ({SWITCH_IP}:5000 ou :21)...")

while time.time() < deadline and not ftp:
    for port in PORTS:
        try:
            f = ftplib.FTP()
            f.connect(SWITCH_IP, port, timeout=5)
            f.login()
            if f.sock:
                f.sock.settimeout(60)
            ftp = f
            print(f"\n[OK] Conectado via FTP na porta {port} (timeout de 60s ativado)!")
            break
        except Exception:
            pass
    if not ftp:
        print(".", end="", flush=True)
        time.sleep(1)

if not ftp:
    print("\nTimeout: Nenhuma conexao FTP ativa. Certifique-se de abrir o ftpd no Switch.")
    sys.exit(2)

try:
    for folder in ["/switch", "/switch/SwitchCast", "/switch/SwitchCast/modules"]:
        try:
            ftp.mkd(folder)
        except Exception:
            pass

    files_to_send = [
        ("SwitchCast.nro", "/switch/SwitchCast/SwitchCast.nro", "App Pai (Launcher)"),
        ("SwitchCastCore.nro", "/switch/SwitchCast/modules/SwitchCastCore.nro", "Modulo Principal (SwitchCast)")
    ]

    for local_path, remote_path, desc in files_to_send:
        if not os.path.exists(local_path):
            print(f"Aviso: {local_path} nao encontrado, pulando...")
            continue

        sz = os.path.getsize(local_path)
        print(f"\nEnviando {desc}: {local_path} ({sz / (1024*1024):.2f} MB) -> {remote_path}...")

        progress = {"uploaded": 0}
        start_time = time.time()

        def make_cb(total_bytes):
            def cb(chunk):
                progress["uploaded"] += len(chunk)
                elapsed = time.time() - start_time
                speed = (progress["uploaded"] / (1024 * 1024)) / (elapsed if elapsed > 0 else 0.001)
                pct = (progress["uploaded"] / total_bytes) * 100
                print(f"\rProgresso: {pct:.1f}% ({progress['uploaded'] / (1024*1024):.2f}/{total_bytes / (1024*1024):.2f} MB) - {speed:.2f} MB/s", end="", flush=True)
            return cb

        with open(local_path, "rb") as fp:
            ftp.storbinary(f"STOR {remote_path}", fp, blocksize=65536, callback=make_cb(sz))

        print(f"\n[OK] {desc} gravado com sucesso!")

    print("\n[SUCESSO COMPLETO] App Pai e Modulo Core sincronizados no Nintendo Switch!")
    ftp.quit()
except Exception as e:
    print(f"\nErro durante a transferencia: {e}")
    sys.exit(3)
