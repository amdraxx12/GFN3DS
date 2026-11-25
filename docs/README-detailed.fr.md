> Document détaillé (ancienne version longue du README, en français). Le README principal est à la racine.

# GFN 3DS — jouer à GeForce NOW sur Nintendo 3DS, via un relais sur votre PC

> Projet **non officiel**. Aucun lien avec NVIDIA ni Nintendo. « GeForce NOW » est une marque de NVIDIA.

Ce dépôt contient deux programmes qui travaillent ensemble :

| Dossier | Programme | Rôle |
|---|---|---|
| `relay/` | **Relais PC** (Python, Windows) | Lance, sur ordre de la 3DS, le jeu choisi dans le **client GeForce NOW officiel** installé sur votre PC (c'est ce client qui gère la connexion, le captcha, la file d'attente…), capture sa fenêtre et l'envoie à la console en JPEG, transforme les touches de la 3DS en manette Xbox virtuelle et, si vous l'avez calibré, lit la position dans la file d'attente dans les journaux du client. |
| `client3ds/` | **Client 3DS** (C/C++, devkitARM + libctru) | Application homebrew `.3dsx` : liste des jeux, file d'attente, vidéo sur l'écran du haut, manette sur l'écran du bas. Old 3DS / 3DS XL / 2DS et New 3DS / 3DS XL / 2DS XL. |

```
┌──────────────────────────────────────────────────────────────┐
│ PC Windows                                                   │
│                                                              │
│  Client GeForce NOW officiel (connexion, captcha, file, jeu) │
│        ▲ lance le jeu        │ image de la fenêtre           │
│        │ (raccourci)         ▼ (capture)                     │
│  Relais GFN 3DS (Python) : JPEG 400x240, manette Xbox 360    │
│  virtuelle (ViGEmBus), lecture des journaux (facultatif)     │
└─────────┬───────────────────┬───────────────────┬────────────┘
          │ vidéo             │ touches           │ commandes,
          │ JPEG              │ + tactile         │ liste, état
          │ UDP 47972         │ UDP 47971         │ TCP 47970
          ▼                   ▲                   ▲▼
┌─────────┴───────────────────┴───────────────────┴────────────┐
│ Console 3DS : client GFN 3DS (.3dsx)                         │
│  liste des jeux · file d'attente · vidéo 400x240 (haut)      │
│  manette physique + tactile (bas)                            │
└──────────────────────────────────────────────────────────────┘
              réseau local, Wi-Fi 2,4 GHz côté console
```

---

## 1. Pourquoi un relais ? (verdict honnête)

Faire tourner GeForce NOW **directement** sur une 3DS n'est pas réaliste :

- le service est propriétaire et son protocole n'est pas documenté ; la connexion passe par un site web (captcha, double authentification) que ce projet ne cherche pas à reproduire ;
- le flux officiel est conçu pour des décodeurs et des débits très au-delà de ce que sait faire une 3DS (processeur ARM11 à 268 MHz sur une Old 3DS, Wi-Fi 2,4 GHz) ;
- rien ne permet d'y accéder sans le client officiel.

La seule architecture honnête est donc celle-ci : **le PC fait le travail lourd** (client officiel, décodage, captcha), **la 3DS n'est qu'un écran et une manette** qui reçoit une vidéo légère (JPEG 400×240). Il vous faut un PC Windows allumé, connecté à GeForce NOW, sur le même réseau local que la console.

**Ce que le projet ne fait pas** : il ne se connecte pas à GeForce NOW à votre place, ne contourne ni le captcha, ni l'authentification, ni la file d'attente, ni les limites de votre abonnement. Le client officiel fait tout cela sur votre PC. Vérifiez que cet usage respecte les conditions d'utilisation de NVIDIA et de votre abonnement.

---

## 2. Ce qui est fourni

