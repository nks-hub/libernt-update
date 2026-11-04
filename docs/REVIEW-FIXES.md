# Opravy nálezů z review

Výchozí stav: commit `c6f6fed`. Opravy z 4. listopadu 2026. Tento přehled odděluje změnu kódu od ověření skutečným startem systému.

| Nález | Oprava |
|---|---|
| R01: předčasné potvrzení jádra | Záchrana zůstává po prvním bootu. Potvrzení vyžaduje další boot a jinou identitu běžícího jádra. |
| R02: souběh služby, GUI a CLI | Společný pojmenovaný mutex pokrývá kontrolu, zápisy stavu, instalaci i obnovu. |
| R03: přerušená instalace | Aktivní transakce se uloží před změnou souborů. Obnova zpracuje přímou instalaci i samostatnou značku přerušeného bootu. |
| R04: EFI na jiném svazku | Boot program kopíruje do dočasného souboru na cílovém svazku a teprve tam jej přejmenuje. |
| R05: nesprávně odhadnutý základ | Neznámá instalace vyžaduje ověřené `baseRelease`. Test přechodu R1 na R3 zahrnuje knihovnu přidanou v R2. |
| R06: ruční self-update bez ochrany | Společná instalační cesta připraví starého hlídače. Samostatná služba Guard chrání i aktualizaci provedenou při restartu. |
| R07: restart služby a rušení transakce | Restart služby ponechá plán. `/cancel` odmítne už zahájenou instalaci. Trvalá značka deaktivuje zrušený plán před uvolněním transakce. |
| R08: boot rollback přepisuje cizí změny | Každá nová operace ověřuje původ cílového obsahu. Konflikt ponechá soubor i zálohu pro další pokus. |
| R09: částečný návrat hlásí původní vydání | Konfliktní návrat má stav `partial-rollback` a nepotvrzuje původní vydání. |
| R10: neomezené HTTP odpovědi | Metadata mají limit 16 MiB, podpis 256 bajtů. Objekt musí mít podepsanou délku, nejvýše 512 MiB. Limity platí i pro místní feed. |
| R11: zaniklý řetězec parseru podpisu | Řetězec zůstává platný až do kontroly ukazatele vráceného ze `strtol`. |
| R12: jiné mapování offline EFI | Kontrola i instalace volají stejnou funkci pro mapování cíle. |
| R13: první nalezený EFI oddíl | Použije se oddíl určený Setupem nebo výslovně zadaný `bootRoot`. Nejednoznačný výsledek se odmítne. |
| R14: opakované publikování vynechá ověření | Ověření feedu stojí před každým přenosem, včetně větve `already published`. |
| R15: neověřená cache klienta | Kontrolní součty se ověřují při každém použití cache i před vložením klienta do obrazu. |
| R16: nečistý zdrojový strom | Sestavení odmítne rozpracované změny a znovu kontroluje strom i commit před publikováním. |
| R17: práva klíče až po zápisu | Soubor vznikne pomocí výhradního otevření s právy `0600`; existující klíč nelze přepsat. |

Další kontrola oprav doplnila trvalé potvrzení boot plánu, obnovu EXE odsunutého při self-update a uchování hlídače při neúspěšné obnově. Hlídač musí zaznamenat skutečný stav služby `RUNNING` a dalších 30 sekund jeho trvání.

Datová složka se kontroluje před důvěrou uloženým plánům. Neznámý vlastník, zápisové právo běžného uživatele nebo reparse bod způsobí odmítnutí. To samo o sobě není důkaz správných oprávnění na každé instalaci; ta je nutné ověřit na cílovém systému.

## Ověření

- Sestavení klienta a vestavěné regresní testy proběhly na Windows i na LiberNT. Sada má 58 kontrol, včetně úklidu starých záloh po potvrzení bootu. Kontrolní součet EXE na LiberNT odpovídal finálnímu místnímu sestavení: `e10e75b22398c106708d82027be7d0fb3048d68f0748dc0a87925de75e927e65`.
- Test vydavatele ověřuje i poškozenou cache a výhradní vytvoření klíče.
- Čtyři testy skutečného `build-release.sh` proběhly na Linuxu: ověření před přenosem, odmítnutí neplatného feedu, poškozené cache a nečistého stromu.
- [VM test](../client/tests/boot_vm.cpp) ověřil skutečnou výměnu testovacích souborů na systémovém NTFS i samostatném FAT svazku. První boot ponechal čekající transakci; druhý měl jinou identitu bootu a potvrdil dokončení.
- Další skutečný restart obnovil připravený přerušený stav `applying` + `plan.done` bez `trial`. Původní obsahy na obou svazcích byly ověřené a čekající transakce se vyčistila.
- Na LiberNT prošel test služby startující několik sekund ve `START_PENDING` i odmítnutí služby, která v tomto stavu zůstala. Použita byla samostatná [testovací služba](../client/tests/service_vm.cpp), nikoli změna produkčního klienta.
- Na testované instalaci kontrola skutečně zjistila nedůvěryhodné zápisové právo datové složky. Nový klient ji odmítl. Před jeho provozním nasazením je potřeba vyřešit práva a ověřit původ starých dat; automatické převzetí těchto plánů by nebylo bezpečné.
- Po testu byl obnoven původní `BootExecute` a odstraněn testovací boot program. Původní služba běžela a její kontrola hlásila nula změn.

Tyto testy nenahrazují start jiného upstream jádra, skutečný UEFI boot ani úplné vydání vadného klienta s následným samočinným návratem. Tato ověření zatím nejsou doložená. Test přerušeného bootu používá připravený stav transakce; nejde o měření chování disku při výpadku napájení.

Nové klientské vydání ani změna serverové služby nejsou součástí tohoto commitu. Oddělení sestavení od podpisového klíče vyžaduje samostatnou změnu provozního postupu. Review neprokázalo obejití ověření Ed25519; klient používá Monocypher k ověřování, produkční podpis vytváří PyNaCl.
