#NOTA
#
# LIVE corrisponde a LATCH FALSO
# SNAPSHOT corrisponde a LATCH VERO
# mem_block_start_pct a memory_opem_percent
# mem_block_end_pct memory_close_percent

from enum import Enum, auto
from collections import deque

# =========================
# ENUM E CONFIG
# =========================

class IambicMode(Enum):
    A = auto()  # niente elemento bonus al rilascio
    B = auto()  # elemento bonus opposto al rilascio squeeze

class MemoryMode(Enum):
    NONE = auto()           # nessuna memoria
    DOT_ONLY = auto()       # memoria solo del punto
    DAH_ONLY = auto()       # memoria solo della linea
    DOT_AND_DAH = auto()    # memoria completa

class SqueezeMode(Enum:
    LIVE = auto()       # valuta squeeze in tempo reale
    SNAPSHOT = auto()   # valuta squeeze "come era al rilascio"

class Element(Enum):
    DIT = auto()
    DAH = auto()

class State(Enum):
    IDLE = auto()
    SEND_DIT = auto()
    SEND_DAH = auto()
    INTER_ELEMENT_GAP = auto()

class PaddleCombo(Enum):
    NONE = auto()
    DIT_ONLY = auto()
    DAH_ONLY = auto()
    BOTH = auto()  # squeeze


class KeyerConfig:
    def __init__(self,
                 wpm=20,
                 iambic_mode=IambicMode.B,
                 memory_mode=MemoryMode.DOT_AND_DAH,
                 squeeze_mode=SqueezeMode.LIVE,
                 mem_block_start_pct=15,   # % zona morta INIZIO elemento
                 mem_block_end_pct=15):    # % zona morta FINE elemento

        self.wpm = wpm
        self.iambic_mode = iambic_mode
        self.memory_mode = memory_mode
        self.squeeze_mode = squeeze_mode
        self.mem_block_start_pct = mem_block_start_pct
        self.mem_block_end_pct = mem_block_end_pct

    def dit_duration_ms(self):
        # convenzione classica: dit = 1200 / WPM (ms)
        return 1200.0 / self.wpm

    def dah_duration_ms(self):
        return 3.0 * self.dit_duration_ms()

    def gap_duration_ms(self):
        # gap intra-carattere tra elementi = 1 dit
        return self.dit_duration_ms()


# =========================
# RUNTIME DELLA FSM
# =========================

class KeyerRuntime:
    def __init__(self, cfg: KeyerConfig):
        self.cfg = cfg

        # Stato macchina
        self.state = State.IDLE

        # Elemento attuale
        self.current_element = None            # Element.DIT / Element.DAH / None
        self.current_element_total_ms = 0.0
        self.current_element_elapsed_ms = 0.0

        # Gap attuale
        self.gap_total_ms = 0.0
        self.gap_elapsed_ms = 0.0

        # Avanzamento percentuale elemento (0..100)
        self.element_progress_pct = 0.0

        # Coda di elementi pianificati (FIFO)
        self.queue = deque()

        # Latch memoria (dot/dash richiesti per dopo)
        self.dot_requested = False
        self.dah_requested = False

        # Stato palette (input istantaneo esterno)
        self.dit_pressed = False
        self.dah_pressed = False

        # Per gestione squeeze
        self.squeeze_seen_this_element = False   # abbiamo mai visto BOTH durante questo elemento?
        self.last_valid_combo = PaddleCombo.NONE # combo "significativa" per SNAPSHOT

    # -------------------------
    # Lettura combo attuale
    # -------------------------
    def get_combo_now(self) -> PaddleCombo:
        if self.dit_pressed and self.dah_pressed:
            return PaddleCombo.BOTH
        if self.dit_pressed and not self.dah_pressed:
            return PaddleCombo.DIT_ONLY
        if self.dah_pressed and not self.dit_pressed:
            return PaddleCombo.DAH_ONLY
        return PaddleCombo.NONE

    # -------------------------
    # Aggiorna stato paddle (chiamata esterna)
    # -------------------------
    def update_paddles(self, dit: bool, dah: bool):
        prev_combo = self.get_combo_now()

        self.dit_pressed = dit
        self.dah_pressed = dah

        new_combo = self.get_combo_now()

        # squeeze_mode influenza come memorizziamo la combo "rilevante"
        if self.cfg.squeeze_mode == SqueezeMode.SNAPSHOT:
            # SNAPSHOT:
            # aggiorno last_valid_combo sugli edge / rilasci.
            # Esempio: se da BOTH passo a NONE nel gap,
            # last_valid_combo rimane BOTH per decidere il bonus.
            if prev_combo != new_combo:
                # memorizza l'ultima situazione "stabile" prima del cambio
                self.last_valid_combo = prev_combo
        else:
            # LIVE:
            # last_valid_combo è sempre lo stato corrente
            self.last_valid_combo = new_combo

    # -------------------------
    # Avvia trasmissione di un elemento
    # -------------------------
    def _start_element(self, element: Element):
        self.current_element = element

        if element == Element.DIT:
            self.current_element_total_ms = self.cfg.dit_duration_ms()
            self.state = State.SEND_DIT
        else:
            self.current_element_total_ms = self.cfg.dah_duration_ms()
            self.state = State.SEND_DAH

        self.current_element_elapsed_ms = 0.0
        self.element_progress_pct = 0.0

        # reset info squeeze/bonus per questo elemento
        self.squeeze_seen_this_element = False

        # all'inizio di un nuovo elemento, i latch rimangono quelli vecchi
        # ma NON li consumiamo ancora

        # (Firmware reale: qui chiuderemmo la linea TX / attiveremmo sidetone)

    # -------------------------
    # Avvia gap tra elementi
    # -------------------------
    def _start_gap(self):
        self.gap_total_ms = self.cfg.gap_duration_ms()
        self.gap_elapsed_ms = 0.0
        self.state = State.INTER_ELEMENT_GAP

        # (Firmware reale: qui apriremmo TX / silezio audio)

    # -------------------------
    # Durante l'elemento:
    # - aggiorna avanzamento
    # - registra eventuale squeeze
    # - cattura la memoria (se abilitata e dentro la finestra "valida")
    # -------------------------
    def _check_memory_and_squeeze_during_element(self):
        # Avanzamento percentuale dell'elemento corrente
        if self.current_element_total_ms > 0.0:
            self.element_progress_pct = (
                100.0 * self.current_element_elapsed_ms / self.current_element_total_ms
            )
        else:
            self.element_progress_pct = 0.0

        combo = self.get_combo_now()

        # Traccia squeeze visto almeno 1 volta durante l'elemento
        if combo == PaddleCombo.BOTH:
            self.squeeze_seen_this_element = True

        # Se memoria disattivata, stop qui
        if self.cfg.memory_mode == MemoryMode.NONE:
            return

        # Controllo se siamo nella finestra attiva per la memoria:
        left_ok  = (self.element_progress_pct >= self.cfg.mem_block_start_pct)
        right_ok = (self.element_progress_pct <= (100.0 - self.cfg.mem_block_end_pct))
        mem_window_ok = left_ok and right_ok

        if not mem_window_ok:
            return

        # Se sto trasmettendo DIT, posso latchare DAH
        if self.state == State.SEND_DIT:
            # Se combo dice che l'operatore vuole DAH (o squeeze BOTH)
            # e la nostra memory_mode lo consente
            if combo in (PaddleCombo.DAH_ONLY, PaddleCombo.BOTH):
                if self.cfg.memory_mode in (MemoryMode.DAH_ONLY, MemoryMode.DOT_AND_DAH):
                    self.dah_requested = True

        # Se sto trasmettendo DAH, posso latchare DIT
        if self.state == State.SEND_DAH:
            if combo in (PaddleCombo.DIT_ONLY, PaddleCombo.BOTH):
                if self.cfg.memory_mode in (MemoryMode.DOT_ONLY, MemoryMode.DOT_AND_DAH):
                    self.dot_requested = True

    # -------------------------
    # Fine dell'elemento:
    # - calcolo eventuale bonus Mode B
    # - consumo la memoria latched
    # - passo al GAP
    # -------------------------
    def _finish_element_and_enter_gap(self):
        bonus_element = None

        # MODE B: elemento bonus
        if self.cfg.iambic_mode == IambicMode.B:
            # Chi decide se dare bonus? Dipende dallo squeeze_mode.
            # LIVE  -> guardo combo attuale (in tempo reale)
            # SNAPSHOT -> guardo last_valid_combo (stato "al rilascio")
            if self.cfg.squeeze_mode == SqueezeMode.LIVE:
                ref_combo = self.get_combo_now()
            else:
                ref_combo = self.last_valid_combo

            # Regola Mode B classica:
            # se durante l'elemento ho mai visto BOTH (squeeze),
            # e ora non sono più in BOTH,
            # aggiungi l'elemento opposto in coda.
            if self.squeeze_seen_this_element and ref_combo != PaddleCombo.BOTH:
                if self.current_element == Element.DIT:
                    bonus_element = Element.DAH
                else:
                    bonus_element = Element.DIT

        # MEMORIA (dot_requested / dah_requested):
        # Se MemoryMode.NONE, queste flags resteranno sempre False.
        if self.dot_requested:
            self.queue.append(Element.DIT)
            self.dot_requested = False
        if self.dah_requested:
            self.queue.append(Element.DAH)
            self.dah_requested = False

        # Mode B bonus va in coda dopo la memoria
        if bonus_element is not None:
            self.queue.append(bonus_element)

        # Chiudo l'elemento e avvio il gap
        self._start_gap()

    # -------------------------
    # tick() = "clock" della FSM
    # dt_ms = millisecondi trascorsi da ultimo tick
    # -------------------------
    def tick(self, dt_ms: float):
        if self.state == State.IDLE:
            # Stato di riposo: nessun elemento in trasmissione.
            # Se l'operatore tiene una o entrambe le palette,
            # partiamo subito con l'elemento iniziale.

            combo = self.get_combo_now()

            if combo == PaddleCombo.DIT_ONLY:
                self._start_element(Element.DIT)

            elif combo == PaddleCombo.DAH_ONLY:
                self._start_element(Element.DAH)

            elif combo == PaddleCombo.BOTH:
                # squeeze: per semplicità di base partiamo col DIT
                # (la logica di alternanza successiva arriva dopo)
                self._start_element(Element.DIT)

            else:
                # combo NONE → rimani fermo
                pass

        elif self.state in (State.SEND_DIT, State.SEND_DAH):
            # Stiamo trasmettendo un elemento (DIT o DAH)
            self.current_element_elapsed_ms += dt_ms

            if self.current_element_elapsed_ms < self.current_element_total_ms:
                # Elemento ancora in corso → aggiorna memoria/squeeze live
                self._check_memory_and_squeeze_during_element()
            else:
                # Elemento finito → linea TX andrà aperta, passiamo al GAP
                self._finish_element_and_enter_gap()

        elif self.state == State.INTER_ELEMENT_GAP:
            # Silenzio tra un elemento e il successivo
            self.gap_elapsed_ms += dt_ms

            if self.gap_elapsed_ms < self.gap_total_ms:
                # Ancora in gap. Possiamo aggiornare i paddle dall'esterno
                # via update_paddles(); tick non fa altro qui.
                pass
            else:
                # Fine gap: decidiamo cosa trasmettere dopo,
                # oppure se tornare IDLE.

                next_elem = None

                # 1) Hai qualcosa già in coda (da memoria o da bonus mode B)?
                if self.queue:
                    next_elem = self.queue.popleft()

                else:
                    # 2) Niente in coda → guarda lo stato attuale delle palette
                    combo = self.get_combo_now()

                    if combo == PaddleCombo.DIT_ONLY:
                        next_elem = Element.DIT

                    elif combo == PaddleCombo.DAH_ONLY:
                        next_elem = Element.DAH

                    elif combo == PaddleCombo.BOTH:
                        # iambic alternato:
                        # se ultimo elemento era DIT, adesso DAH;
                        # se ultimo era DAH, adesso DIT.
                        if self.current_element == Element.DIT:
                            next_elem = Element.DAH
                        else:
                            next_elem = Element.DIT

                    else:
                        # combo NONE → nessun nuovo elemento
                        next_elem = None

                if next_elem is not None:
                    # Partiamo col prossimo elemento
                    self._start_element(next_elem)
                else:
                    # Nessun prossimo elemento → ritorno a IDLE pulito
                    self.state = State.IDLE
                    self.current_element = None
                    self.current_element_total_ms = 0.0
                    self.current_element_elapsed_ms = 0.0
                    self.gap_total_ms = 0.0
                    self.gap_elapsed_ms = 0.0
                    self.element_progress_pct = 0.0
                    self.squeeze_seen_this_element = False
                    # NOTA: queue potrebbe avere ancora roba?
                    # qui la svuotiamo per sicurezza
                    self.queue.clear()

        else:
            # fallback di sicurezza
            self.state = State.IDLE
            self.queue.clear()
            self.current_element = None
            self.current_element_elapsed_ms = 0.0
            self.current_element_total_ms = 0.0
            self.gap_elapsed_ms = 0.0
            self.gap_total_ms = 0.0
            self.squeeze_seen_this_element = False
            self.element_progress_pct = 0.0
