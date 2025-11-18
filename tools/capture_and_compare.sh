#!/bin/bash
#
# CWNet Capture and Compare Tool
# Automatizza cattura e confronto traffico MORSE tra client ufficiale e ESP32
#

set -e

# Colori per output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configurazione
CWNET_PORT=7355
CAPTURES_DIR="./captures"
DISSECTOR_PATH="$(dirname "$0")/cwnet_dissector.lua"
COMPARE_SCRIPT="$(dirname "$0")/compare_morse_captures.py"

# Funzioni helper
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[OK]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "Questo script richiede privilegi root per catturare pacchetti"
        echo "Riavvia con: sudo $0 $@"
        exit 1
    fi
}

check_dependencies() {
    local missing=0

    for cmd in tcpdump tshark python3; do
        if ! command -v "$cmd" &> /dev/null; then
            log_error "Dipendenza mancante: $cmd"
            missing=1
        fi
    done

    if ! python3 -c "import scapy" 2>/dev/null; then
        log_error "Modulo Python 'scapy' non installato"
        log_info "Installa con: pip3 install scapy"
        missing=1
    fi

    if [[ $missing -eq 1 ]]; then
        exit 1
    fi

    log_success "Tutte le dipendenze sono soddisfatte"
}

show_interfaces() {
    log_info "Interfacce di rete disponibili:"
    ip link show | grep -E "^[0-9]+:" | awk -F': ' '{print "  - " $2}'
}

capture_traffic() {
    local label=$1
    local server_ip=$2
    local duration=$3
    local interface=${4:-any}

    local output_file="${CAPTURES_DIR}/cwnet_${label}_$(date +%Y%m%d_%H%M%S).pcap"

    mkdir -p "$CAPTURES_DIR"

    log_info "Cattura in corso: $label"
    log_info "Server: $server_ip | Durata: ${duration}s | Interfaccia: $interface"
    log_info "Output: $output_file"

    echo ""
    echo -e "${YELLOW}>>> Inizia la sessione CW adesso! <<<${NC}"
    echo ""

    timeout "$duration" tcpdump \
        -i "$interface" \
        -s 65535 \
        -w "$output_file" \
        "tcp port $CWNET_PORT and host $server_ip" \
        2>/dev/null || true

    if [[ -f "$output_file" ]]; then
        local size=$(du -h "$output_file" | cut -f1)
        log_success "Cattura completata: $output_file ($size)"
        echo "$output_file"
    else
        log_error "Cattura fallita"
        return 1
    fi
}

analyze_capture() {
    local pcap_file=$1

    log_info "Analisi cattura: $(basename "$pcap_file")"

    # Conta pacchetti CWNet
    local total_packets=$(tshark -r "$pcap_file" -Y "tcp.port == $CWNET_PORT" 2>/dev/null | wc -l)
    log_info "Pacchetti TCP sulla porta $CWNET_PORT: $total_packets"

    # Estrai statistiche MORSE
    if python3 "$COMPARE_SCRIPT" "$pcap_file" 2>/dev/null; then
        log_success "Analisi completata"
    else
        log_warning "Analisi parzialmente fallita (verifica formato cattura)"
    fi
}

compare_captures() {
    local capture1=$1
    local capture2=$2

    log_info "Confronto catture:"
    log_info "  [1] $(basename "$capture1")"
    log_info "  [2] $(basename "$capture2")"

    echo ""
    python3 "$COMPARE_SCRIPT" -v "$capture1" "$capture2"
}

open_in_wireshark() {
    local pcap_file=$1

    if command -v wireshark &> /dev/null; then
        log_info "Apertura in Wireshark: $(basename "$pcap_file")"

        # Carica il dissector automaticamente
        if [[ -f "$DISSECTOR_PATH" ]]; then
            wireshark -X lua_script:"$DISSECTOR_PATH" -r "$pcap_file" &
        else
            log_warning "Dissector non trovato: $DISSECTOR_PATH"
            wireshark -r "$pcap_file" &
        fi

        log_success "Wireshark avviato"
    else
        log_warning "Wireshark non installato, apri manualmente: $pcap_file"
    fi
}

