# Cíle, postup a stav práce

Stav k 5. listopadu 2026. Tento dokument se průběžně aktualizuje. Rozpracovaný kód, úspěšný test a vydaná verze jsou samostatné stavy.

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
- Klient odpovídající `f9974a7` prošel na LiberNT 95 kontrolami, včetně zachování místních změn, potvrzení registru bez zápisů, rollbacku a zákazu návratu na starý feed. SHA-256 testovaného EXE: `388de44e8c9723cce134d42b253587ba36efbbec734f0db440b617e2790c6c61`.
- Nativní helper použitý při skutečných restartech měl SHA-256 `f9fb10540bd9b5b2d663c669a2e86a3c0d8bfad1b3b22dfd54286ad98e1aac69`. Toto je důkaz pro konkrétní testovaný artefakt; novější sestavení jej automaticky nepřebírá.
- `f9974a7`: napojení registru a odděleného v2 feedu je commitnuté a odeslané; [CI 41926092184](https://github.com/nks-hub/libernt-update/actions/runs/41926092184) prošlo pro klienta i publisher. Samostatně prošlo také všech pět Linux testů sestavovacího skriptu.
- Optimalizace shodných podepsaných referencí přeskočí stahování a rozebírání nepotřebného továrního registru. Na LiberNT prošlo 96 kontrol včetně testu s odstraněným objektem i cache; SHA-256 klienta je `62e7c79b8bf633b7f587bc8b2e7efcd23a45230891ab7b5b82fbb8eb3c0f9890`.
- `21ae6e8`: optimalizace registru je odeslaná; [CI 41927512019](https://github.com/nks-hub/libernt-update/actions/runs/41927512019) prošlo pro klienta i publisher.
- Nabídka ikony umí připnout a odepnout pouze vlastní ikonu. LiberNT potvrdil přesun mezi skrytými ikonami a panelem, zachování volby po ukončení aplikace i po normálním restartu celého systému a nezměněné nastavení ostatních existujících ikon.
- Nabídka se vejde nad spodní lištu; skutečný [screenshot](SCREENSHOTS.md#tray-pin-preference) je z testu na LiberNT. Testovací proces je ukončený a jeho ikona opět odepnutá.
- Klient s touto úpravou rozhraní prošel na LiberNT všemi 96 kontrolami. SHA-256 testovaného EXE: `81e83629ca58af4b8023e3d51c467e75c67f6711a5330a814df941ef119e9a71`. Tento běh nenahrazuje dřívější důkaz nativních operací při startu.

## Naměřený výkon registru

Testovací VM měla 2 CPU a 2 GiB RAM. Skutečný tovární export obsahoval 6 884 571 bajtů, 16 382 klíčů a 26 947 hodnot. Dvě měření dala 565–567 ms pro načtení a rozebrání JSON a 861–872 ms pro úplné porovnání stejných základů. Použitý testovací příkaz je `boot_vm.exe registry-snapshot <soubor>`.

Při shodných podepsaných hashech se nyní tato práce neprovádí. Uvedená čísla měří pouze zpracování registru; nezahrnují síť, porovnání systémových souborů ani celou instalaci.

## Právě probíhá

- Registr i úprava ikony jsou implementované a otestované; změny zatím nejsou ve vydaném klientovi.
- Ověření přechodu konkrétní starší provozní instalace; nový klient ani nový v2 feed zatím nejsou publikované.

## Důležité zjištění z testů

Původní `BootExecute` běží před `NtInitializeRegistry`. SOFTWARE ještě není připojený a `CmFlushKey` může kvůli `CmpNoWrite` vrátit úspěch bez zápisu na disk. První skutečný test proto správně odmítl zápis a obnovil původní soubory. Pouhé dočasné připojení hive by příčinu nevyřešilo. Proto je nutná fáze `UpdateExecute` a kontrola její dostupnosti; marker je v paměťovém hive `HARDWARE`, aby nezůstal po návratu ke staršímu SMSS.

Cppcheck s cestami a přepínači skutečného sestavení nenašel problém v novém `update.c`. V `sminit.c:957` hlásí původní `uninitvar`, který je i v předchozí revizi; tento nesouvisející úsek se neměnil.

Kernel při každém startu zapisuje `FirmwareBootDevice` podle oddílu zavaděče a `SystemBootDevice` podle oddílu Windows. První údaj může pomoci s hledáním EFI oddílu, ale UEFI loader při neúspěšném hledání dosadí OS oddíl bez zvláštního příznaku. Proto samotné přečtení této hodnoty nestačí k bezpečné aktualizaci zavaděče. Je nutné ověřit oddíl i očekávaný loader; při rozporu nadále vyžadovat `bootRoot`.

## Zbývající práce

- Ověřit přechod ze staršího klienta: důvěryhodnost datové složky, původní tovární stav registru a pořadí aktualizace SMSS a klienta.
- Dořešit obnovu ovladačů, které spadnou dříve, než se updater spustí; samotná záloha jádra a HAL nestačí.
- Dořešit nezávislou návratovou cestu při selhání nového zavaděče; kód ve spuštěném systému takové selhání sám neopraví.
- Upřesnit, zda nezávislá obnova musí fungovat také na fyzickém počítači; rozhodnutí zatím chybí a postup obnovy není implementovaný.
- Doplnit bezpečné určení EFI oddílu a ověřit skutečný UEFI boot po změně zavaděče.
- Ověřit chybové HTTP scénáře nového feedu; současné regrese fallbacku používají místní feed.
- Upřesnit release kanál pro testovaná a provozní vydání. Synchronizace fork zdrojů nepřebírá upstream GitHub Releases; vlastní binární vydání patří do stávajícího sestavovacího postupu.

Nová klientská verze ani nový registry feed zatím nejsou publikované. Tento přehled není potvrzením, že všech pět omezení už zmizelo.
