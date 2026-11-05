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
- `d102bde`: připnutí ikony, oprava nabídky a screenshot jsou odeslané; [CI 41934024700](https://github.com/nks-hub/libernt-update/actions/runs/41934024700) prošlo pro klienta i publisher.
- Na LiberNT prošlo devět HTTP kontrol: přechod na starý feed pouze při 404 před přijetím v2, odmítnutí 403/500, chybějícího či nesprávného podpisu a neplatného JSON, přijetí v2, zákaz pozdějšího návratu na starý protokol a limit velikosti odpovědi. Test používá pouze dočasný port na `127.0.0.1` a syntetická data; nejde o veřejný aktualizační server.
- HTTP testy prošly review a jsou součástí CI. SHA-256 testovaného helperu: `4bb1f43264385f27b599f07e88b1bb83077dc872305945f17094ac55cc1e7147`; výsledky dalších sestavení jsou v [CI](https://github.com/nks-hub/libernt-update/actions/workflows/ci.yml).
- `1ff805e`: HTTP regrese a jejich napojení do CI jsou odeslané; [CI 41934672654](https://github.com/nks-hub/libernt-update/actions/runs/41934672654) prošlo včetně Windows běhu všech devíti HTTP kontrol.
- Kanály vydání klienta: verze s příponou, například `v0.3.0-rc.1`, je GitHub prerelease a nenahradí stabilní latest. Běžný publisher odmítá draft i nevyžádanou předběžnou verzi; pro samostatný testovací feed lze zvolit přesný `CLIENT_TAG`. Devět Linux regresí publisher skriptu prošlo; nová verze tím není vydaná.
- `9353df0`: kanály klienta jsou odeslané a [CI 41996328546](https://github.com/nks-hub/libernt-update/actions/runs/41996328546) prošlo.
- Migrace staré datové složky prošla na LiberNT 106 kontrolami, včetně odmítnutí poškozeného feedu, ochrany rozpracované obnovy, zachování původních bajtů a návratu po neúspěšném přesunu. SHA-256 klienta: `c2909a570b2c045db1665255ada9f510cc74ab4704ef7fce0e58d5c82b5f75a0`.
- Skutečná starší instalace prošla `/migrate-legacy`, instalací testovaného klienta, registrací služby, `/check` a normálním restartem. Služba běží se shodným SHA-256, nová složka prošla kontrolou oprávnění a `pending` je prázdný. Automatické instalace zůstávají vypnuté.
- Starý připravený plán 1 194 souborů nebyl aplikován. Hash jádra po restartu zůstal `047e21f4200a0cf11ca1e0e363b1f3915377f43f27e76814df1fd06ce5d1422d`; archivované `state.json`, `history.json` a `boot/plan.txt` se bajtově shodují s původními soubory.
- Závěrečná oprava z review odmítá také adresář místo příznaku obnovy a nepřístupný boot adresář; kontrola se opakuje po zastavení služby. Rozšířená sada na LiberNT prošla 108 kontrolami. SHA-256 klienta: `5cd0e7038013ad86d8d4d44be3eb10da46ccd9c19066b0372439a244fb32eb96`.
- `18b6a96`: migrace je odeslaná a [CI 42005500867](https://github.com/nks-hub/libernt-update/actions/runs/42005500867) prošlo. Služba ve VM běží také s touto závěrečnou verzí klienta a prošla kontrolou běhu po dobu 30 sekund.
- Automatické určení EFI ověřuje aktuální firmware, fyzický disk a oddíl, typ ESP, FAT a soubor zavaděče. Chybějící údaj Setupu nevadí; rozpor, neplatný údaj nebo odkaz na oddíl OS se odmítá. Osm kontrol těchto případů prošlo na skutečném UEFI LiberNT.
- Review zachytilo překrytí názvu fyzického disku místním aliasem. Inventář nyní otevírá disky v globálním prostoru názvů; neúplné nebo nejednoznačné čtení nevede k automatickému výběru.
- Dvě EFI kopie zavaděče a soubor na NTFS prošly společnou aktualizací a návratem přes nativní helper a čtyři skutečné restarty. Test změnil nepoužívaný DOS stub a přepočítal PE checksum, aby šlo prokázat změnu bajtů bez změny kódu zavaděče. Obě kopie měly před testem i po návratu SHA-256 `e7cb92765a55135b70f93b475f67d90d604e34980b31836f9c8574be8b60c8bc`, během aktualizace `4799fc3ba2ec262087182bef1ceb2dd4c319ac5c18bd074989427eb2ec8ff8c7`.
- Tento UEFI test používal OVMF, profil PC, disk MBR a vypnutý Secure Boot. Nativní helper měl SHA-256 `d4cf0ec21f4a3859bbbc887036bbc59a56b61476063d8669acf49a3d4b5cba48`. Původní boot příkazy jsou obnovené a dočasný systémový helper i testovací soubor odstraněné. Test neprokazuje GPT, fyzický počítač ani kompatibilitu jiné verze kódu zavaděče.
- Klient s novou detekcí EFI prošel na původní testovací instalaci všemi 108 kontrolami. SHA-256: `6dc9eeb5bc14450da278ec3be8c32041af6b921fdb3f12ff1eb60d331ca0ec58`.
- Přesný klon celého vypnutého testovacího disku byl před připojením bajtově porovnaný. Se dvěma shodnými ESP updater automatický výběr odmítl, přestože enumerace svazků ukázala jen první kopii. Po testu byl klon odpojený. Písmeno systémového disku je obnovené na původní C: a opět prošlo všech osm kontrol detekce. Pro další obnovovací testy má kopie vlastní identitu disku.
- `602c662`: ověření EFI je odeslané a [CI 42020229612](https://github.com/nks-hub/libernt-update/actions/runs/42020229612) prošlo.

## Nezávislá obnova

- CLI `/recover-offline` pracuje z jiného systému s výslovně zvoleným cílovým svazkem a transakcí; běžící OS odmítá a výpis nezapisuje do jeho datové složky.
- Pád boot ovladače: testovací transakce přidala ovladač, službu v SYSTEM a změnu hodnoty v SOFTWARE. Nový ovladač se Start=0 zavaděč při startu nenačetl, proto byl pád `0xE2` vyvolán ručním spuštěním služby. Systém mezitím transakci potvrdil; obnova proto musí umět vybrat i potvrzenou instalaci, když historie zůstala ve stavu čekání na restart.
- Obnova ze samostatného LiberNT na druhém disku odstranila ovladač i službu, vrátila hodnotu v SOFTWARE a nesouvisející kontrolní hodnotu ponechala. Cílový disk potom samostatně nabootoval s prázdnou čekající transakcí.
- Poškozený zavaděč: přímá transakce nahradila obě EFI kopie zavaděče 4 KiB neplatného obsahu. Firmware potom nenašel spustitelnou volbu a OS nenastartoval. Tento test odhalil, že obnova neuznávala dokončenou přímou instalaci; oprava přidala i regresní kontrolu.
- Opravený klient ze samostatného systému vrátil obě kopie na původní SHA-256 `e7cb92765a55135b70f93b475f67d90d604e34980b31836f9c8574be8b60c8bc`. BCD měl před obnovou i po ní stejný hash; updater ho nemění. Cílový systém potom normálně nabootoval s prázdnou čekající transakcí.
- Review po testu zachytilo tři další chyby: služba by po neúplné offline obnově každý den v nastavenou hodinu opakovaně restartovala, opakovaný `--list` mohl nabídnout starší instalaci a dvě odmítnutí neměla text chyby. Opravy mají vlastní regresní kontroly. Dokumentace nyní uvádí, že po zahájení offline obnovy se boot plány transakce už nespouštějí.
- Výsledný klient prošel na LiberNT 125 kontrolami a 9 HTTP kontrolami bez chyby. SHA-256: `0192e9a2ff8aed23fcb9105bd4ce68925c079d4e0b3fabfb746b7cb967ce5388`. Skutečnou obnovu zavaděče provedl předchozí kandidát `cabe35dcc27e868e181d6c2c03e49a4b74103aa47bf1ca7ed0b35c563f9def51`; výběr transakce se od té doby změnil jen u starších instalací.
- Čerstvá instalace LiberNT `5fccfb5` dává systémovým souborům vlastníka TrustedInstaller a SYSTEM i správcům jen čtení. Updater je proto nemohl nahradit; plán při startu skončil `STATUS_ACCESS_DENIED` a bezpečně se vrátil. Starší testovací instalace měla `Everyone:F`, proto se to dřív neprojevilo. `SeRestorePrivilege` v LiberNT právo DELETE nepřidělí. Updater nyní u odmítnutého souboru převezme vlastnictví, nahradí ho a novému souboru dá původního vlastníka i pravidla přístupu.
- Pád aktualizovaného boot ovladače: transakce nahradila chráněný `disk.sys` testovacím ovladačem. Nativní helper ho při startu vyměnil se zachováním práv; další start skončil pádem `0xE2` ve `Phase1Initialization`, ještě před SMSS. Obnova z druhého systému vrátila původní `disk.sys` včetně vlastníka a pravidel přístupu a cílový systém normálně nabootoval.
- Nově zaregistrovaný ovladač se Start=0 LiberNT při startu nespustí, ověřeno i neškodným ovladačem. Jde o chování OS; aktualizace existujícího boot ovladače se načte a byla otestována výše.
- Finální klient prošel úplnou aktualizací a rollbackem obou EFI zavaděčů a souboru na NTFS přes čtyři skutečné restarty. Na obou testovacích instalacích prošlo 127 kontrol a 9 HTTP kontrol bez chyby. SHA-256 klienta `52d88bf4e33d9edc39910f32e3b4bc03f8cf8b61d6cbb6fa3a8e0dbde580b68c`, nativního helperu `998fce5795a529e3e828e54db6c904d3b671d9678a100463d3a59f45c97bc4f6`.
- Testy proběhly ve VM s OVMF, diskem MBR a vypnutým Secure Boot. Fyzický UEFI počítač, GPT a Secure Boot ověřené nejsou. Testovací ovladač a poškozování zavaděče jsou jen v testovacím helperu, ne v běžném sestavení.

## Naměřený výkon registru

Testovací VM měla 2 CPU a 2 GiB RAM. Skutečný tovární export obsahoval 6 884 571 bajtů, 16 382 klíčů a 26 947 hodnot. Dvě měření dala 565–567 ms pro načtení a rozebrání JSON a 861–872 ms pro úplné porovnání stejných základů. Použitý testovací příkaz je `boot_vm.exe registry-snapshot <soubor>`.

Při shodných podepsaných hashech se nyní tato práce neprovádí. Uvedená čísla měří pouze zpracování registru; nezahrnují síť, porovnání systémových souborů ani celou instalaci.

## Stav vydání

- Registr i úprava ikony jsou implementované a otestované; změny zatím nejsou ve vydaném klientovi.
- Testovaný klient je nyní nainstalovaný na VM. Stará složka umožňovala zápis běžným uživatelům; je archivovaná a její plány se nepoužívají. Nová složka má chráněná oprávnění a vychází z ověřeného feedu. Veřejné vydání klienta ani zapnutí v2 feedu tím neproběhlo.

## Důležité zjištění z testů

Původní `BootExecute` běží před `NtInitializeRegistry`. SOFTWARE ještě není připojený a `CmFlushKey` může kvůli `CmpNoWrite` vrátit úspěch bez zápisu na disk. První skutečný test proto správně odmítl zápis a obnovil původní soubory. Pouhé dočasné připojení hive by příčinu nevyřešilo. Proto je nutná fáze `UpdateExecute` a kontrola její dostupnosti; marker je v paměťovém hive `HARDWARE`, aby nezůstal po návratu ke staršímu SMSS.

Cppcheck s cestami a přepínači skutečného sestavení nenašel problém v novém `update.c`. V `sminit.c:957` hlásí původní `uninitvar`, který je i v předchozí revizi; tento nesouvisející úsek se neměnil.

První UEFI test samotného továrního obrazu `5fccfb5` na Q35 skončil před updaterem pádem `0x7B / 0xC0000034`. Debugger potvrdil ARC cestu k druhému oddílu, načtený `storahci` i `disk.sys`, ale žádné vytvořené diskové zařízení. Profil Q35 obsahoval dva AHCI řadiče a disk byl za dalšími PCI mosty. Tentýž obraz přes OVMF na jednodušším profilu PC nabootoval až na plochu a spustil grafickou instalaci; to zatím neprokazuje aktualizaci EFI souborů. Příčina rozdílu Q35 není potvrzená a stav pádu včetně RAM je zachovaný pro další rozbor.

Kernel při každém startu zapisuje `FirmwareBootDevice` podle oddílu zavaděče a `SystemBootDevice` podle oddílu Windows. První údaj může pomoci s hledáním EFI oddílu, ale UEFI loader při neúspěšném hledání dosadí OS oddíl bez zvláštního příznaku. Proto samotné přečtení této hodnoty nestačí k bezpečné aktualizaci zavaděče. Je nutné ověřit oddíl i očekávaný loader; při rozporu nadále vyžadovat `bootRoot`.

## Zbývající práce

- Soukromý testovací v2 feed (16 vydání) prošel ověřením podpisu i všech objektů. Plán první migrace registru z továrního základu `9d41077` na `5fccfb5` byl na starší instalaci spočítaný jen pro čtení: 11 nových klíčů a hodnot v SOFTWARE (ovladač OpenGL a náhrady písem), jedna místní změna zachovaná, nic k odstranění. Stejný výsledek dala úplná kontrola klientem se samostatnou datovou složkou: ověřený podpis v2 feedu, manifesty a oba objekty registru, potvrzená podpora `UpdateExecute` v SMSS. Plán zatím nebyl aplikován a v2 feed není veřejný.
- Ověřit nezávislou obnovu na fyzickém počítači, s GPT a se Secure Boot; ve VM už prošla po pádu ovladače i po poškození zavaděče.
- V LiberNT zjistit, proč se nově zaregistrovaný ovladač se Start=0 při startu nespustí.
- Doplnit běhové ověření EFI na GPT a fyzickém počítači; automatické určení a skutečný boot po změně i návratu zavaděče na OVMF/MBR už prošly.

Nová klientská verze ani nový registry feed zatím nejsou publikované. Tento přehled není potvrzením, že všech pět omezení už zmizelo.
