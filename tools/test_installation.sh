#!/bin/bash
#
# Test installazione CWNet Debug Tools
#

set -e

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

SCRIPT_DIR="$(dirname "$0")"
TESTS_PASSED=0
TESTS_FAILED=0

test_pass() {
    echo -e "${GREEN}✓${NC} $1"
    ((TESTS_PASSED++))
}

test_fail() {
    echo -e "${RED}✗${NC} $1"
    ((TESTS_FAILED++))
}

test_warn() {
    echo -e "${YELLOW}⚠${NC} $1"
}

echo "=========================================="
echo "  CWNet Debug Tools - Test Installazione"
echo "=========================================="
echo ""

# Test 1: File esistenti
echo "Test 1: Verifica file tools..."
for file in cwnet_dissector.lua compare_morse_captures.py capture_and_compare.sh WIRESHARK_DEBUGGING.md README.md; do
    if [[ -f "$SCRIPT_DIR/$file" ]]; then
        test_pass "$file presente"
    else
        test_fail "$file MANCANTE"
    fi
done

# Test 2: Permessi esecuzione
echo ""
echo "Test 2: Permessi esecuzione..."
for script in compare_morse_captures.py capture_and_compare.sh; do
    if [[ -x "$SCRIPT_DIR/$script" ]]; then
        test_pass "$script eseguibile"
    else
        test_fail "$script NON eseguibile"
        test_warn "Esegui: chmod +x $SCRIPT_DIR/$script"
    fi
done

# Test 3: Dipendenze sistema
echo ""
echo "Test 3: Dipendenze sistema..."
for cmd in tcpdump tshark wireshark python3; do
    if command -v "$cmd" &> /dev/null; then
        test_pass "$cmd installato"
    else
        test_fail "$cmd NON installato"
        case $cmd in
            tcpdump|tshark|wireshark)
                test_warn "Installa con: sudo apt-get install $cmd"
                ;;
            python3)
                test_warn "Installa con: sudo apt-get install python3"
                ;;
        esac
    fi
done

# Test 4: Dipendenze Python
echo ""
echo "Test 4: Dipendenze Python..."
if command -v python3 &> /dev/null; then
    if python3 -c "import scapy" 2>/dev/null; then
        test_pass "scapy installato"
    else
        test_fail "scapy NON installato"
        test_warn "Installa con: pip3 install scapy"
    fi
else
    test_fail "Python3 non disponibile, impossibile testare scapy"
fi

# Test 5: Dissector Wireshark
echo ""
echo "Test 5: Installazione dissector..."
WIRESHARK_PLUGIN_DIR="$HOME/.local/lib/wireshark/plugins"
if [[ -f "$WIRESHARK_PLUGIN_DIR/cwnet_dissector.lua" ]]; then
    test_pass "Dissector installato in $WIRESHARK_PLUGIN_DIR"
else
    test_warn "Dissector NON installato nella directory plugin di Wireshark"
    echo "   Installa con:"
    echo "   mkdir -p $WIRESHARK_PLUGIN_DIR"
    echo "   cp $SCRIPT_DIR/cwnet_dissector.lua $WIRESHARK_PLUGIN_DIR/"
fi

# Test 6: Sintassi Lua dissector
echo ""
echo "Test 6: Validazione sintassi dissector..."
if command -v lua &> /dev/null || command -v luac &> /dev/null; then
    if luac -p "$SCRIPT_DIR/cwnet_dissector.lua" 2>/dev/null; then
        test_pass "Sintassi Lua valida"
    else
        test_fail "Errori di sintassi in cwnet_dissector.lua"
    fi
else
    test_warn "Lua/luac non disponibile, validazione syntax saltata"
fi

# Test 7: Sintassi Python
echo ""
echo "Test 7: Validazione sintassi Python..."
if python3 -m py_compile "$SCRIPT_DIR/compare_morse_captures.py" 2>/dev/null; then
    test_pass "Sintassi Python valida"
    rm -f "$SCRIPT_DIR/__pycache__/compare_morse_captures.cpython-"*.pyc 2>/dev/null
    rmdir "$SCRIPT_DIR/__pycache__" 2>/dev/null || true
else
    test_fail "Errori di sintassi in compare_morse_captures.py"
fi

# Test 8: Sintassi Bash
echo ""
echo "Test 8: Validazione sintassi Bash..."
if bash -n "$SCRIPT_DIR/capture_and_compare.sh" 2>/dev/null; then
    test_pass "Sintassi Bash valida"
else
    test_fail "Errori di sintassi in capture_and_compare.sh"
fi

# Test 9: Directory captures
echo ""
echo "Test 9: Directory output..."
CAPTURES_DIR="$SCRIPT_DIR/captures"
if [[ -d "$CAPTURES_DIR" ]]; then
    test_pass "Directory captures/ già esistente"
    capture_count=$(ls -1 "$CAPTURES_DIR"/*.pcap 2>/dev/null | wc -l)
    if [[ $capture_count -gt 0 ]]; then
        echo "   Trovate $capture_count catture esistenti"
    fi
else
    test_warn "Directory captures/ non esiste (verrà creata al primo uso)"
fi

# Riepilogo
echo ""
echo "=========================================="
echo "  Riepilogo Test"
echo "=========================================="
echo -e "Test superati: ${GREEN}$TESTS_PASSED${NC}"
echo -e "Test falliti:  ${RED}$TESTS_FAILED${NC}"
echo ""

if [[ $TESTS_FAILED -eq 0 ]]; then
    echo -e "${GREEN}✓ Installazione completa e funzionante!${NC}"
    echo ""
    echo "Prossimi passi:"
    echo "  1. Avvia cattura: sudo ./capture_and_compare.sh"
    echo "  2. Leggi guida:   cat WIRESHARK_DEBUGGING.md"
    echo "  3. Test dissector: wireshark -X lua_script:cwnet_dissector.lua"
    exit 0
else
    echo -e "${RED}✗ Installazione incompleta${NC}"
    echo ""
    echo "Risolvi i problemi evidenziati sopra e rilancia questo script."
    exit 1
fi
