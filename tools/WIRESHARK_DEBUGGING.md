# Debug del Protocollo CWNet con Wireshark

Guida completa per catturare e analizzare il traffico CWNet tra client e server.

## Installazione del Dissector

### Linux
```bash
mkdir -p ~/.local/lib/wireshark/plugins
cp cwnet_dissector.lua ~/.local/lib/wireshark/plugins/
```

### Windows
```cmd
copy cwnet_dissector.lua %APPDATA%\Wireshark\plugins\
```

### Alternativa: Caricamento Manuale
```bash
wireshark -X lua_script:cwnet_dissector.lua
```

## Cattura del Traffico

### 1. Cattura in tempo reale con Wireshark GUI

```bash
# Avvia Wireshark con permessi di cattura
sudo wireshark

# Oppure senza GUI (salva in file):
sudo wireshark -i <interfaccia> -k -f "tcp port 7355" -w cwnet_capture.pcapng
```

**Filtro di cattura:**
```
tcp port 7355
```

### 2. Cattura con tcpdump (CLI)

```bash
# Cattura tutto il traffico sulla porta 7355
sudo tcpdump -i any -s 65535 -w cwnet_capture.pcap "tcp port 7355"

# Cattura solo traffico verso/da server specifico
sudo tcpdump -i any -s 65535 -w cwnet_capture.pcap \
    "tcp port 7355 and host <server_ip>"
```

### 3. Cattura con tshark (Wireshark CLI)

```bash
# Cattura e decodifica in tempo reale
sudo tshark -i any -f "tcp port 7355" -Y cwnet

# Salva in file per analisi successiva
sudo tshark -i any -f "tcp port 7355" -w cwnet_capture.pcapng
```

## Filtri di Visualizzazione Wireshark

Dopo aver caricato la cattura, usa questi filtri:

### Filtri Generali
```
cwnet                          # Tutti i frame CWNet
cwnet.command.type == 0x10     # Solo messaggi MORSE
cwnet.command.type == 0x03     # Solo PING
cwnet.command.type == 0x01     # Solo CONNECT
```

### Filtri Specifici per MORSE (il tuo problema!)
```
# Tutti i messaggi MORSE
cwnet.command.type == 0x10

# Eventi Key DOWN
cwnet.morse.key == 1

# Eventi Key UP
cwnet.morse.key == 0

# Eventi con timestamp > 100ms
cwnet.morse.timestamp_ms > 100

# Frame MORSE dal client (usa IP sorgente del tuo ESP32)
cwnet.command.type == 0x10 && ip.src == <esp32_ip>

# Frame MORSE dal server
cwnet.command.type == 0x10 && ip.src == <server_ip>
```

### Filtri per PING/Latency
```
# Tutti i PING
cwnet.command.type == 0x03

# PING requests (type 0)
cwnet.ping.type == 0

# PING responses
cwnet.ping.type == 1 || cwnet.ping.type == 2
```

### Filtri per Handshake
```
# Sequenza di connessione
cwnet.command.type == 0x01 || cwnet.command.type == 0x03

# CONNECT con permessi specifici
cwnet.connect.permissions != 0x00
```

## Analisi dei Problemi MORSE

### Cosa cercare nei frame MORSE:

1. **Ordine degli eventi**
   - Verifica che KEY DOWN e KEY UP si alternino correttamente
   - Controlla i timestamp tra eventi consecutivi

2. **End-Of-Transmission**
   - Cerca due byte consecutivi con key=UP (bit 7 = 0)
   - Il dissector li evidenzia automaticamente

3. **Timestamp anomali**
   - Timestamp troppo corti (<5ms potrebbero indicare problemi di timing)
   - Timestamp troppo lunghi (>1000ms potrebbero indicare perdita eventi)

4. **Confronto Client ↔ Server**
   - Filtra per `ip.src == <client_ip>` vs `ip.src == <server_ip>`
   - Verifica che i pattern siano simmetrici

### Esempio di analisi step-by-step:

