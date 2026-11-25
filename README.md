# GFN 3DS — GeForce NOW on a Nintendo 3DS, through a relay on your PC

> **Unofficial, experimental** project. Not affiliated with NVIDIA or Nintendo. "GeForce NOW" is an NVIDIA trademark.
> Français : [version française en bas de page](#version-française).

Your Windows PC runs the **official GeForce NOW client** (login, captcha, queue). A small **relay** on the PC
captures that window, streams it to the 3DS as 400×240 JPEG, and turns the 3DS buttons into a virtual Xbox 360
gamepad. On the 3DS you **pick the game** and see the **queue**.

```
 PC (Windows)                                               Nintendo 3DS
 GeForce NOW client ──capture──▶ relay ──── video (UDP) ───▶ top screen
        ▲ launches the game ◀──── relay ◀─── inputs (UDP) ── buttons / touch
                                  relay ◀──── control (TCP) ─ game list, queue, settings
```

This project does **not** bypass login, captcha, the queue or your subscription limits — the official client does all
that on your PC. Check that this use complies with NVIDIA's terms of service.

## Requirements

- Windows 10/11 PC with Python 3.10+ and the official GeForce NOW client (a GeForce NOW account).
- A hacked Nintendo 3DS / 2DS (Old or New) with the Homebrew Launcher, on the **same local network** as the PC
  (3DS Wi-Fi is 2.4 GHz only; some routers isolate Wi-Fi clients).

## Install

### On the PC

1. Install the **GeForce NOW client** and **log in once** (captcha etc. happen here, never on the 3DS).
   While playing, keep its window **visible, not minimised, not covered**, on your main screen, and disable sleep/lock.
2. [Create a shortcut for each game](#adding-your-games-gfnpc-shortcuts) you want on the 3DS.
3. Double-click **`relay\start_relay.bat`**. On first run it installs what it needs, including the **ViGEmBus**
   virtual-gamepad driver (via `vgamepad`): accept Windows' prompts.
   > ⚠️ ViGEmBus is **archived** (no more updates), and its updater contacted a domain its author no longer owns.
   > Read the [official end-of-life page](https://docs.nefarius.at/projects/ViGEm/End-of-Life/) first. To skip it,
   > delete the `vgamepad` line in `relay\requirements.txt`: video works, the gamepad doesn't.
4. Allow Python in the Windows firewall on **private** networks (TCP 47970, UDP 47971).
5. Optional: `start_relay.bat --check` tells you what is missing, which games were found, and saves
   `check_capture.jpg` — exactly what the 3DS will see.

The relay prints your PC's IP address at start-up; leave its window open while you play.

### On the 3DS

Download **`gfn3ds.3dsx`** (and `gfn3ds.smdh`, the icon) from the [Releases](../../releases) page, copy them to
`sd:/3ds/` and launch **GFN 3DS** from the Homebrew Launcher. (Or press **Y** in the Homebrew Launcher and run
`3dslink gfn3ds.3dsx` from the PC.)

### First run

On the 3DS (the interface is in French): **Réseau** (network) tab → pick your relay (or type the PC's IP) →
**Jeux** (games) tab → pick a game → `A`. If a PIN is set in `relay\config.json`, enter the same one in
*Réseau → Code PIN*.

Want to check Wi-Fi and decoding before using GeForce NOW? Put
`[ { "name": "Test", "type": "command", "command": ["notepad.exe"] } ]` in `relay\games.json`, start the relay with
`start_relay.bat --source testpattern`, and launch *Test*: you should see a test image (red top-left, green top-right,
blue bottom-left, yellow bottom-right). *Options → Infos à l'écran → Détaillées* shows fps, decode time and losses.

## Adding your games (.gfnpc shortcuts)

The 3DS lists the games for which you created a **GeForce NOW desktop shortcut** (a `.gfnpc` file).

1. Open the **GeForce NOW** app and log in.
2. Find the game, open its menu (the **⋯** button on its card or page, or right-click) and choose
   **"Create desktop shortcut"** (exact wording may vary by version). A `<Game name>.gfnpc` file appears on your Desktop.
3. Repeat for every game you want. Check it works: **double-click the `.gfnpc` on the PC** — it should launch the game
   in GeForce NOW. If it does, the 3DS can launch it too.
4. On the 3DS, in the *Jeux* tab, press **`X`** to refresh the list.

Good to know:
- The relay looks at `.gfnpc` files on your **Desktop** (OneDrive Desktop included) and at every shortcut in the Start
  menu folder `NVIDIA Corporation\Games`. Other Desktop shortcuts are ignored.
- You can also drop `.gfnpc` / `.lnk` / `.url` files in **`relay\games\`**, or list folders in `games.extra_dirs`
  (`relay\config.json`).
- Names are shown in plain ASCII (accents removed), without a trailing "(GeForce NOW)". Two shortcuts with the same
  name give one entry.
- If Windows hides file extensions, enable *View → File name extensions* in Explorer to see the `.gfnpc`.
- If nothing shows up, run `start_relay.bat --check`: it lists the games found.
- Why not my whole library automatically? That needs NVIDIA's private API and your session token, which this project
  doesn't touch.

## Using it

- **Tabs** (`L`/`R`): *Jeux* (games), *Réseau* (network), *Touches* (keys), *Options*. `A` launches, `X` refreshes.
  `Start` opens the menu (resume, stream options, key config, **stop stream — the game keeps running on the PC**, quit);
  `Select` opens stream options. Both are reserved for the app.
- **Controls:** Circle Pad = left stick, C-stick (New 3DS) = right stick, D-pad, ABXY, `L`/`R` = LB/RB,
  `ZL`/`ZR` (New 3DS) = LT/RT. On Old 3DS the touch screen provides LT/RT/L3/R3/Back/Guide/Start and a right stick.
  Three layouts plus per-key remapping in the *Touches* tab.
- **Profiles** (*Options*): max performance, balanced (default), low latency, max compatibility.
- **Queue:** by default the 3DS shows the **PC screen as is**, queue included. For a native queue screen (position,
  estimated wait) you must calibrate the relay once to your client's log format — see
  [docs/README-detailed.fr.md](docs/README-detailed.fr.md) (section 8, in French).

## Limitations

400×240 picture, modest JPEG quality (about 1–3 Mbit/s), added latency (not measured: not for competitive play),
**no audio**, no mouse/keyboard, one console at a time, local network only, **no encryption** (a PIN only prevents
accidents). Stopping the stream on the 3DS does **not** close the GeForce NOW session: close the game from the PC.

## Troubleshooting

| Problem | Try |
|---|---|
| 3DS can't see the relay | Same network? Firewall rule (private)? Router client isolation? Else type the PC's IP in *Réseau*. |
| Empty game list | Create the shortcuts (see above), press `X`; check `start_relay.bat --check`. |
| "PAS DE SIGNAL" on screen | GeForce NOW window not found or minimised: `start_relay.bat --list-windows`, then set `capture.window_title` in `relay\config.json`. |
| Black or wrong image | Open `check_capture.jpg`. Window covered? Screen asleep? Exclusive fullscreen (use windowed/borderless)? |
| Gamepad not working | `--check` (is ViGEmBus installed?). The game window needs focus. |
| Stutter, lost frames | Profile *Performance* or *Compatibilité*, buffer *Fluide*, move closer to the access point. |
| PIN error | `pin` in `relay\config.json` must equal *Réseau → Code PIN* on the 3DS. |

## Credits and licenses

Built with libctru (zlib; the font glyphs derive from it) and libjpeg-turbo (BSD/IJG/zlib); the relay uses `numpy`,
`opencv-python`, `dxcam`, `vgamepad` and the ViGEmBus driver, each under its own license. No NVIDIA logo or asset is used.
For developers (protocol, architecture, build, tests): [docs/README-detailed.fr.md](docs/README-detailed.fr.md).

---

# Version française

> Projet **non officiel et expérimental**, sans lien avec NVIDIA ni Nintendo.

Votre PC Windows fait tourner le **client GeForce NOW officiel** (connexion, captcha, file d'attente). Un **relais**
capture sa fenêtre, l'envoie à la 3DS en JPEG 400×240 et transforme les touches de la 3DS en manette Xbox 360
virtuelle. Sur la 3DS, vous **choisissez le jeu** et voyez la **file d'attente**. Le projet ne contourne ni connexion,
ni captcha, ni file, ni limites d'abonnement : vérifiez que cet usage respecte les conditions d'utilisation de NVIDIA.

## Prérequis

- PC Windows 10/11 avec Python 3.10+ et le client GeForce NOW officiel (compte GeForce NOW).
- Une 3DS / 2DS (Old ou New) avec le Homebrew Launcher, sur le **même réseau local** que le PC (Wi-Fi 2,4 GHz
  uniquement ; certaines box isolent les clients Wi-Fi).

## Installation

### Sur le PC

1. Installez le **client GeForce NOW** et **connectez-vous une fois** (le captcha se fait ici, jamais sur la 3DS).
   Pendant le jeu, gardez sa fenêtre **visible, non réduite, non recouverte**, sur l'écran principal, sans veille
   ni verrouillage.
2. [Créez un raccourci pour chaque jeu](#ajouter-vos-jeux-raccourcis-gfnpc) voulu sur la 3DS.
3. Double-cliquez **`relay\start_relay.bat`**. Au premier lancement, il installe le nécessaire, dont le pilote de
   manette virtuelle **ViGEmBus** (via `vgamepad`) : acceptez les demandes de Windows.
   > ⚠️ ViGEmBus est **archivé** (plus de mises à jour) et son outil de mise à jour contactait un domaine qui
   > n'appartient plus à l'auteur. Lisez d'abord la [page de fin de vie](https://docs.nefarius.at/projects/ViGEm/End-of-Life/).
   > Pour l'éviter, supprimez la ligne `vgamepad` de `relay\requirements.txt` : la vidéo marche, pas la manette.
4. Autorisez Python dans le pare-feu Windows sur les réseaux **privés** (TCP 47970, UDP 47971).
5. Facultatif : `start_relay.bat --check` indique ce qui manque, les jeux trouvés, et enregistre `check_capture.jpg`
   — exactement ce que verra la 3DS.

Le relais affiche l'adresse IP du PC au démarrage ; laissez sa fenêtre ouverte pendant que vous jouez.

### Sur la 3DS

Téléchargez **`gfn3ds.3dsx`** (et `gfn3ds.smdh`, l'icône) depuis la page [Releases](../../releases), copiez-les dans
`sd:/3ds/` et lancez **GFN 3DS** depuis le Homebrew Launcher. (Ou, dans le Homebrew Launcher, touche **Y**, puis
`3dslink gfn3ds.3dsx` depuis le PC.)

### Premier lancement

Sur la 3DS : onglet **Réseau** → choisissez votre relais (ou saisissez l'IP du PC) → onglet **Jeux** → choisissez un jeu
→ `A`. Si un code PIN est défini dans `relay\config.json`, saisissez le même dans *Réseau → Code PIN*.

Pour vérifier le Wi-Fi et le décodage avant d'utiliser GeForce NOW : mettez
`[ { "name": "Test", "type": "command", "command": ["notepad.exe"] } ]` dans `relay\games.json`, lancez le relais avec
`start_relay.bat --source testpattern` puis *Test* : une image de test doit s'afficher (rouge en haut à gauche, vert en
haut à droite, bleu en bas à gauche, jaune en bas à droite). *Options → Infos à l'écran → Détaillées* donne les
images/s, le temps de décodage et les pertes.

## Ajouter vos jeux (raccourcis .gfnpc)

La 3DS liste les jeux pour lesquels vous avez créé un **raccourci GeForce NOW sur le Bureau** (fichier `.gfnpc`).

1. Ouvrez l'application **GeForce NOW** et connectez-vous.
2. Trouvez le jeu, ouvrez son menu (bouton **⋯** sur sa vignette ou sa page, ou clic droit) et choisissez
   **« Créer un raccourci sur le Bureau »** (libellé exact variable selon la version). Un fichier `<Nom du jeu>.gfnpc`
   apparaît sur votre Bureau.
3. Répétez pour chaque jeu. Vérifiez : **double-cliquez le `.gfnpc` sur le PC** — il doit lancer le jeu dans
   GeForce NOW. Si c'est le cas, la 3DS le lancera aussi.
4. Sur la 3DS, onglet *Jeux* : appuyez sur **`X`** pour actualiser la liste.

À savoir :
- Le relais regarde les `.gfnpc` du **Bureau** (Bureau OneDrive compris) et tous les raccourcis du dossier du menu
  Démarrer `NVIDIA Corporation\Games`. Les autres raccourcis du Bureau sont ignorés.
- Vous pouvez aussi déposer des fichiers `.gfnpc` / `.lnk` / `.url` dans **`relay\games\`**, ou lister des dossiers
  dans `games.extra_dirs` (`relay\config.json`).
- Les noms sont affichés en ASCII (accents retirés), sans « (GeForce NOW) » final. Deux raccourcis de même nom ne
  donnent qu'une entrée.
- Si Windows masque les extensions, activez *Affichage → Extensions de noms de fichiers* dans l'Explorateur.
- Si rien n'apparaît : `start_relay.bat --check` liste les jeux trouvés.
- Pourquoi pas toute ma bibliothèque automatiquement ? Cela demande l'API privée de NVIDIA et le jeton de votre
  session, que ce projet ne touche pas.

## Utilisation

- **Onglets** (`L`/`R`) : *Jeux*, *Réseau*, *Touches*, *Options*. `A` lance, `X` actualise. `Start` ouvre le menu
  (reprendre, options, touches, **arrêter le flux — le jeu continue sur le PC**, quitter) ; `Select` ouvre les options
  de streaming. Ces deux touches sont réservées à l'appli.
- **Commandes :** Circle Pad = stick gauche, C-stick (New 3DS) = stick droit, croix, ABXY, `L`/`R` = LB/RB,
  `ZL`/`ZR` (New 3DS) = LT/RT. Sur Old 3DS, l'écran tactile donne LT/RT/L3/R3/Back/Guide/Start et un stick droit.
  Trois dispositions et remappage touche par touche (onglet *Touches*).
- **Profils** (*Options*) : performance max, équilibré (défaut), faible latence, compatibilité max.
- **File d'attente :** par défaut, la 3DS montre **l'écran du PC tel quel**, file comprise. Pour un écran natif
  (position, attente estimée), il faut calibrer une fois le relais sur le format des journaux de votre client :
  voir [docs/README-detailed.fr.md](docs/README-detailed.fr.md) (section 8).

## Limites

Image 400×240, qualité JPEG modeste (environ 1–3 Mbit/s), latence ajoutée (non mesurée : pas pour du compétitif),
**pas d'audio**, pas de souris/clavier, une seule console à la fois, réseau local uniquement, **aucun chiffrement**
(le PIN ne sert qu'à éviter les accidents). Arrêter le flux sur la 3DS ne ferme **pas** la session GeForce NOW :
fermez le jeu depuis le PC.

## Dépannage

| Problème | Piste |
|---|---|
| La 3DS ne voit pas le relais | Même réseau ? Règle de pare-feu (privé) ? Isolation des clients de la box ? Sinon saisissez l'IP du PC dans *Réseau*. |
| Liste de jeux vide | Créez les raccourcis (voir plus haut), appuyez sur `X` ; voir `start_relay.bat --check`. |
| « PAS DE SIGNAL » | Fenêtre GeForce NOW introuvable ou réduite : `start_relay.bat --list-windows`, puis `capture.window_title` dans `relay\config.json`. |
| Image noire ou incorrecte | Ouvrez `check_capture.jpg`. Fenêtre recouverte ? Écran en veille ? Plein écran exclusif (passez en fenêtré/sans bordure) ? |
| Manette inactive | `--check` (ViGEmBus installé ?). La fenêtre du jeu doit avoir le focus. |
| Saccades, images perdues | Profil *Performance* ou *Compatibilité*, tampon *Fluide*, rapprochez-vous de la borne. |
| Erreur de PIN | Le `pin` de `relay\config.json` doit égaler *Réseau → Code PIN* sur la 3DS. |

## Crédits et licences

Construit avec libctru (zlib ; les glyphes de la police en dérivent) et libjpeg-turbo (BSD/IJG/zlib) ; le relais utilise
`numpy`, `opencv-python`, `dxcam`, `vgamepad` et le pilote ViGEmBus, chacun sous sa licence. Aucun logo ni élément
graphique de NVIDIA n'est utilisé. Pour les développeurs (protocole, architecture, compilation, tests) :
[docs/README-detailed.fr.md](docs/README-detailed.fr.md).
