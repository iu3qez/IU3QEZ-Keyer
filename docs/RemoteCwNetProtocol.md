# Manuale del Protocollo CWNet
## Remote CW Keyer - Client/Server Protocol Specification

**Autore codice:** Wolfgang Buescher (DL4YHF)
**Data:** 2024-01-01
**Versione documento:** 1.0

---

## Indice

1. [Introduzione](#1-introduzione)
2. [Architettura del Protocollo](#2-architettura-del-protocollo)
3. [Formato dei Messaggi](#3-formato-dei-messaggi)
4. [Codifica del CW Stream](#4-codifica-del-cw-stream)
5. [Comandi del Protocollo](#5-comandi-del-protocollo)
6. [Flusso di Connessione](#6-flusso-di-connessione)
7. [Streaming Audio](#7-streaming-audio)
8. [Gestione della Latenza](#8-gestione-della-latenza)
9. [Esempi Pratici](#9-esempi-pratici)
10. [Strutture Dati](#10-strutture-dati)

---

## 1. Introduzione

Il protocollo **CWNet** è un sistema di comunicazione binario basato su TCP/IP progettato per il controllo remoto di stazioni radio CW (telegrafia).

### Dati Trasmessi

- **Segnali di keying Morse** (key up/down con timing preciso)
- **Audio streaming** (compresso A-Law o Ogg/Vorbis)
- **Dati spettrali** (waterfall e spectrum display)
- **Comandi di controllo radio** (compatibili con rigctld/Hamlib)
- **Messaggi CI-V** (per transceiver Icom)
- **Parametri del transceiver** (frequenza, modo, potenza, etc.)

### Caratteristiche principali

- **Multiplexing binario**: Tutti i tipi di dati viaggiano sullo stesso socket TCP
- **Bassa latenza**: Ottimizzato per keying CW in tempo reale
- **Streaming parser**: Gestisce frame che possono arrivare frammentati
- **Multi-client**: Fino a 3 client simultanei per server
- **Compressione efficiente**: CW stream codificato in 1 byte per transizione

---

## 2. Architettura del Protocollo

### 2.1 Modello Client-Server

```
┌─────────────┐                    ┌─────────────┐
│   CLIENT    │◄──────TCP──────────►│   SERVER    │
│             │                     │             │
│ - Paddle    │  Morse Stream      │ - Radio TX  │
│ - GUI       │◄────────────────   │ - Spectrum  │
│ - Audio Out │                     │ - Audio In  │
│             │   Audio Stream      │             │
│             │  ────────────────►  │             │
│             │  Control Commands   │             │
│             │◄────────────────────►│             │
└─────────────┘                    └─────────────┘
```

### 2.2 Thread Model

**Server Side:**
- `ServerThread()`: Gestisce accept() e select() per connessioni multiple
- Polling interval: 20ms

**Client Side:**
- `ClientThread()`: Gestisce singola connessione con auto-reconnect

**Callback comuni:**
- `CwNet_OnReceive()`: Parser streaming per dati in ingresso
- `CwNet_ExecuteCmd()`: Esecuzione comandi ricevuti
- `CwNet_OnPoll()`: Trasmissione periodica (ogni 20ms) e housekeeping

### 2.3 Costanti di Sistema

```c
#define CWNET_MAX_CLIENTS 3
#define CWNET_DEFAULT_SERVER_PORT 7355
#define CWNET_STREAM_SAMPLING_RATE 8000
#define CWNET_SOCKET_POLLING_INTERVAL_MS 20
#define CWNET_ACTIVITY_TIMEOUT_MS 5000
```

---

## 3. Formato dei Messaggi

### 3.1 Struttura Generale

```
┌────────────┬──────────────┬────────────────┐
│  COMMAND   │ BLOCK LENGTH │    PAYLOAD     │
│  (1 byte)  │ (0,1,2 bytes)│  (0-65535 byte)│
└────────────┴──────────────┴────────────────┘
```

### 3.2 Byte di Comando

Il byte di comando codifica sia il **tipo di comando** che il **formato della lunghezza**:

```
Bit 7-6: Block Length Indicator
┌────┬────┬──────────────────────────────────────┐
│ 7  │ 6  │ Significato                          │
├────┼────┼──────────────────────────────────────┤
│ 0  │ 0  │ Nessuna lunghezza (comando semplice) │
│ 0  │ 1  │ Short block (1 byte lunghezza)       │
│ 1  │ 0  │ Long block (2 byte lunghezza)        │
│ 1  │ 1  │ Riservato / Comando speciale 0xFF    │
└────┴────┴──────────────────────────────────────┘

Bit 5-0: Command Type (0x00 - 0x3F)
```

**Maschere:**
```c
#define CWNET_CMD_MASK_BLOCKLEN    0xC0
#define CWNET_CMD_MASK_NO_BLOCK    0x00
#define CWNET_CMD_MASK_SHORT_BLOCK 0x40
#define CWNET_CMD_MASK_LONG_BLOCK  0x80
#define CWNET_CMD_MASK_COMMAND     0x3F
```

### 3.3 Block Length Encoding

**Nessun payload:**
```
[COMMAND]
```

**Short block:**
```
[COMMAND | 0x40][LENGTH][PAYLOAD]
```

**Long block:**
```
[COMMAND | 0x80][LENGTH_LO][LENGTH_HI][PAYLOAD]
```

*Nota: LENGTH in little-endian (LSB first)*

---

## 4. Codifica del CW Stream

### 4.1 Principio di Funzionamento

Il **CW Stream Encoder** converte transizioni key-up/key-down in byte singoli ottimizzati.

Come indicato nel codice sorgente:
- BIT 7: stato "key up / key down"
- BITS 6-0: tempo da attendere PRIMA di emettere bit 7

### 4.2 Formato del Byte CW

```
┌───┬───────────────────────────┐
│ 7 │   6 - 0 (7 bit)          │
├───┼───────────────────────────┤
│ K │      TIMESTAMP            │
└───┴───────────────────────────┘

K (bit 7): 1=Key DOWN, 0=Key UP
TIMESTAMP: Tempo da attendere prima di applicare K
```

### 4.3 Codifica Timestamp Non-Lineare

| Codificato | Range | Risoluzione | Formula |
|------------|-------|-------------|---------|
| 0x00-0x1F | 0-31 ms | 1 ms | t = value |
| 0x20-0x3F | 32-156 ms | 4 ms | t = 32 + (v-32)×4 |
| 0x40-0x7F | 157-1165 ms | 16 ms | t = 157 + (v-64)×16 |

**Funzioni:**

```c
BYTE CwStreamEnc_MillisecondsTo7BitTimestamp(int ms) {
    if (ms <= 31) return (BYTE)ms;
    if (ms <= 156) return 0x20 + (ms-32)/4;
    if (ms <= 1165) return 0x40 + (ms-157)/16;
    return 0x7F;
}

int CwStreamEnc_7BitTimestampToMilliseconds(BYTE ts) {
    ts &= 0x7F;
    if (ts <= 0x1F) return ts;
    if (ts <= 0x3F) return 32 + 4*(ts-0x20);
    return 157 + 16*(ts-0x40);
}
```

### 4.4 End-Of-Transmission

Due byte consecutivi con key=0 (entrambi key UP):
```
[0x0X] [0x0Y]  ← Bit 7 = 0 in entrambi
```

---

## 5. Comandi del Protocollo

### 5.1 Tabella Comandi

| Comando | Hex | Dir | Descrizione |
|---------|-----|-----|-------------|
| CONNECT | 0x01 | ⇄ | Login handshake |
| DISCONN | 0x02 | ⇄ | Disconnect |
| PING | 0x03 | ⇄ | Latency test |
| PRINT | 0x04 | ⇄ | Text message |
| TX_INFO | 0x05 | S→C | Who's transmitting |
| RIGCTLD | 0x06 | ⇄ | Hamlib command |
| MORSE | 0x10 | C→S | CW keying |
| AUDIO | 0x11 | S→C | A-Law audio |
| VORBIS | 0x12 | S→C | Ogg/Vorbis |
| CI_V | 0x14 | ⇄ | Icom CI-V |
| SPECTRUM | 0x15 | S→C | Waterfall |
| FREQ_REPORT | 0x16 | S→C | VFO state |
| PARAM_INTEGER | 0x18 | ⇄ | Integer param |
| PARAM_DOUBLE | 0x19 | ⇄ | Double param |
| PARAM_STRING | 0x1A | ⇄ | String param |
| METER_REPORT | 0x20 | S→C | Meters |
| POTI_REPORT | 0x21 | S→C | Settings |
| TUNNEL_1/2/3 | 0x31-33 | ⇄ | Serial tunnel |

### 5.2 CONNECT (0x01)

**Payload:** 92 bytes (44 + 44 + 4)

```c
typedef struct {
    char sz40UserName[44];
    char sz40Callsign[44];
    DWORD dwPermissions;  // little-endian (LSB first)
} T_CwNet_ConnectData;
```

**Permessi:**
```c
#define CWNET_PERMISSION_NONE     0x00
#define CWNET_PERMISSION_TALK     0x01
#define CWNET_PERMISSION_TRANSMIT 0x02
#define CWNET_PERMISSION_CTRL_RIG 0x04
#define CWNET_PERMISSION_ADMIN    0x08
```

### 5.3 PING (0x03)

**Payload:** 16 bytes fissi

```c
byte[0]    : Tipo (0=REQ, 1=RESP1, 2=RESP2)
byte[1]    : ID
byte[2-3]  : Reserved
byte[4-7]  : Timestamp[0]
byte[8-11] : Timestamp[1]
byte[12-15]: Timestamp[2]
```

**Sequenza:**
```
CLIENT → SERVER: REQUEST (t0)
CLIENT ← SERVER: RESPONSE_1 (t1)
CLIENT → SERVER: RESPONSE_2 (t2)

Latency ≈ (t2-t0)/2
```

### 5.4 MORSE (0x10)

**Payload:** Stream byte CW

```
0x50 0x05 0x80 0x14 0x8F 0x22 0x9F
│    │    └──┴──┴──┴──┴─ CW bytes
│    └─ Length
└─ CMD_MORSE short block
```

### 5.5 AUDIO (0x11)

**Payload:** A-Law samples (8 bit @ 8kHz)

```
0x91 0x40 0x01 [320 bytes]
│    │    │    └─ 40ms @ 8kHz
│    └────┴─ Length = 320
└─ CMD_AUDIO long block
```

### 5.6 SPECTRUM (0x15)

**Payload:** Header + bins (float32)

```c
typedef struct {
    unsigned short u16NumBinsUsed;
    unsigned short u16Reserved;
    float flt32BinWidth_Hz;
    float flt32FreqMin_Hz;
} T_CwNet_SpectrumHeader;
```

### 5.7 RIGCTLD (0x06)

**Payload:** Stringa ASCII Hamlib

Comandi:
```
set_freq <Hz>
get_freq
set_mode <mode> <bw>
get_mode
set_ptt 0|1
get_ptt
```

**Esempio:**
```
C→S: 0x46 0x0F "set_freq 7055000\0"
S→C: 0x46 0x06 "RPRT 0\0"
```

### 5.8 CI_V (0x14)

**Payload:** Pacchetto CI-V nativo

```
0x54 0x08
0xFE 0xFE 0x94 0xE0 0x03
0x00 0x55 0x70 0x07
0xFD
```

---

## 6. Flusso di Connessione

### 6.1 Handshake

```
CLIENT                          SERVER
  ├─ TCP connect ─────────────►│
  ├─ CONNECT (user/call) ──────►│
  │                              │ Validate user
  │◄─ CONNECT (permissions) ────┤
  │◄─ PRINT ("Welcome") ─────────┤
  ├─ PING (request) ───────────►│
  │◄─ PING (response_1) ─────────┤
  ├─ PING (response_2) ─────────►│
  │  → Latency measured
  │◄─ FREQ_REPORT ───────────────┤
  │◄─ POTI_REPORT ───────────────┤
  │  [Connected]
```

### 6.2 Stati Client

```c
#define CWNET_CLIENT_STATE_DISCONN    0
#define CWNET_CLIENT_STATE_ACCEPTED   1
#define CWNET_CLIENT_STATE_CONNECTED  1
#define CWNET_CLIENT_STATE_LOGIN_SENT 2
#define CWNET_CLIENT_STATE_LOGIN_CFMD 3
```

### 6.3 Autenticazione

```c
cfg.sz255AcceptUsers = "DL4YHF:7,Guest:0";
```

Permessi:
- 0 = RX only
- 1 = Talk
- 3 = Talk+TX
- 7 = Talk+TX+RigCtrl
- 15 = Admin

---

## 7. Streaming Audio

### 7.1 A-Law (Default)

- Sample rate: 8000 Hz
- Compressione: ITU-T G.711
- Bit rate: 64 kbit/s
- Latenza: ~20-40ms

**Flusso:**
1. Campiona @ 48kHz
2. Resample → 8kHz
3. Comprimi A-Law
4. Invia CMD_AUDIO

```c
// Encoder
BYTE alaw = ALAW_Compress16to8(int16);

// Decoder
short int16 = ALAW_Decompress8to16(alaw);
```

### 7.2 Vorbis (Opzionale)

- Sample rate: 8000 Hz
- Quality: 0.4-1.0
- Bit rate: ~16-64 kbit/s
- Latenza: 100-500ms

Encoder condiviso per tutti i client.

---

## 8. Gestione della Latenza

### 8.1 Misurazione (3-way ping)

```
t0: Client → REQUEST
t1: Server → RESPONSE_1
t2: Client → RESPONSE_2

Latency ≈ (t2-t0)/2
```

### 8.2 Buffering Adattivo

```c
int buffered_ms =
    CwStream_GetNumMillisecondsBufferedInFifo(&MorseRxFifo);

int target_ms = iPingLatency_ms + 50;

if (buffered_ms < target_ms) {
    fSendingFromRxFifo = FALSE;
} else {
    fSendingFromRxFifo = TRUE;
}
```

### 8.3 Configurazione

**Auto:**
```c
cfg.iNetworkLatency_ms = 0;
```

**Manuale:**
```c
cfg.iNetworkLatency_ms = 250;
```

---

## 9. Esempi Pratici

### 9.1 QSO Completo

```
Phase 1: Connection
  Client → Server: TCP connect
  Client → Server: CMD_CONNECT
  Server → Client: CMD_CONNECT (perm=0x07)
  Ping sequence → Latency: 85ms

Phase 2: Sync
  Server → Client: CMD_FREQ_REPORT
  Server → Client: CMD_POTI_REPORT
  Server → Client: CMD_AUDIO (background)

Phase 3: Client TX "CQ"
  Client → Server: CMD_MORSE
  Server: Keys radio
  Server → Client: CMD_TX_INFO
  Server → Client: CMD_AUDIO (sidetone)

Phase 4: Server responds
  Server → Client: CMD_MORSE
  Client: Decodes, plays
```

### 9.2 Controllo Frequenza

```c
// Via rigctld
Client → Server:
  0x46 0x13 "set_freq 14050000\0"

Server → Client:
  0x46 0x07 "RPRT 0\0"

Server → Client:
  0x56 [T_RigCtrl_VfoReport]
```

### 9.3 Serial Tunnel

```
Logger → COM3: 0x00 0x02 (Winkeyer)
  ↓
Client → Network: CMD_TUNNEL_1
  ↓
Server: Winkeyer emulation
  ↓
Server → Network: CMD_TUNNEL_1 (response)
  ↓
Client → COM3: 0x17 (version)
```

---

## 10. Strutture Dati

### T_CwNet

```c
typedef struct {
    struct {
        int iFunctionality;
        char sz80ClientRemoteIP[84];
        int iServerListeningPort;
        int iNetworkLatency_ms;
    } cfg;

    T_CW_KEYING_FIFO MorseTxFifo;
    T_CW_KEYING_FIFO MorseRxFifo;

    BYTE bRxBuffer[16384];
    BYTE bTxBuffer[16384];

    T_CwNetClient Client[4];

    HANDLE hThread;
    int iThreadStatus;

    T_RigCtrlInstance RigControl;
} T_CwNet;
```

### T_CW_KEYING_FIFO

```c
typedef struct {
    BYTE bCmd;
    long i32TimeOfReception_ms;
} T_CW_KEYING_FIFO_ELEMENT;

typedef struct {
    T_CW_KEYING_FIFO_ELEMENT elem[128];
    int iHeadIndex;
    int iTailIndex;
} T_CW_KEYING_FIFO;
```

---

## Conclusioni

Il protocollo CWNet è una soluzione completa per controllo remoto CW.

**Punti di forza:**

1. **Efficienza**: 1 byte per transizione CW
2. **Flessibilità**: Audio + spettro + controlli su singolo socket
3. **Compatibilità**: Comandi Hamlib standard
4. **Scalabilità**: Multi-client con permessi
5. **Robustezza**: Parser streaming, auto-reconnect

Ottimizzato per CW real-time con latenza 50-250ms.

---

## Riferimenti

- **ITU-T G.711**: A-Law compression
- **Hamlib**: [hamlib.github.io](https://hamlib.github.io)
- **Ogg Vorbis**: [xiph.org/vorbis](https://xiph.org/vorbis)
- **Icom CI-V**: CI-V Reference Manual

**File analizzati:**
- `CwNet.h` (578 righe)
- `CwNet.c` (84K+ tokens)
- `CwStreamEnc.h` (100 righe)
- `CwStreamEnc.c` (230 righe)

---

**Fine del Manuale**

*Documento generato tramite reverse engineering del codice sorgente*
*Remote CW Keyer © 2024 Wolfgang Buescher (DL4YHF)*