```bash
# 1. Cattura traffico tra client ufficiale e server ufficiale
sudo tcpdump -i any -s 65535 -w cwnet_official.pcap "tcp port 7355 and host <official_server>"

# 2. Cattura traffico tra tuo client ESP32 e server ufficiale
sudo tcpdump -i any -s 65535 -w cwnet_esp32.pcap "tcp port 7355 and host <official_server>"

# 3. Apri entrambi i file in Wireshark e confronta:
wireshark cwnet_official.pcap &
wireshark cwnet_esp32.pcap &

# 4. Applica filtro MORSE in entrambi:
#    cwnet.command.type == 0x10

# 5. Esporta i pacchetti MORSE per confronto:
#    File → Export Packet Dissections → As Plain Text
```

## Colonne Personalizzate Wireshark

Aggiungi queste colonne per analisi veloce (Edit → Preferences → Columns):

| Nome | Tipo | Campo |
|------|------|-------|
| CWNet Cmd | Custom | `cwnet.command.type` |
| Morse Key | Custom | `cwnet.morse.key` |
| Morse Time | Custom | `cwnet.morse.timestamp_ms` |
| Ping Type | Custom | `cwnet.ping.type` |
| Latency | Custom | Calculated Field |

## Esportazione Dati per Analisi

### Esporta frame MORSE in CSV
```bash
tshark -r cwnet_capture.pcapng -Y "cwnet.command.type == 0x10" \
    -T fields -E separator=, -E quote=d \
    -e frame.number \
    -e frame.time_relative \
    -e ip.src \
    -e cwnet.morse.key \
    -e cwnet.morse.timestamp_ms \
    > morse_events.csv
```

### Esporta PING per analisi latenza
```bash
tshark -r cwnet_capture.pcapng -Y "cwnet.command.type == 0x03" \
    -T fields -E separator=, -E quote=d \
    -e frame.number \
    -e cwnet.ping.type \
    -e cwnet.ping.t0 \
    -e cwnet.ping.t1 \
    -e cwnet.ping.t2 \
    > ping_analysis.csv
```

## Troubleshooting

### Il dissector non appare
1. Verifica installazione:
   ```bash
   ls -la ~/.local/lib/wireshark/plugins/cwnet_dissector.lua
   ```

2. Controlla log Wireshark:
   ```
   Help → About Wireshark → Plugins
   ```
   Cerca "cwnet_dissector.lua" nella lista

3. Abilita debug Lua:
   ```bash
   wireshark -o "gui.console_open:ALWAYS" -X lua_script:cwnet_dissector.lua
   ```

### Il traffico non viene catturato
1. Verifica che la porta sia corretta (default: 7355)
2. Controlla firewall:
   ```bash
   sudo iptables -L -n | grep 7355
   sudo firewall-cmd --list-ports  # CentOS/RHEL
   sudo ufw status | grep 7355      # Ubuntu
   ```

3. Verifica connessione TCP:
   ```bash
   sudo netstat -tnp | grep 7355
   # oppure
   sudo ss -tnp | grep 7355
   ```

### Il dissector non decodifica i frame
1. Verifica che sia TCP (non UDP)
2. Usa "Decode As" per forzare il dissector:
   - Click destro sul pacchetto
   - Decode As...
   - Seleziona "CWNet"

## Script di Cattura Automatica

```bash
#!/bin/bash
# capture_cwnet.sh - Cattura automatica con rotazione file

INTERFACE="any"
SERVER_IP="YOUR_SERVER_IP"
OUTPUT_DIR="./captures"
DURATION=300  # 5 minuti per file

mkdir -p "$OUTPUT_DIR"

while true; do
    FILENAME="$OUTPUT_DIR/cwnet_$(date +%Y%m%d_%H%M%S).pcap"
    echo "Capturing to $FILENAME for ${DURATION}s..."

    timeout "$DURATION" sudo tcpdump \
        -i "$INTERFACE" \
        -s 65535 \
        -w "$FILENAME" \
        "tcp port 7355 and host $SERVER_IP"

    echo "Capture saved. Files in $OUTPUT_DIR:"
    ls -lh "$OUTPUT_DIR"
done
```

## Riferimenti

- **Protocollo CWNet**: `docs/RemoteCwNetProtocol.md`
- **Client Implementation**: `components/remote/remote_cw_client.cpp`
- **Wireshark Lua API**: https://www.wireshark.org/docs/wsdg_html_chunked/wsluarm.html
