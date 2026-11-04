# Cíle, postup a stav práce

Stav k 4. listopadu 2026. Tento dokument se průběžně aktualizuje. Rozpracovaný kód, úspěšný test a vydaná verze jsou samostatné stavy.

## Cíle

- Vyřešit pět omezení z README: aktualizace registru, obnova po pádu boot ovladače, návrat vadného zavaděče, určení a ověření EFI oddílu a použitelnost ikony v liště.
- Zachovat místní nastavení a uživatelská data; neobnovovat celé tovární registry přes živou instalaci.
- Udržet jednoduché rozhraní bez dekorací navíc; nové chování ověřit ve skutečné aplikaci a potom aktualizovat screenshoty.
- Ověřovat rychlost i stabilitu na LiberNT; tvrzení o výkonu opírat o měření.
- Oddělit synchronizaci zdrojů, sestavení OS, vydání klienta a publikování podepsaného feedu.

## Pracovní postup

1. Zjistit příčinu v dotčeném kódu a ověřit současné chování.
2. Udělat nejmenší úplnou změnu a připravit návrat při chybě.
3. Nechat změnu projít review a přidat test původního problému.
4. Spustit potřebné testy na LiberNT přes RemoteCmd; při přenosu porovnat SHA-256 sestavení a testovaného souboru.
5. Změny při startu ověřit skutečnými restarty; volání boot kódu ze spuštěného procesu není stejný důkaz.
6. Uložit ověřený celek do samostatného commitu a zkontrolovat související CI.
7. Aktualizovat tento přehled, návod a případně screenshoty podle skutečně dosaženého stavu.

## Hotové a ověřené části

- `01007be`: opraveno 17 nálezů z review; 58 regresních kontrol, tři skutečné restarty a testy restartu služeb. Podrobnosti jsou v [přehledu oprav](REVIEW-FIXES.md).
- Fork OS: automatická synchronizace upstreamu znovu proběhla úspěšně; workflow pouze slučuje zdroje a vypíná zděděné úlohy. Sestavení OS zůstává v dosavadním build prostředí.
- `7750899`: publisher umí připojit podepsaný odkaz na tovární stav registru; exportér má tři regresní testy.
- `b453f3b`: nativní operace registru kontrolují očekávaný obsah, zachovávají pozdější úpravy a vyžadují uložení změn. Tehdejší sada 70 kontrol prošla i na LiberNT; tato revize ještě neobsahovala úplné napojení instalace.
- OS `5fccfb5`: SMSS zpřístupňuje fázi `UpdateExecute` po inicializaci registru a před KnownDLLs a Win32. Sestavení Clang a přechod ve dvou skutečných restartech prošly; běžící systém potvrdil podporu této fáze.
- Společná instalace souboru a SOFTWARE a následný rollback prošly čtyřmi dalšími skutečnými restarty. Původní `BootExecute` i `UpdateExecute` jsou obnovené a dočasný nativní helper odstraněný.
- Aktuální klient prošel na LiberNT 95 kontrolami, včetně zachování místních změn, potvrzení registru bez zápisů, rollbacku a zákazu návratu na starý feed. SHA-256 testovaného EXE: `388de44e8c9723cce134d42b253587ba36efbbec734f0db440b617e2790c6c61`.
- Nativní helper použitý při skutečných restartech měl SHA-256 `f9fb10540bd9b5b2d663c669a2e86a3c0d8bfad1b3b22dfd54286ad98e1aac69`. Toto je důkaz pro konkrétní testovaný artefakt; novější sestavení jej automaticky nepřebírá.

## Právě probíhá

- Napojení továrních stavů registru na kontrolu, společnou transakci se soubory, potvrzení a rollback je implementované a otestované; změny zatím nejsou ve vydaném klientovi.
- Oddělení `feed-v2.json` od původního feedu, aby starší klient nedostal změny registru, které neumí použít. Publisher testy prošly; zbývá finální CI celého aktuálního celku.
- Zbývá dokončit CI aktuálního celku a ověřit přechod konkrétní starší provozní instalace.

## Důležité zjištění z testů

Původní `BootExecute` běží před `NtInitializeRegistry`. SOFTWARE ještě není připojený a `CmFlushKey` může kvůli `CmpNoWrite` vrátit úspěch bez zápisu na disk. První skutečný test proto správně odmítl zápis a obnovil původní soubory. Pouhé dočasné připojení hive by příčinu nevyřešilo. Proto je nutná fáze `UpdateExecute` a kontrola její dostupnosti; marker je v paměťovém hive `HARDWARE`, aby nezůstal po návratu ke staršímu SMSS.

Cppcheck s cestami a přepínači skutečného sestavení nenašel problém v novém `update.c`. V `sminit.c:957` hlásí původní `uninitvar`, který je i v předchozí revizi; tento nesouvisející úsek se neměnil.

## Zbývající práce

- Ověřit přechod ze staršího klienta: důvěryhodnost datové složky, původní tovární stav registru a pořadí aktualizace SMSS a klienta.
- Dořešit obnovu ovladačů, které spadnou dříve, než se updater spustí; samotná záloha jádra a HAL nestačí.
- Dořešit nezávislou návratovou cestu při selhání nového zavaděče; kód ve spuštěném systému takové selhání sám neopraví.
- Doplnit bezpečné určení EFI oddílu a ověřit skutečný UEFI boot po změně zavaděče.
- Upravit práci s ikonou v liště při zachování uživatelovy volby připnutí; potom pořídit nové screenshoty.
- Změřit náklady zpracování skutečného továrního registru a ověřit chybové HTTP scénáře nového feedu.

Nová klientská verze ani nový registry feed zatím nejsou publikované. Tento přehled není potvrzením, že všech pět omezení už zmizelo.
