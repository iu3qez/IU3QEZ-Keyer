#!/usr/bin/env python3
"""
CWNet MORSE Capture Comparator

Confronta due catture Wireshark (.pcap) per identificare differenze nei messaggi MORSE.
Utile per debug: confronta client ufficiale vs implementazione ESP32.

Uso:
    python3 compare_morse_captures.py cwnet_official.pcap cwnet_esp32.pcap

Requisiti:
    pip3 install scapy
"""

import sys
import argparse
from collections import namedtuple
from typing import List, Tuple

try:
    from scapy.all import rdpcap, TCP, Raw
except ImportError:
    print("ERROR: scapy non installato. Installa con: pip3 install scapy")
    sys.exit(1)


MorseEvent = namedtuple('MorseEvent', ['frame_num', 'timestamp', 'src_ip', 'dst_ip',
                                        'key_state', 'delay_ms', 'raw_byte'])


def decode_timestamp(ts_encoded):
    """Decodifica timestamp non-lineare a 7 bit in millisecondi (protocollo CWNet)"""
    ts = ts_encoded & 0x7F
    if ts <= 0x1F:
        return ts
    elif ts <= 0x3F:
        return 32 + 4 * (ts - 0x20)
    else:
        return 157 + 16 * (ts - 0x40)


def parse_cwnet_morse_frame(payload: bytes) -> List[Tuple[bool, int, int]]:
    """
    Estrae eventi MORSE da payload CWNet.

    Returns:
        Lista di tuple (key_down, delay_ms, raw_byte)
    """
    events = []
    i = 0
    while i < len(payload):
        cmd_byte = payload[i]
        cmd_type = cmd_byte & 0x3F
        len_type = cmd_byte & 0xC0

        # Solo frame MORSE (0x10)
        if cmd_type != 0x10:
            # Salta frame non-MORSE
            if len_type == 0x00:
                i += 1
            elif len_type == 0x40:
                if i + 1 < len(payload):
                    frame_len = payload[i + 1]
                    i += 2 + frame_len
                else:
                    break
            elif len_type == 0x80:
                if i + 2 < len(payload):
                    frame_len = payload[i + 1] | (payload[i + 2] << 8)
                    i += 3 + frame_len
                else:
                    break
            else:
                break
            continue

        # Frame MORSE
        if len_type == 0x40:  # Short block
            if i + 1 >= len(payload):
                break
            morse_len = payload[i + 1]
            i += 2

            for j in range(morse_len):
                if i + j >= len(payload):
                    break
                cw_byte = payload[i + j]
                key_down = (cw_byte & 0x80) != 0
                ts_raw = cw_byte & 0x7F
                delay_ms = decode_timestamp(ts_raw)
                events.append((key_down, delay_ms, cw_byte))

            i += morse_len
        else:
            # MORSE dovrebbe sempre usare short block
            i += 1

    return events


def extract_morse_events(pcap_file: str) -> List[MorseEvent]:
    """Estrae tutti gli eventi MORSE da un file .pcap"""
    packets = rdpcap(pcap_file)
    morse_events = []
    frame_num = 0

    for pkt in packets:
        frame_num += 1

        if not pkt.haslayer(TCP):
            continue

        tcp_layer = pkt[TCP]

        # Filtra porta CWNet (default 7355)
        if tcp_layer.sport != 7355 and tcp_layer.dport != 7355:
            continue

        if not pkt.haslayer(Raw):
            continue

        payload = bytes(pkt[Raw].load)

        try:
            events = parse_cwnet_morse_frame(payload)
            for key_down, delay_ms, raw_byte in events:
                morse_event = MorseEvent(
                    frame_num=frame_num,
                    timestamp=float(pkt.time),
                    src_ip=pkt[1].src if hasattr(pkt[1], 'src') else "unknown",
                    dst_ip=pkt[1].dst if hasattr(pkt[1], 'dst') else "unknown",
                    key_state="DOWN" if key_down else "UP",
                    delay_ms=delay_ms,
                    raw_byte=raw_byte
                )
                morse_events.append(morse_event)
        except Exception as e:
            # Ignora frame malformati
            continue

    return morse_events


