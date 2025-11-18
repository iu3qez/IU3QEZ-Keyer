# CWNet Protocol Debugging Tools

Suite completa per il debug del protocollo CWNet, con focus sui problemi di trasmissione MORSE.

## Contenuto

### 1. **cwnet_dissector.lua** - Dissector Wireshark
Decodifica automatica del protocollo CWNet in Wireshark con supporto completo per:
- Tutti i tipi di frame (CONNECT, PING, MORSE, RIGCTLD, etc.)
- Decodifica timestamp non-lineari
- Visualizzazione eventi Key UP/DOWN
- Rilevamento End-Of-Transmission
- Calcolo latency automatico dai frame PING

### 2. **compare_morse_captures.py** - Comparatore Catture
Script Python per confronto automatico di catture .pcap:
- Estrae eventi MORSE da catture
- Confronta sequenze byte-per-byte
- Identifica differenze di timing
- Analisi statistica (min/max/avg delay, conteggi UP/DOWN)
- Export risultati in formato leggibile

### 3. **capture_and_compare.sh** - Workflow Automatizzato
Script bash con menu interattivo per:
- Cattura traffico CWNet (singola o comparativa)
- Analisi automatica delle catture
- Confronto client ufficiale vs ESP32
- Apertura automatica in Wireshark con dissector

### 4. **WIRESHARK_DEBUGGING.md** - Guida Completa
Documentazione dettagliata con:
- Istruzioni installazione dissector
- Filtri Wireshark specifici per CWNet
- Esempi di analisi problemi MORSE
- Script per export dati
- Troubleshooting

## Quick Start

### Installazione

```bash
cd /home/user/keyer_qrs2hst/tools

# 1. Installa dipendenze
sudo apt-get install wireshark tshark tcpdump python3 python3-pip
pip3 install scapy

# 2. Installa dissector Wireshark
mkdir -p ~/.local/lib/wireshark/plugins
cp cwnet_dissector.lua ~/.local/lib/wireshark/plugins/

# 3. Rendi eseguibili gli script (già fatto)
chmod +x capture_and_compare.sh compare_morse_captures.py
```

### Uso Base

#### Modalità Interattiva (Consigliata)
```bash
sudo ./capture_and_compare.sh
```

Seleziona dal menu:
- **Opzione 3**: Cattura comparativa completa (ufficiale + ESP32)
- Segui le istruzioni per catturare prima con client ufficiale, poi con ESP32
- Lo script confronta automaticamente e mostra le differenze

#### Modalità CLI

```bash
# Cattura singola (60 secondi)
sudo ./capture_and_compare.sh capture esp32 192.168.1.100 60

# Analizza cattura esistente
./capture_and_compare.sh analyze captures/cwnet_esp32_*.pcap

# Confronta due catture
./capture_and_compare.sh compare official.pcap esp32.pcap

# Apri in Wireshark con dissector
./capture_and_compare.sh wireshark captures/cwnet_esp32_*.pcap
```

#### Uso Diretto Python

```bash
# Confronta due catture con analisi dettagliata
python3 compare_morse_captures.py -v official.pcap esp32.pcap

# Analizza singola cattura
python3 compare_morse_captures.py esp32.pcap
```

## Workflow Debug MORSE

### 1. Setup
```bash
# Verifica connettività server
ping <server_ip>

# Identifica interfaccia di rete
ip addr show
```

### 2. Cattura Baseline (Client Ufficiale)
```bash
# Avvia cattura
sudo tcpdump -i wlan0 -s 65535 -w official.pcap "tcp port 7355 and host <server_ip>"

# In un'altra finestra: avvia client ufficiale e invia CW
# Esempio: invia "CQ CQ DE IU3QEZ K"

# Dopo invio: fermata cattura (Ctrl+C)
```

### 3. Cattura Test (ESP32)
```bash
# Avvia cattura
sudo tcpdump -i wlan0 -s 65535 -w esp32.pcap "tcp port 7355 and host <server_ip>"

# Invia STESSO messaggio con ESP32: "CQ CQ DE IU3QEZ K"

# Ferma cattura
```

### 4. Confronto
```bash
# Confronto automatico
python3 compare_morse_captures.py -v official.pcap esp32.pcap
```