| Demande | État |
|---|---|
| Choisir le jeu **depuis la 3DS** | ✅ liste des jeux dont vous avez créé le raccourci dans le client GeForce NOW (détection automatique, non vérifiée sous Windows) ou déclarés à la main ; lancement depuis la console |
| Voir la **file d'attente** sur la 3DS | ✅ écran dédié (position, attente estimée, temps écoulé) **si le relais est calibré** (§ 8). Sinon, l'écran du PC s'affiche tel quel, file comprise |
| Connexion / captcha | ✅ gérés **sur le PC** par le client officiel (la 3DS peut les *montrer*, mais ne peut pas saisir un captcha) |
| Vidéo écran du haut 400×240, ~30 i/s visés | ✅ JPEG (libjpeg-turbo), tampon de gigue, abandon d'images plutôt que retard |
| Manette complète | ✅ Circle Pad, croix, ABXY, L/R, ZL/ZR et C-stick (New 3DS), boutons et stick droit **tactiles** pour tout le reste |
| 4 profils (performance, équilibré, faible latence, compatibilité) | ✅ définis côté relais (cadence, qualité JPEG, taille cible) ; choix depuis la console |
| Réseau : pertes, reconnexion automatique, latence, tampon adaptatif, thread principal jamais bloqué | ✅ |
| Configuration des touches | ✅ 3 dispositions prédéfinies + remplacement touche par touche + test des touches |
| Interface « façon GeForce NOW » | ✅ interface sombre à accent vert, inspirée de GeForce NOW (aucun logo ni élément graphique de NVIDIA) |
| `.3dsx` | ✅ code source complet, `make` |
| `.cia` | ⚠️ cible `make cia` fournie, **jamais testée** |
| Son | ❌ pas d'audio (voir § 11) |
| Souris / clavier | ❌ manette uniquement |

---

## 3. Ce qui a été vérifié — et ce qui ne l'a **pas** été

J'ai développé et testé tout ce qui pouvait l'être dans un environnement Linux, sans console ni Windows. Voici la liste exacte.

**Vérifié automatiquement (tests fournis, voir § 12) :**