def compare_morse_sequences(events1: List[MorseEvent], events2: List[MorseEvent]) -> None:
    """Confronta due sequenze di eventi MORSE e mostra differenze"""

    print(f"\n{'='*80}")
    print(f"CONFRONTO EVENTI MORSE")
    print(f"{'='*80}\n")

    print(f"Cattura 1: {len(events1)} eventi MORSE")
    print(f"Cattura 2: {len(events2)} eventi MORSE")

    if len(events1) != len(events2):
        print(f"\n⚠️  ATTENZIONE: Numero di eventi diverso! (diff: {abs(len(events1) - len(events2))})")

    print(f"\n{'-'*80}")
    print(f"{'#':<5} {'Cattura 1':<35} {'Cattura 2':<35} {'Match'}")
    print(f"{'-'*80}")

    differences = 0
    max_compare = min(len(events1), len(events2))

    for i in range(max_compare):
        e1 = events1[i]
        e2 = events2[i]

        # Confronta key state e delay
        match = (e1.key_state == e2.key_state and e1.delay_ms == e2.delay_ms)
        match_str = "✓" if match else "✗"

        if not match:
            differences += 1

        e1_str = f"{e1.key_state:>4} @ {e1.delay_ms:4d}ms (0x{e1.raw_byte:02X})"
        e2_str = f"{e2.key_state:>4} @ {e2.delay_ms:4d}ms (0x{e2.raw_byte:02X})"

        # Evidenzia differenze
        if not match:
            print(f"{i+1:<5} {e1_str:<35} {e2_str:<35} {match_str} ⚠️")
        else:
            print(f"{i+1:<5} {e1_str:<35} {e2_str:<35} {match_str}")

    # Eventi extra in una delle due catture
    if len(events1) > max_compare:
        print(f"\n⚠️  Cattura 1 ha {len(events1) - max_compare} eventi extra:")
        for i in range(max_compare, len(events1)):
            e = events1[i]
            print(f"   [{i+1}] {e.key_state:>4} @ {e.delay_ms:4d}ms (0x{e.raw_byte:02X})")

    if len(events2) > max_compare:
        print(f"\n⚠️  Cattura 2 ha {len(events2) - max_compare} eventi extra:")
        for i in range(max_compare, len(events2)):
            e = events2[i]
            print(f"   [{i+1}] {e.key_state:>4} @ {e.delay_ms:4d}ms (0x{e.raw_byte:02X})")

    print(f"\n{'-'*80}")
    print(f"RIEPILOGO:")
    print(f"  Eventi confrontati: {max_compare}")
    print(f"  Differenze trovate: {differences}")
    if differences > 0:
        print(f"  ⚠️  Accuracy: {100 * (max_compare - differences) / max_compare:.1f}%")
    else:
        print(f"  ✓ Sequenze identiche!")
    print(f"{'='*80}\n")


def analyze_timing_patterns(events: List[MorseEvent], label: str) -> None:
    """Analizza pattern di timing in una sequenza MORSE"""
    if not events:
        return

    print(f"\n{'-'*80}")
    print(f"ANALISI TIMING: {label}")
    print(f"{'-'*80}")

    delays = [e.delay_ms for e in events]

    print(f"  Eventi totali: {len(events)}")
    print(f"  Delay minimo: {min(delays)} ms")
    print(f"  Delay massimo: {max(delays)} ms")
    print(f"  Delay medio: {sum(delays) / len(delays):.1f} ms")

    # Conta transizioni
    key_downs = sum(1 for e in events if e.key_state == "DOWN")
    key_ups = sum(1 for e in events if e.key_state == "UP")

    print(f"  Key DOWN: {key_downs}")
    print(f"  Key UP: {key_ups}")

    if key_downs != key_ups:
        print(f"  ⚠️  ATTENZIONE: Numero di UP/DOWN non bilanciato!")

    # Cerca End-Of-Transmission (due UP consecutivi)
    eot_count = 0
    for i in range(len(events) - 1):
        if events[i].key_state == "UP" and events[i+1].key_state == "UP":
            eot_count += 1

    if eot_count > 0:
        print(f"  End-Of-Transmission rilevati: {eot_count}")


def main():
    parser = argparse.ArgumentParser(
        description="Confronta catture CWNet per debug messaggi MORSE",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Esempi:
  # Confronta client ufficiale vs ESP32
  %(prog)s official.pcap esp32.pcap

  # Analizza singola cattura
  %(prog)s esp32.pcap

  # Con analisi timing dettagliata
  %(prog)s -v official.pcap esp32.pcap
        """
    )

    parser.add_argument('capture1', help='Prima cattura .pcap')
    parser.add_argument('capture2', nargs='?', help='Seconda cattura .pcap (opzionale)')
    parser.add_argument('-v', '--verbose', action='store_true',
                       help='Mostra analisi timing dettagliata')

    args = parser.parse_args()

    print(f"\n{'='*80}")
    print("CWNet MORSE Capture Comparator")
    print(f"{'='*80}\n")

    print(f"Parsing {args.capture1}...")
    events1 = extract_morse_events(args.capture1)

    if args.verbose:
        analyze_timing_patterns(events1, args.capture1)

    if args.capture2:
        print(f"Parsing {args.capture2}...")
        events2 = extract_morse_events(args.capture2)

        if args.verbose:
            analyze_timing_patterns(events2, args.capture2)

        compare_morse_sequences(events1, events2)
    else:
        print(f"\n✓ Trovati {len(events1)} eventi MORSE")

        if events1:
            print(f"\nPrimi 10 eventi:")
            print(f"{'-'*60}")
            for i, e in enumerate(events1[:10]):
                print(f"  [{i+1}] {e.key_state:>4} after {e.delay_ms:4d}ms "
                      f"(0x{e.raw_byte:02X}) @ frame {e.frame_num}")

            if len(events1) > 10:
                print(f"  ... ({len(events1) - 10} eventi non mostrati)")


if __name__ == '__main__':
    main()