Output mostra:
```
================================================================================
CONFRONTO EVENTI MORSE
================================================================================

Cattura 1: 42 eventi MORSE
Cattura 2: 38 eventi MORSE

⚠️  ATTENZIONE: Numero di eventi diverso! (diff: 4)

--------------------------------------------------------------------------------
#     Cattura 1                           Cattura 2                           Match
--------------------------------------------------------------------------------
1     DOWN @    0ms (0x80)                DOWN @    0ms (0x80)                ✓
2       UP @   80ms (0x50)                  UP @   82ms (0x52)                ✗ ⚠️
3     DOWN @   40ms (0xA8)                DOWN @   40ms (0xA8)                ✓
...
```

### 5. Analisi Wireshark

```bash
# Apri con dissector
wireshark -X lua_script:cwnet_dissector.lua -r esp32.pcap

# Applica filtro MORSE
cwnet.command.type == 0x10

# Ordina per tempo e confronta visualmente
```

## Filtri Wireshark Utili

### Focus MORSE
```
# Solo frame MORSE
cwnet.command.type == 0x10

# MORSE dal client (sostituisci IP)
cwnet.command.type == 0x10 && ip.src == 192.168.1.50

# Eventi Key DOWN
cwnet.morse.key == 1

# Timestamp sospetti (troppo brevi)
cwnet.morse.timestamp_ms < 5

# Timestamp sospetti (troppo lunghi)
cwnet.morse.timestamp_ms > 500
```

### Debug Handshake
```
# Sequenza connessione completa
cwnet.command.type == 0x01 || cwnet.command.type == 0x03

# Solo CONNECT
cwnet.command.type == 0x01

# PING con latency
cwnet.command.type == 0x03
```

## Problemi Comuni e Soluzioni

### Problema: "Numero di eventi diverso"
**Causa possibile**: ESP32 perde o duplica eventi di keying

**Debug**:
1. Controlla buffer overflow nei log ESP32
2. Verifica timing di `QueueLocalKeyEvent()`
3. Cerca pattern: quali eventi mancano? (inizio/fine/casuali?)

### Problema: "Timestamp differenti"
**Causa possibile**: Errore calcolo delta o encoding timestamp

**Debug**:
1. Verifica funzioni `EncodeTimestamp()` e `DecodeTimestamp()` in `remote_cw_client.cpp:844-866`
2. Confronta con formule in `docs/RemoteCwNetProtocol.md` (righe 185-198)
3. Controlla overflow in calcolo delta: `remote_cw_client.cpp:500-511`

### Problema: "Key UP/DOWN non bilanciati"
**Causa possibile**: Manca evento di rilascio o pressione

**Debug**:
1. Verifica stato iniziale: primo evento dovrebbe essere DOWN
2. Cerca pattern nel keyer hardware (debounce?)
3. Controlla `FlushKeyingQueue()` in `remote_cw_client.cpp:478-553`

## Analisi Avanzata

### Export eventi in CSV per grafici
```bash
tshark -r esp32.pcap -Y "cwnet.command.type == 0x10" \
    -T fields -E separator=, -E quote=d \
    -e frame.time_relative \
    -e cwnet.morse.key \
    -e cwnet.morse.timestamp_ms \
    > morse_events.csv

# Apri in LibreOffice Calc o Google Sheets per visualizzazione grafica
```

### Confronto binario frame
```bash
# Estrai payload MORSE in hex
tshark -r esp32.pcap -Y "cwnet.command.type == 0x10" -T fields -e data | head -5

# Confronta byte-per-byte con cattura ufficiale
diff <(tshark -r official.pcap -Y "cwnet.command.type == 0x10" -T fields -e data) \
     <(tshark -r esp32.pcap -Y "cwnet.command.type == 0x10" -T fields -e data)
```

## File di Output

Tutte le catture vengono salvate in:
```
./captures/
├── cwnet_official_20240615_143022.pcap
├── cwnet_esp32_20240615_143145.pcap
└── ...
```

Formato nome: `cwnet_<label>_<YYYYMMDD_HHMMSS>.pcap`

## Riferimenti

- **Protocollo CWNet**: `../docs/RemoteCwNetProtocol.md`
- **Client Implementation**: `../components/remote/remote_cw_client.cpp`
- **Codifica Timestamp**: Vedi sezione 4.3 del documento protocollo
- **Wireshark Lua API**: https://www.wireshark.org/docs/wsdg_html_chunked/wsluarm.html

## Supporto

Per problemi o miglioramenti:
1. Verifica log ESP32: `idf.py monitor`
2. Controlla documentazione: `WIRESHARK_DEBUGGING.md`
3. Rivedi codice sorgente con riferimenti line number indicati sopra

---

**Autore**: IU3QEZ
**Versione**: 1.0
**Data**: 2025-01-11