- **Relais** : 63 tests (protocole, catalogue de jeux et détection automatique, file d'attente par expressions régulières, profils, code PIN, reprise de session, coupures, anti-blocage des entrées, état « non suivi »…).
- **Client 3DS, le vrai code C++** compilé sur PC (la couche « plate-forme » a une branche PC) :
  - 34 tests unitaires (≈ 2 000 vérifications, avec AddressSanitizer/UBSan) : réassemblage des fragments vidéo, tampon de gigue, réglages, correspondance des touches, zones tactiles, format des paquets ;
  - **9 tests de bout en bout : le vrai client C++ contre le vrai relais Python** — connexion, liste des jeux, changement de profil, mauvais PIN, lancement, file d'attente (position/estimation), chargement, vidéo (orientation et couleurs vérifiées), manette, coupure Wi-Fi puis reprise automatique de la vidéo, arrêt/reprise du flux, installation « non calibrée » ;
  - 10 tests qui garantissent que `protocol.hpp` (3DS) et `protocol.py` (PC) décrivent exactement le même protocole (constantes, états, octets).
- Les fichiers qui appellent libctru passent un **contrôle de syntaxe** contre les vrais en-têtes de libctru (`client3ds/tools/check_3ds_syntax.sh`) : ce n'est pas une compilation ARM.
- Pour vérifier que les tests servent à quelque chose, j'ai cassé volontairement le code à plusieurs endroits : ils échouent bien.

**NON vérifié (à faire par vous, dans cet ordre — voir § 6) :**

1. **La compilation ARM avec devkitARM** : le Makefile est le modèle officiel adapté, mais `make` n'a **jamais été exécuté** (pas de chaîne devkitPro ici). Une correction mineure peut être nécessaire. Le nom du paquet `3ds-libjpeg-turbo` n'a pas pu être vérifié non plus.
2. **L'exécution sur une console** : performances réelles (vitesse de décodage JPEG sur Old 3DS, mémoire), clavier logiciel, réception des balises de découverte, comportement du Wi-Fi.
3. **Tout le côté Windows** : capture avec `dxcam`, manette virtuelle `vgamepad`/ViGEmBus, détection de la fenêtre GeForce NOW, lancement par raccourci, lecture des journaux. Ces éléments ont été testés uniquement avec des doublons logiciels sous Linux, **pas** avec Windows ni avec le vrai GeForce NOW.
4. **Le format des journaux de GeForce NOW** (file d'attente) : NVIDIA ne le documente pas ; il faut le calibrer une fois (§ 8).
5. `make cia` (`tools/gfn3ds.rsf`), l'icône et la bannière (générées automatiquement, simples).

Les chiffres de débit du § 9 sont des **calculs**, pas des mesures. La latence de bout en bout n'a pas été mesurée.

---

## 4. Installation côté PC (Windows 10/11)

1. **Python 3.10 ou plus récent** (python.org, avec le lanceur `py`). Le relais n'a été exécuté que sous Linux avec Python 3.13.
2. **Client GeForce NOW officiel** : installez-le et **connectez-vous une fois sur le PC** (captcha, double authentification…). Le relais capture ce que l'écran affiche à l'emplacement de la fenêtre : elle doit rester **visible, non réduite et non recouverte** (mode fenêtré ou plein écran sans bordure), sur l'écran principal. Pendant une partie, le PC ne doit ni se mettre en veille ni verrouiller la session.
3. **Relais** : dans le dossier `relay/`, double-cliquez `start_relay.bat`. Au premier lancement, il crée un environnement `.venv` et installe `numpy`, `opencv-python`, `dxcam` et `vgamepad`. `vgamepad` automatise l'installation du pilote **ViGEmBus** (il fournit la manette Xbox 360 virtuelle) : acceptez la demande de Windows si elle apparaît.

   > ⚠️ **ViGEmBus est un projet archivé** : il ne reçoit plus aucune mise à jour (voir la [page de fin de vie](https://docs.nefarius.at/projects/ViGEm/End-of-Life/)). D'après cette page, les installations existantes continuent de fonctionner, mais son outil de mise à jour contactait un domaine qui n'appartient plus à l'auteur (risque d'exposer votre adresse IP publique) ; la page explique comment corriger ou retirer cet outil. **Lisez-la avant d'installer.** Pour ne pas l'installer, supprimez la ligne `vgamepad` de `relay/requirements.txt` avant le premier lancement : la vidéo fonctionne sans, mais la console n'aura plus de manette (la 3DS l'indique).
4. **Pare-feu Windows** : à la première exécution, autorisez Python sur les réseaux **privés** (entrées TCP 47970 et UDP 47971). Le PC et la console doivent être sur le même réseau local ; certaines box ont une « isolation des clients » qui empêche la console de voir le PC.
5. **Diagnostic** : `start_relay.bat --check` vérifie les modules, le dossier des journaux, la fenêtre GeForce NOW, la capture et la manette virtuelle, et écrit `check_capture.jpg` (dans `relay/`) : **c'est l'image exacte que verra la 3DS**. `--list-windows` liste les fenêtres visibles si celle de GeForce NOW n'est pas trouvée. `--no-pad` lance le relais sans manette virtuelle.
6. **Vos jeux apparaissent tout seuls** : dans le client GeForce NOW, ouvrez la fiche d'un jeu et utilisez **« Créer un raccourci »** (Bureau ; fichier `.gfnpc`). Le relais cherche automatiquement :
   - le **client GeForce NOW** (dossiers usuels, chemins non vérifiés) : s'il le trouve, une entrée « [ Ouvrir GeForce NOW ] » apparaît en tête de liste de la 3DS ; sinon le relais affiche le lien de téléchargement (`--check` aussi) ;
   - les raccourcis `.gfnpc` du **Bureau** (y compris OneDrive) et tous les raccourcis du dossier du menu Démarrer `NVIDIA Corporation\Games` ;
   - les raccourcis `.lnk` / `.url` / `.gfnpc` déposés dans `relay/games/` ou dans les dossiers listés dans `games.extra_dirs` de `config.json`.

   La liste est relue à chaque appui sur **Actualiser** (`X`) sur la 3DS : créez un raccourci, puis actualisez. `start_relay.bat --check` affiche ce qui a été trouvé. L'option `games.auto_scan` (`false`) désactive la recherche automatique. Ces emplacements et le format `.gfnpc` viennent de descriptions de projets tiers : **je n'ai pas pu les vérifier sur Windows**. Si rien n'apparaît, déposez les raccourcis dans `relay/games/`.

   Autres entrées possibles dans `relay/games.json` (modèle : `games.example.json`) : `shortcut` (chemin d'un raccourci), `command` (programme et arguments, jamais passés à un shell) et `url_route` (`GeForceNOW.exe --url-route=…` : format **non vérifié sous Windows**).

   Les noms sont convertis en ASCII (accents retirés, caractères non latins supprimés), limités à 38 caractères, et le suffixe « (GeForce NOW) » est retiré. Deux raccourcis de même nom ne donnent qu'un jeu.

   **Pourquoi pas toute votre bibliothèque d'un coup ?** La liste complète de vos jeux n'est accessible que par l'API privée de NVIDIA, avec le jeton de votre session (certains outils tiers le lisent dans un fichier local). Ce n'est ni documenté ni vérifiable ici, et cela revient à utiliser votre compte hors du client officiel : je ne l'ai donc pas fait. Un client « maison » complet est ce que fait le projet tiers [OpenNOW](https://github.com/OpenCloudGaming/OpenNOW) (MIT, Windows, ne se présente pas comme officiel) ; il n'offre ni ligne de commande ni API pour choisir un jeu depuis l'extérieur, et sa conformité aux conditions de NVIDIA n'est pas établie. Si vous préférez l'utiliser à la place du client officiel, réglez `capture.window_title` sur le titre de sa fenêtre : la capture et la manette fonctionneront, mais le choix du jeu se fera sur le PC.
7. `config.json` est créé au premier lancement, à côté de `start_relay.bat` : nom affiché, ports, **code PIN** facultatif, source de capture, profils, zones mortes et correspondance de la manette… Chaque option est commentée dans `relay/gfn3ds_relay/config.py`.

Au démarrage, le relais affiche ses adresses IP, ses ports, l'état de la file d'attente (« suivie » ou « non calibrée ») et le nombre de jeux.

> **Sécurité** : le relais est prévu pour un **réseau local de confiance**. Il refuse par défaut les clients dont l'adresse n'est pas privée (`lan_only`). Le code PIN évite les connexions accidentelles, mais **rien n'est chiffré**, PIN compris : n'exposez jamais ces ports sur Internet.

---

## 5. Installation côté 3DS

**Compiler** (devkitPro installé et à jour ; sous Windows, depuis le shell MSYS2 de devkitPro, où la commande est `pacman` — `dkp-pacman` sous Linux/macOS) :

```
pacman -S 3ds-dev 3ds-libjpeg-turbo     # nom du paquet non vérifié ici : `pacman -Ss jpeg` pour le retrouver
cd client3ds
make                                    # -> gfn3ds.3dsx (+ gfn3ds.smdh)
```
Le client utilise **libjpeg-turbo** (décodage JPEG, lien `-ljpeg`). Si votre installation devkitPro n'en fournit pas, compilez-la avec `$DEVKITPRO/cmake/3DS.cmake` et placez-la dans vos portlibs. En cas d'erreur de compilation, c'est le point le plus probable : le code n'a jamais été compilé avec devkitARM (§ 3).

**Installer** : copiez `gfn3ds.3dsx` dans `sdmc:/3ds/` (ou `sdmc:/3ds/gfn3ds/`) sur la carte SD et lancez-le depuis le Homebrew Launcher. Les réglages sont enregistrés dans `sdmc:/3ds/gfn3ds/settings.ini`.

**CIA (facultatif, non testé)** : `make cia` demande `bannertool` et `makerom` dans le PATH ; `tools/make_assets.py` régénère icône, bannière et son muet.

---

## 6. Première mise en route (valider la chaîne en 3 étapes)

Chaque étape isole un risque différent ; ne passez à la suivante que si la précédente marche.

**Étape A — la 3DS et le Wi-Fi, sans GeForce NOW.** Dans `relay/games.json`, déclarez un « jeu » de test :

```json
[ { "name": "Test (Bloc-notes)", "type": "command", "command": ["notepad.exe"] } ]
```

puis lancez le relais avec une image de test à la place de la capture (dans `relay/`) :

```
start_relay.bat --source testpattern
```

Sur la 3DS : onglet **Réseau**, choisissez le relais (ou saisissez son adresse), puis **Jeux** → « Test (Bloc-notes) ». L'écran du haut doit montrer l'image de test (coins **rouge en haut à gauche, vert en haut à droite, bleu en bas à gauche, jaune en bas à droite**, avec un compteur). Dans **Options → Infos à l'écran → Détaillées**, vous lisez les images/s décodées, le temps de décodage en ms, le débit et les pertes : c'est là que vous saurez si votre console tient la cadence.

**Étape B — la charge soutenue.** `start_relay.bat --source video:C:\chemin\video.mp4` envoie un fichier vidéo (du mouvement, donc des images plus lourdes qu'une image de test).

**Étape C — GeForce NOW pour de vrai** (§ 4, 5 et 8). Lancez d'abord `start_relay.bat --check` et ouvrez `check_capture.jpg`.

---

## 7. Utilisation sur la console

**Écran du bas — 4 onglets** (changer avec `L`/`R` ou en touchant l'onglet) :

| Onglet | Contenu |
|---|---|
| **Jeux** | Panneau « Session GeForce NOW » (état, file d'attente, **Voir l'écran du PC** / **Reprendre le flux**), puis la liste des jeux. `A` lance (au toucher : un appui choisit, le second valide) ; `X` actualise la liste. |
| **Réseau** | Relais détectés automatiquement (balises UDP 47973) → `A` pour choisir ; ou saisie à la main de l'adresse, du port et du code PIN (clavier logiciel) ; reconnexion automatique ; connexion au démarrage ; état de la liaison (latence, relais). |
| **Touches** | Disposition (Standard / Position (B=A, Y=X) / Gâchettes sur L/R), remplacement touche par touche, stick droit tactile, test des touches. |
| **Options** | Profil de streaming, tampon vidéo (Adaptatif / Latence min. / Fluide), infos à l'écran (Aucune / Compactes / Détaillées), « Mode rapide (804 MHz) » (New 3DS uniquement). |

Dans ces menus, `Start` demande à quitter l'application (avec confirmation).

**Écran du haut** : état de la connexion, puis — selon la situation — « Lancement », **file d'attente** (grande position, attente estimée, temps passé dans la file), « Chargement », ou la **vidéo** avec les informations de streaming. Des bandeaux signalent « Connexion perdue » (avec les tentatives de reconnexion), « En attente de la première image » ou « Image figée depuis N s ».

**Pendant la partie** : l'écran du bas devient la manette tactile (LB, LT, RT, RB ; L3, Back, Guide, Start, R3 ; stick droit). `Start` ouvre le menu (Reprendre · Options de streaming · Configuration des touches · **Arrêter le flux (le jeu continue)** · Quitter l'application), `Select` ouvre les options de streaming. Ces deux touches sont **réservées à l'interface** et ne sont jamais envoyées au jeu (le Start et le Back de la manette Xbox passent par les boutons tactiles).

**Correspondance par défaut (disposition « Standard »)**

| 3DS | Manette Xbox | Remarque |
|---|---|---|
| Circle Pad | Stick gauche | |
| C-stick | Stick droit | New 3DS ; sur Old 3DS, utilisez le stick tactile |
| A / B / X / Y | A / B / X / Y | « Position » : B↔A et Y↔X, selon la place physique des boutons |
| L / R | LB / RB | « Gâchettes sur L/R » : L/R → LT/RT, et ZL/ZR → LB/RB |
| ZL / ZR | LT / RT | New 3DS ; sur Old 3DS : boutons tactiles LT/RT |
| Croix directionnelle | Croix | |
| — | L3, R3, Back, Guide, Start | boutons tactiles |

**Si vous quittez le flux, le jeu continue de tourner sur le PC** : « Arrêter le flux » ne ferme **pas** la session GeForce NOW (la console n'envoie aucune commande de ce type). Fermez le jeu depuis le client GeForce NOW. « Reprendre le flux » vous y ramène.

---

## 8. La file d'attente : ce qui marche tout de suite, et ce qu'il faut calibrer

NVIDIA ne documente ni API ni format de journal pour la file d'attente. Le relais ne **devine** donc rien :

- **Sans calibration (état à l'installation)** — le relais annonce l'état « non suivi » : la 3DS affiche directement **l'écran du client GeForce NOW du PC**. La file d'attente est visible dans l'image, comme sur le PC, mais sans écran dédié (ni position ni estimation sur la 3DS) et sans détection automatique du début ou de la fin de la partie. Un message le rappelle au lancement.
- **Avec calibration** — la 3DS affiche l'écran « File d'attente » natif (position, attente estimée, temps passé), puis « Chargement », puis bascule seule sur la vidéo quand le jeu démarre ; elle revient à la liste à la fin de la session.

**Calibrer (une fois, idéalement à une heure où il y a de la file) :**

1. Sur le PC, dans `relay/` : `.venv\Scripts\python.exe -m gfn3ds_relay.probe_queue` (option `--dir` pour un autre dossier de journaux que `%LOCALAPPDATA%\NVIDIA Corporation\GeForceNOW`). L'outil écrit dans `probe_<date>.txt` **toutes** les nouvelles lignes des journaux et affiche à l'écran celles qui contiennent des mots-clés (queue, position, wait, eta, session, stream…).
2. Lancez un jeu dans GeForce NOW ; attendez dans la file jusqu'au début de la partie ; puis `Ctrl+C`.
3. Dans le fichier, repérez les lignes qui contiennent la position, l'attente estimée, le chargement, le démarrage, la fin de session et les erreurs. **Relisez-le avant de le partager** : des journaux peuvent contenir des identifiants.
4. Renseignez dans `config.json`, section `"gfn"`, des **expressions régulières** Python (insensibles à la casse) : `queue_regex` (avec un groupe nommé, par ex. `(?P<pos>\d+)`), `eta_regex` (`(?P<eta>\d+)`) et `eta_unit` (`min` ou `s`), `loading_regex`, `streaming_regex` (la partie démarre), `ended_regex` (retour à l'état libre) et `error_regex`.
5. Relancez le relais : la bannière indique que la file est suivie via les journaux.

Limites à connaître : le format peut changer à chaque mise à jour du client ; si c'est le cas, la file n'est plus lue (videz les expressions pour revenir au mode « non suivi », qui ne dépend d'aucun format). Sans `streaming_regex`, la 3DS ne passe jamais seule sur la vidéo ; sans `ended_regex`, elle ne sait pas que la session est finie : utilisez alors **Voir l'écran du PC** et **Arrêter le flux**.

---

## 9. Profils et réglages

| Profil (relais) | Cadence | Qualité JPEG | Taille cible / image | Débit estimé* |
|---|---|---|---|---|
| Performance maximale | 30 | 30–60 | 8 Ko | ≈ 1,9 Mbit/s |
| Qualité équilibrée (défaut) | 30 | 40–75 | 11 Ko | ≈ 2,6 Mbit/s |
| Faible latence | 40 | 30–55 | 7 Ko | ≈ 2,2 Mbit/s |
| Compatibilité max | 20 | 30–55 | 6 Ko | ≈ 1,0 Mbit/s |

\* calcul `cadence × taille × 8`, pas une mesure. Valeurs modifiables dans `config.json` (section `profiles`).

**Comment le relais s'adapte.** Il règle la qualité JPEG, dans l'intervalle du profil, pour garder la taille des images près de la cible. La console envoie un rapport de réception chaque seconde : si plus de 8 % des images sont perdues, le relais réduit la taille cible et la cadence (au plus bas : 40 % de la taille cible et 70 % de la cadence du profil), puis remonte progressivement quand les rapports redeviennent bons. Les images identiques à la précédente ne sont pas renvoyées (une image de maintien par seconde).

**Tampon vidéo** : *Adaptatif* (marge selon la gigue mesurée, plafonnée à 50 ms), *Latence min.* (aucune attente : affiche dès que prêt), *Fluide* (marge fixe de 25 ms). **« Latence »** affichée = aller-retour console ↔ relais, **pas** la latence totale jusqu'à l'image (capture + encodage + Wi-Fi + décodage + latence de GeForce NOW elle-même).

---

## 10. Dépannage

| Symptôme | Pistes |
|---|---|
| La 3DS ne voit pas le relais | Même réseau ? « Isolation des clients » désactivée sur la box ? Pare-feu autorisé (réseau privé) ? Sinon saisissez l'adresse IP affichée par le relais dans **Réseau → Adresse**. |
| Erreur de code PIN | Le PIN de `config.json` doit être identique à celui de **Réseau → Code PIN**. Cette erreur est définitive : la console n'insiste pas tant que vous ne l'avez pas corrigée. |
| Liste de jeux vide | Créez un raccourci de jeu dans le client GeForce NOW (§ 4, point 6), puis **Actualiser** sur la 3DS. `start_relay.bat --check` liste les jeux trouvés. Sinon, déposez des raccourcis dans `relay/games/`. |
| L'écran affiche « PAS DE SIGNAL » | Le relais ne trouve pas la fenêtre GeForce NOW : `--list-windows`, fenêtre non réduite et sur l'écran principal, puis `capture.window_title` / `window_process` / `monitor` / `region` dans `config.json`. |
| Image noire ou incorrecte | Ouvrez `check_capture.jpg` (`--check`). La fenêtre est-elle recouverte ? L'écran du PC est-il en veille ? Plein écran exclusif : passez en fenêtré ou sans bordure. |
| La manette ne réagit pas | `--check` (ViGEmBus ?). Le client GeForce NOW doit voir une manette Xbox 360 (Windows : « Configurer les contrôleurs de jeu USB »). La fenêtre du jeu doit avoir le focus. |
| Saccades, images perdues | Profil Performance ou Compatibilité ; tampon *Fluide* ; rapprochez-vous de la borne ; évitez les interférences 2,4 GHz. Sur Old 3DS, le temps de décodage est visible dans les infos « Détaillées ». |
| La liaison tombe | La console se reconnecte seule (0,5 s, 1 s, 2 s puis 4 s) et redemande le flux ; la session de jeu, elle, continue sur le PC. |
| « Réseau indisponible » | Le Wi-Fi de la console est coupé (interrupteur). |

---

## 11. Limites matérielles et fonctionnelles (sans fard)

- **400×240 pixels**, pas de 3D stéréoscopique : le texte des jeux sera petit. JPEG à ~1–3 Mbit/s : qualité modeste, surtout en mouvement.
- **Latence** : elle s'additionne (GeForce NOW + capture + JPEG + Wi-Fi 2,4 GHz de la 3DS + décodage). Je ne l'ai pas mesurée ; **ne comptez pas sur ce montage pour du compétitif**.
- **Old 3DS** : décodage JPEG logiciel sur un ARM11 à 268 MHz — la cadence de 30 i/s n'est **pas garantie** ; le client abandonne des images plutôt que d'accumuler du retard. **New 3DS** : « Mode rapide (804 MHz) », et le décodage est placé sur un cœur supplémentaire quand le système l'autorise (sinon sur le cœur de l'application). Aucun de ces gains n'a été mesuré.
- **Pas d'audio.** Il faudrait capturer le son du PC (WASAPI loopback), l'encoder et le jouer via `ndsp`, ce qui exige le fichier `dspfirm.cdc` extrait de votre console ; `source/audio.cpp` est volontairement un module vide qui l'annonce franchement.
- **Manette uniquement** : pas de souris ni de clavier ; les jeux qui en exigent, et les écrans de connexion/captcha, se font sur le PC.
- **Une seule console à la fois** ; réseau local uniquement, sans chiffrement.
- Le jeu **n'est pas arrêté** quand vous arrêtez le flux.

---

## 12. Tests

```
# Relais + synchronisation du protocole (+ client réel contre relais réel si le binaire existe)
cd relay
python -m unittest discover -s tests -t .

# Client C++ sur PC (g++ et libjpeg-turbo pour PC requis)
cd client3ds
tests/host/run_unit_tests.sh          # 34 tests unitaires, AddressSanitizer + UBSan si disponibles
tests/host/build.sh                   # produit tests/host/out/headless (le vrai client, sans écran)
cd ../relay && python -m unittest discover -s tests -t . -p "test_client_e2e.py" -v

# Contrôle de syntaxe contre les en-têtes de libctru (sans devkitARM ; ce n'est PAS une compilation ARM)
client3ds/tools/check_3ds_syntax.sh /chemin/vers/libctru/include
```
Sous Windows : WSL ou MSYS2. Variables : `GFN3DS_HEADLESS` (chemin du binaire `headless`), `GFN3DS_SHOTS` (dossier où le test écrit des captures de l'interface en `.ppm`), `JPEG_CFLAGS` / `JPEG_LIBS` si libjpeg n'est pas trouvé par `pkg-config`, `JPEG_INC` pour le contrôle de syntaxe.

---

## 13. Architecture

```
client3ds/                      Client 3DS (devkitARM + libctru + libjpeg-turbo)
├── Makefile                    modèle officiel devkitPro adapté (.3dsx ; cible `cia` non testée)
├── include/protocol.hpp        protocole (constantes, paquets) — doit rester identique à protocol.py
├── include/platform.hpp        couche « plate-forme » : libctru (console) ou threads standard (tests sur PC)
├── source/
│   ├── main.cpp                initialisation, boucle principale, arrêt propre
│   ├── geforce_now.cpp/.hpp    client du relais : thread réseau, jeux, file, flux, reconnexion
│   ├── network.cpp/.hpp        sockets non bloquants (TCP par lignes, UDP)
│   ├── video_decoder.cpp/.hpp  réassemblage, tampon de gigue, décodage JPEG (thread dédié), triple tampon
│   ├── renderer.cpp/.hpp       dessin logiciel RGB565, police, formes (écrans 400×240 / 320×240)
│   ├── ui.cpp/.hpp             interface : onglets, file d'attente, manette tactile, menus
│   ├── input.cpp/.hpp          lecture des touches, correspondance, zones tactiles, paquet d'entrées
│   ├── settings.cpp/.hpp       réglages (sdmc:/3ds/gfn3ds/settings.ini)
│   ├── audio.cpp/.hpp          module audio volontairement vide (voir § 11)
│   ├── platform.cpp            implémentation de platform.hpp
│   └── font_data.inc           police 8×8 (table CP437 extraite de la police de console de libctru)
├── assets/                     icône, bannière, son muet (générés par tools/make_assets.py)
├── tools/                      gen_font.py, make_assets.py, gfn3ds.rsf, check_3ds_syntax.sh
└── tests/host/                 tests unitaires et client « sans écran » pour les tests de bout en bout

relay/                          Relais PC (Python)
├── start_relay.bat             installation (.venv) + lancement
├── requirements.txt            numpy, opencv-python, dxcam, vgamepad (les deux derniers : Windows)
├── games/  games.json  games.example.json
├── gfn3ds_relay/               protocol · config · server · capture · encoder · gamepad · gfn
│                               games · winutil · probe_queue · __main__
└── tests/                      relais, synchronisation du protocole, client C++ réel contre relais réel
```

**Protocole v1** (texte ASCII, champs séparés par une tabulation, une ligne = une commande)

| Port | Sens | Contenu |
|---|---|---|
| TCP 47970 | 3DS ↔ relais | `HELLO · LIST · LAUNCH · WATCH · STOP · PROFILE · PING · RX · STATUS · BYE` ; réponses `OK · PROFILES · STATE · GAMES · ERR · PONG · STATS` |
| UDP 47971 | 3DS → relais | paquet d'entrées de 16 octets, 60 Hz pendant le flux |
| UDP 47972 | relais → 3DS | vidéo : en-tête de 12 octets + 1 200 octets de JPEG au plus par datagramme (fragments d'une image) |
| UDP 47973 | relais → réseau | balise de découverte `GFN3DS` |

États annoncés (`STATE`) : `IDLE · LAUNCHING · QUEUE · LOADING · STREAMING · ERROR · UNTRACKED`. Si la liaison tombe, la console se reconnecte seule (délai 0,5 s → 4 s) et **redemande le flux** ; le relais relâche la manette virtuelle après 0,3 s sans entrée et libère la console après 15 s de silence.

---

## 14. Licences, références et mentions

- Le code de ce dépôt : à vous de choisir une licence avant de le publier.
- **libctru** (licence zlib) ; la table de glyphes de `font_data.inc` en est extraite. **libjpeg-turbo** (licences BSD modifiée, IJG et zlib).
- Côté PC : `numpy`, `opencv-python`, `dxcam`, `vgamepad` et le pilote **ViGEmBus** sont des projets tiers, chacun sous sa licence.
- « NVIDIA », « GeForce NOW » : marques de leurs propriétaires. Ce projet n'est ni approuvé ni soutenu par NVIDIA ; aucun logo ni élément graphique de NVIDIA n'est reproduit (l'icône et la bannière sont générées).
- Références consultées pour ce document : [ViGEmBus — fin de vie](https://docs.nefarius.at/projects/ViGEm/End-of-Life/) ; [notes de version de vgamepad](https://github.com/yannbouteiller/vgamepad/releases) (« Automated the installation of ViGEmBus »).