# Menu interattivo
menu_interactive() {
    check_dependencies

    echo ""
    echo "=========================================="
    echo "  CWNet Debug Tool - Menu Interattivo"
    echo "=========================================="
    echo ""
    echo "1) Cattura singola (client ufficiale)"
    echo "2) Cattura singola (ESP32)"
    echo "3) Cattura comparativa (ufficiale + ESP32)"
    echo "4) Analizza cattura esistente"
    echo "5) Confronta due catture esistenti"
    echo "6) Apri cattura in Wireshark"
    echo "7) Lista catture salvate"
    echo "8) Esci"
    echo ""

    read -p "Scelta [1-8]: " choice

    case $choice in
        1)
            read -p "IP server: " server_ip
            read -p "Durata cattura (secondi) [60]: " duration
            duration=${duration:-60}
            read -p "Interfaccia di rete [any]: " interface
            interface=${interface:-any}

            check_root
            capture_file=$(capture_traffic "official" "$server_ip" "$duration" "$interface")
            analyze_capture "$capture_file"
            ;;

        2)
            read -p "IP server: " server_ip
            read -p "Durata cattura (secondi) [60]: " duration
            duration=${duration:-60}
            read -p "Interfaccia di rete [any]: " interface
            interface=${interface:-any}

            check_root
            capture_file=$(capture_traffic "esp32" "$server_ip" "$duration" "$interface")
            analyze_capture "$capture_file"
            ;;

        3)
            read -p "IP server: " server_ip
            read -p "Durata cattura PER SESSIONE (secondi) [60]: " duration
            duration=${duration:-60}
            read -p "Interfaccia di rete [any]: " interface
            interface=${interface:-any}

            check_root

            log_info "FASE 1/2: Cattura client ufficiale"
            echo "Premi INVIO quando sei pronto..."
            read
            capture1=$(capture_traffic "official" "$server_ip" "$duration" "$interface")

            log_info "FASE 2/2: Cattura ESP32"
            echo "Premi INVIO quando sei pronto..."
            read
            capture2=$(capture_traffic "esp32" "$server_ip" "$duration" "$interface")

            log_info "Confronto in corso..."
            compare_captures "$capture1" "$capture2"
            ;;

        4)
            ls -1 "$CAPTURES_DIR"/*.pcap 2>/dev/null || log_error "Nessuna cattura trovata"
            read -p "File da analizzare: " capture_file
            if [[ -f "$capture_file" ]]; then
                analyze_capture "$capture_file"
            else
                log_error "File non trovato: $capture_file"
            fi
            ;;

        5)
            ls -1 "$CAPTURES_DIR"/*.pcap 2>/dev/null || log_error "Nessuna cattura trovata"
            read -p "Prima cattura: " capture1
            read -p "Seconda cattura: " capture2
            if [[ -f "$capture1" ]] && [[ -f "$capture2" ]]; then
                compare_captures "$capture1" "$capture2"
            else
                log_error "File non trovato"
            fi
            ;;

        6)
            ls -1 "$CAPTURES_DIR"/*.pcap 2>/dev/null || log_error "Nessuna cattura trovata"
            read -p "File da aprire: " capture_file
            if [[ -f "$capture_file" ]]; then
                open_in_wireshark "$capture_file"
            else
                log_error "File non trovato: $capture_file"
            fi
            ;;

        7)
            log_info "Catture salvate in $CAPTURES_DIR:"
            if ls -1 "$CAPTURES_DIR"/*.pcap 2>/dev/null; then
                echo ""
                du -h "$CAPTURES_DIR"/*.pcap
            else
                log_warning "Nessuna cattura trovata"
            fi
            ;;

        8)
            log_info "Arrivederci!"
            exit 0
            ;;

        *)
            log_error "Scelta non valida"
            ;;
    esac
}

# Modalità CLI
usage() {
    cat << EOF
Uso: $0 [COMMAND] [OPTIONS]

Comandi:
  capture <label> <server_ip> <duration> [interface]
      Cattura traffico CWNet

  analyze <pcap_file>
      Analizza cattura esistente

  compare <pcap1> <pcap2>
      Confronta due catture

  wireshark <pcap_file>
      Apri cattura in Wireshark con dissector

  interactive
      Modalità menu interattivo (default)

Esempi:
  $0 capture official 192.168.1.100 60 wlan0
  $0 analyze ./captures/cwnet_official_20240101_120000.pcap
  $0 compare official.pcap esp32.pcap
  $0 interactive

EOF
    exit 1
}

# Main
if [[ $# -eq 0 ]]; then
    menu_interactive
else
    case $1 in
        capture)
            [[ $# -lt 4 ]] && usage
            check_root
            check_dependencies
            capture_file=$(capture_traffic "$2" "$3" "$4" "${5:-any}")
            analyze_capture "$capture_file"
            ;;

        analyze)
            [[ $# -lt 2 ]] && usage
            check_dependencies
            analyze_capture "$2"
            ;;

        compare)
            [[ $# -lt 3 ]] && usage
            check_dependencies
            compare_captures "$2" "$3"
            ;;

        wireshark)
            [[ $# -lt 2 ]] && usage
            open_in_wireshark "$2"
            ;;

        interactive)
            menu_interactive
            ;;

        *)
            usage
            ;;
    esac
fi
