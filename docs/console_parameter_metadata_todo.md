# Console Parameter Metadata TODO

## Contesto
L'integrazione della ConsoleCommandHandler con il nuovo Parameter Metadata System è stata interrotta per non interferire con lo sviluppo in corso della console (TUI, status bar avanzata, ecc.). Il registro dei parametri e i template (inclusi i nuovi `StringParameter`) sono già disponibili e pronti all'uso, ma la console continua a utilizzare il percorso legacy finché l'integrazione non verrà completata in coordinamento con il team console.

## Obiettivi rimandati
- **Routing dei comandi**: aggiungere i sottosistemi `general` e `wifi`, instradando i comandi tramite `ParameterRegistry::Execute`.
- **Help auto-generato**: sostituire i blocchi `ConsolePrintf` hard-coded con `GenerateHelpText()` per `audio`, `keying`, `general`, `wifi`.
- **Comandi WiFi**: sostituire i parser manuali con il metadata system (`wifi.sta_ssid`, `wifi.ap_ssid`, ecc.) e integrare le chiamate a `WiFiSubsystem`.
- **Sidetone enable**: usare `audio.enabled` (bool) dal registro, gestendo start/stop della sidetone service.
- **Persistenza**: dopo l’esecuzione di un parametro, valutare se serve chiamare `Storage::Save` o rimandarlo ai comandi `save`.
- **Messaggistica**: convogliare i messaggi `OK/ERR` coerenti con quelli restituiti da `Parameter::Execute`.
- **Visibilità condizionale**: assicurare che i comandi console rispettino `IsVisible()` (es. parametri manual-mode).
- **Test di regressione**: verificare che i comandi esistenti (`audio`, `keying`, `config`) restino funzionanti.

## Pre-requisiti tecnici
- Il `ParameterRegistry` deve essere già popolato da `ApplicationController` (done).
- La console deve ricevere i puntatori a `ParameterRegistry` e `WiFiSubsystem` (binding da ripristinare quando il team console dà l'ok).
- Gli handler legacy dovranno essere mantenuti fino al completamento della migrazione per evitare regressioni immediate.

## Prossimi passi suggeriti
1. Concordare con il team console la finestra di integrazione e il formato messaggi TUI.
2. Implementare un wrapper `ExecuteParameterCommand()` che traghetti risultati e messaggi dal registry.
3. Migrare i sottocomandi uno alla volta (audio → keying → general → wifi) verificando ogni stadio via console manuale.
4. Aggiornare `docs/CONSOLE.md` e `docs/journal.md` una volta completata la migrazione.
5. Valutare test host-side per i nuovi percorsi (mock USB console).

_Documento creato per tracciare il lavoro console sospeso e coordinare la futura integrazione col Parameter Metadata System._
