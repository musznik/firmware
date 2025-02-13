import os
from ftplib import FTP

# Konfiguracja serwera FTP
FTP_HOST = "loranet.pl"
FTP_USER = "musznik"
FTP_PASS = "1986mucha12"

# Ścieżka do katalogu build
BUILD_DIR = ".pio/build"

# Struktura plików i docelowych katalogów na serwerze FTP
UPLOAD_STRUCTURE = {
    "tbeam": "/var/www/meshmap.net/website/firmwarebin/tbeam",
    "t-echo": "/var/www/meshmap.net/website/firmwarebin/techo",
    "tlora-v2-1-1_6": "/var/www/meshmap.net/website/firmwarebin/TLORA_V2_1_1P6",
    "rak4631": "/var/www/meshmap.net/website/firmwarebin/rak4631",
    "heltec-wireless-tracker": "/var/www/meshmap.net/website/firmwarebin/heltec-wireless-tracker",
    "heltec-wireless-paper": "/var/www/meshmap.net/website/firmwarebin/heltecpapper",
    "heltec-v3": "/var/www/meshmap.net/website/firmwarebin/heltecv3",
    "tbeam-s3-core": "/var/www/meshmap.net/website/firmwarebin/tbeam-s3-core",
    "heltec-mesh-node-t114": "/var/www/meshmap.net/website/firmwarebin/heltec-mesh-node-t114",
    "heltec-wsl-v3": "/var/www/meshmap.net/website/firmwarebin/heltec-wsl-v3",
    "rak_killer_v2": "/var/www/meshmap.net/website/firmwarebin/rak_killer_v2"
}

def upload_file(ftp, local_file_path, remote_file_path):
    """Wysyła plik na serwer FTP."""
    with open(local_file_path, 'rb') as file:
        ftp.storbinary(f"STOR {remote_file_path}", file)
        print(f"Uploaded: {local_file_path} -> {remote_file_path}")

def main():
    # Połączenie z serwerem FTP
    ftp = FTP(FTP_HOST)
    ftp.login(FTP_USER, FTP_PASS)
    print(f"Connected to FTP server: {FTP_HOST}")

    # Iteracja po strukturze plików
    for environment, remote_dir in UPLOAD_STRUCTURE.items():
        local_dir = os.path.join(BUILD_DIR, environment)
        if not os.path.exists(local_dir):
            print(f"Directory does not exist: {local_dir}, skipping...")
            continue

        # Tworzenie katalogu na serwerze, jeśli nie istnieje
        try:
            ftp.cwd(remote_dir)
        except Exception:
            ftp.mkd(remote_dir)
            ftp.cwd(remote_dir)

        # Iteracja po plikach w katalogu lokalnym
        for root, _, files in os.walk(local_dir):
            for file in files:
                if file.endswith((".bin", ".zip", ".uf2")):
                    local_file_path = os.path.join(root, file)
                    remote_file_path = os.path.join(remote_dir, file).replace('\\', '/')
                    upload_file(ftp, local_file_path, remote_file_path)

    # Zamknięcie połączenia FTP
    ftp.quit()
    print("All files uploaded successfully.")

if __name__ == "__main__":
    main()

