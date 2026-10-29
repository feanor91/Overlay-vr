# OverlayVR : zones passthrough globales pour Virtual Desktop (VDXR)

Dessine dans le casque des **plans** (clavier, souris) et des **volumes** (HOTAS, manettes) qui deviennent
du passthrough dans **n'importe quel jeu OpenXR** (MSFS 2024 inclus), sans SteamVR.

Deux programmes, un seul fichier de zones (`%APPDATA%\OverlayVR\zones.json`) :

| Programme | Rôle |
|---|---|
| `OverlayVR_Editor.exe` | Application OpenXR à lancer dans le casque **avant** le jeu. Fond magenta (donc on voit la pièce) ; on y dessine les zones avec les manettes. |
| `XR_APILAYER_OVERLAYVR_zones.dll` | Couche API OpenXR implicite, chargée par tous les jeux OpenXR. Ajoute les zones en magenta pur (255, 0, 255) à chaque image ; Virtual Desktop les remplace par le passthrough. Recharge `zones.json` à chaud. |

## Installation

1. Récupère les binaires : onglet **Actions** du dépôt, workflow *build-windows*, artefact `OverlayVR-windows`
   (ou compile : `cmake -S . -B build -A x64 && cmake --build build --config Release`).
2. Mets les 5 fichiers dans un dossier permanent, puis lance `install_layer.bat` **en administrateur**
   (`uninstall_layer.bat` pour retirer).
3. Dans Virtual Desktop, onglet *Streaming* : active le passthrough par couleur (chroma key) sur **RGB 255, 0, 255**
   et choisis VDXR comme runtime OpenXR.

## Utilisation

1. Dans le casque, lance `OverlayVR_Editor.exe` depuis le bureau Virtual Desktop. Le fond est du passthrough.
2. Dessine, ajuste, ferme avec le bouton menu gauche. Chaque modification est enregistrée aussitôt.
3. Lance ton jeu : les zones apparaissent en passthrough. Le journal de la couche est dans `%APPDATA%\OverlayVR\layer.log`.

### Manettes Touch (éditeur)

Chaque manette projette un **laser** ; la zone qu'il touche (elle devient blanche) est celle sur laquelle agissent les boutons.
Les commandes sont aussi **affichées dans le casque** (panneau à gauche, masquable au clic du stick gauche).

**Main droite**

| Action | Commande |
|---|---|
| Tracer une zone | Gâchette, en plusieurs temps : (1) maintiens la gâchette et tire le **1er bord** de A à B, c'est lui qui donne l'**orientation** ; (2) vise le côté opposé, gâchette pour valider. Un plan s'arrête là. Pour un volume : (3) place la manette au **sommet** de l'objet (la boîte suit ta main), gâchette ; (4) descends-la au **bas** de l'objet, gâchette (ou valide directement : le bas reste alors au plan de visée). Le haut et le bas sont des hauteurs réelles : une boîte peut donc dépasser du bureau **et** descendre dessous. Stick ▲▼ pour affiner, B annule |
| Changer de mode de tracé | **A** : volume (cyan) → plan (jaune) → **forme libre** (orange). Le laser prend la couleur du mode |
| Tracer une **forme libre** | Mode forme libre : **gâchette à chaque sommet** du contour (sur le plan de visée), puis **clic sur le 1er point** (ou **A**) pour fermer ; **B** retire le dernier sommet. Ensuite, comme un volume : manette au **sommet** puis au **bas**, gâchette à chaque fois. Formes concaves possibles (L, U…) |
| Déplacer / orienter une zone | Grip tenu sur la zone visée, puis bouge la main et **tourne le poignet** ; stick droit ◄► tourne, ▲▼ agrandit/réduit ; stick gauche ▲▼ change la hauteur seule |
| Supprimer | B sur la zone visée (ou B pendant qu'on la tient) |
| Hauteur du plan de visée | Stick ▲▼ (sans grip) pour le monter ou le descendre, ou pose la manette sur une surface et clique le stick ; mémorisée (75 cm par défaut) |

**Main gauche**

| Action | Commande |
|---|---|
| Annuler | X (n'importe quelle action, plusieurs fois de suite) ; **maintenu 2 s : tout effacer** (X annule aussi ça) |
| Affichage conditionnel | Y sur la zone visée : visible seulement si tu regardes au moins 25° vers le bas (repère blanc au-dessus) |
| Déplacer une zone | Grip gauche (ou droit) sur la zone visée, puis bouge la main et tourne le poignet |
| Rattacher une zone au jeu actif | Gâchette gauche sur la zone visée (prise en compte au **relâchement**) |
| Choisir le jeu actif | Stick gauche ▲▼ (tous les jeux, MSFS, Assetto Corsa…) |
| **Recaler toutes les zones** (elles sont toutes décalées) | Grip gauche **dans le vide** (laser sur aucune zone) : toutes les zones suivent ta main d'un bloc, poignet compris ; pendant ce geste, stick droit ◄► pour tourner finement, ▲▼ pour avancer/reculer |
| Aide / quitter | Clic du stick : masque l'aide, ou la ré-affiche **devant toi** (les panneaux restent fixes dans la pièce, ils ne suivent pas la tête) ; Menu : quitter |

## Choisir les jeux concernés

La couche est chargée par tous les jeux OpenXR, mais elle ne **dessine** que dans ceux que tu autorises. Les réglages sont dans
`%APPDATA%\OverlayVR\settings.json` (raccourci « Réglages » dans le menu Démarrer), relus à chaud :

```json
{
  "enabled": true,
  "mode": "allowlist",
  "apps": ["FlightSimulator2024.exe", "FlightSimulator.exe"]
}
```

| Réglage | Effet |
|---|---|
| `enabled: false` | Interrupteur général : plus aucune zone nulle part |
| `mode: "allowlist"` (défaut) | Seulement les jeux listés dans `apps` |
| `mode: "blocklist"` | Tous les jeux sauf ceux listés |
| `mode: "all"` | Tous les jeux |

Autres réglages du même fichier :

| Réglage | Effet |
|---|---|
| `gaze_only: true` (défaut) | Une zone n'apparaît que **quand ton regard pointe vers elle** : un rayon part du centre de ta vue (orientation de la tête, pas de suivi des yeux) et doit croiser la zone ; `false` : toujours visibles |
| `gaze_margin: 8` | Tolérance en degrés autour de la zone. Plus petit = il faut viser plus précisément ; plus grand = elle apparaît plus tôt. Une fois affichée, elle reste visible 4° de plus |
| `edge_overlap_cm: 3` | Recouvrement des faces d'un volume : cache les lignes roses aux raccords |
| `edge_passes: 1` | Chaque face est superposée n fois (1 à 6) : les bords sont plus propres (moins de liseré rose) mais chaque passe ajoute une couche à composer et **coûte des FPS**. 1 par défaut ; monte à 2 si tu as de la marge |

Si un léger liseré rose subsiste, augmente aussi la tolérance de la couleur du chroma key dans Virtual Desktop.

Pour ajouter un jeu, lance-le une fois puis ouvre `layer.log` (raccourci « Journal de la couche ») : la ligne
`couche chargée dans XXX.exe` donne le nom exact à copier dans `apps` (le nom de l'application OpenXR marche aussi).

## Recaler les zones en jeu (au clavier)

Le repère de la pièce n'est pas toujours exactement le même d'une session à l'autre : toutes les zones peuvent se retrouver
légèrement décalées. Sans quitter le jeu, tiens **Ctrl + Alt** et utilise :

| Touche (avec Ctrl+Alt) | Effet |
|---|---|
| ↑ ↓ ← → | Avancer / reculer / gauche / droite, selon la direction où tu regardes |
| Page haut / Page bas | Monter / descendre |
| Inser / Suppr | Tourner vers la droite / vers la gauche |
| Origine | Tout remettre à zéro |
| + Maj | Pas plus grands (15 cm/s et 15°/s au lieu de 3 cm/s et 3°/s) |

Le décalage est **propre à chaque jeu** (profil) : recaler MSFS ne touche pas Assetto Corsa. Il est enregistré dans `settings.json`
(champs `offset` / `offset_yaw_deg` du profil) quelques instants après avoir relâché les touches. `"hotkeys": false` désactive ces raccourcis.

## Activer / désactiver les zones en jeu (touche ou bouton de HOTAS)

Comme le clic gauche sur l'icône de la barre d'état, mais sans quitter le casque : une touche du clavier et/ou un bouton de ton
joystick / HOTAS inversent l'interrupteur général (`enabled` de `settings.json`, donc l'icône de la barre d'état suit).

**Le plus simple : la fenêtre de configuration** (`OverlayVR Configuration` dans le menu Démarrer, ou clic droit sur l'icône de
la barre d'état → *Configuration…*). Elle liste les joysticks / HOTAS détectés ; clique **Détecter…** puis appuie sur le bouton
voulu : le périphérique et le numéro de bouton sont renseignés. Pour la touche clavier, clique **Définir…** puis appuie sur la
touche (avec Ctrl / Alt / Maj si tu veux). Un encart affiche en direct les boutons enfoncés, pour tester. Clique **Enregistrer** :
c'est pris en compte tout de suite, sans relancer le jeu. La fenêtre règle aussi l'interrupteur général, le regard, les bords et
les jeux concernés.

À la main, dans `%APPDATA%\OverlayVR\settings.json` (relu à chaud) :

```json
"toggle_key": "space",
"toggle_joystick": "Throttle",
"toggle_joystick_button": 12
```

| Réglage | Valeurs |
|---|---|
| `toggle_key` (défaut `"space"`) | `space`, `enter`, `tab`, `f1`…`f24`, `a`…`z`, `0`…`9`, `numpad0`…`numpad9`, `insert`, `delete`, `home`, `end`, `pageup`, `pagedown`, flèches `up`/`down`/`left`/`right`. Modificateurs possibles : `ctrl+f9`, `ctrl+alt+x`, `shift+space`. `"none"` : désactivé |
| `toggle_joystick_button` (défaut `0` : aucun) | Numéro du bouton, tel que l'affiche *Configurer les contrôleurs de jeu USB* (`joy.cpl` → Propriétés), à partir de 1 (jusqu'à 128) |
| `toggle_joystick` (défaut `""`) | Morceau du nom du périphérique, sans tenir compte des majuscules (`"Throttle"`, `"Warthog"`, `"VPC"`…). Vide : le bouton est cherché sur tous les joysticks. Les noms exacts sont dans `layer.log` (lignes « joystick détecté ») |

- Le joystick est lu en arrière-plan, sans exclusivité (DirectInput) : MSFS garde ses propres assignations, et le bouton choisi
  peut donc servir aussi à autre chose dans le jeu.
- La touche est lue globalement (même si le jeu n'a pas le focus). Les deux ne basculent que dans un jeu concerné par la couche.
- Un HOTAS branché après le lancement du jeu est repris dans les 10 secondes.

## Zones par jeu

**Depuis la fenêtre de configuration** (onglet *Zones et jeux*) : la liste montre toutes les zones et le jeu de chacune. Sélectionne
une ou plusieurs zones (Ctrl / Maj), choisis le jeu (ou « Tous les jeux ») et clique **Affecter** : c'est enregistré tout de suite
dans `zones.json` et pris en compte en jeu. Le même onglet permet d'ajouter, renommer ou supprimer un jeu (nom + exécutables).
Ferme l'éditeur du casque avant d'affecter, sinon il peut réécrire `zones.json`.

**Nommer les zones** : à la création, dès qu'une zone est tracée dans le casque, un **bip** retentit et un panneau « NOM DE LA ZONE »
apparaît **devant toi** (collé au regard, un peu en dessous) : tape le nom sur le
clavier du PC puis **Entrée** (Retour arrière corrige, **Échap** garde le nom par défaut `plan1`, `volume2`…). Sans rien taper,
la proposition disparaît au bout de 20 s. Après coup, sélectionne la zone dans l'onglet *Zones et jeux*, change son nom et clique
**Renommer**. Les noms sont uniques (un doublon reçoit un numéro dans l'éditeur, est refusé dans la fenêtre).

Chaque jeu est un **profil** dans `settings.json` (`MSFS` et `Assetto Corsa` par défaut), avec la liste de ses exe :

```json
"profiles": [
  { "name": "MSFS", "apps": ["FlightSimulator2024.exe", "FlightSimulator.exe"] },
  { "name": "Assetto Corsa", "apps": ["acs.exe", "AssettoCorsa.exe"] }
]
```

Une zone peut appartenir à un profil : elle n'apparaît alors que dans ce jeu. Une zone sans profil apparaît dans tous les jeux
autorisés. Les jeux qui ont un profil sont autorisés automatiquement. Pour un autre jeu, ajoute un profil avec son exe
(le nom exact est dans `layer.log`, ligne « couche chargée dans … »).

Dans l'éditeur :
- **Stick gauche ▲▼** : choisit le **jeu actif** (« tous les jeux » ou un profil). Les nouvelles zones lui appartiennent ; les zones des autres jeux
  s'affichent en gris et ne sont pas sélectionnables.
- **Gâchette gauche** sur une zone visée : la rattache au **jeu actif**.

## Lancer le gestionnaire de zones au clavier ou au HOTAS

Sans toucher à la souris ni à la fenêtre de configuration : l'**application de la barre d'état** (il faut qu'elle tourne, comme d'habitude
après l'installation) surveille une touche du clavier et un bouton de joystick / HOTAS, et lance le gestionnaire de zones quand tu
les actionnes. Rien ne demande les droits administrateur.

**Le plus simple : la fenêtre de configuration**, onglet **Gestionnaire de zones** (clic droit sur l'icône → *Configuration…*) :
clique **Définir…** puis appuie sur la touche voulue (avec Ctrl / Alt / Maj si tu veux), ou **Détecter…** puis appuie sur le bouton de ton
HOTAS : le périphérique et le numéro de bouton sont renseignés. Un encart affiche en direct les boutons enfoncés. Clique **Enregistrer**.

- **Clavier** : **Ctrl + Alt + E** par défaut.
- **HOTAS** : aucun bouton par défaut. À la main, dans `settings.json` (clic droit sur l'icône → *Modifier settings.json*) :

```json
"editor_key": "ctrl+alt+e",
"editor_joystick": "Throttle",
"editor_joystick_button": 14
```

| Réglage | Valeurs |
|---|---|
| `editor_key` (défaut `"ctrl+alt+e"`) | Même format que `toggle_key` : `f9`, `ctrl+f9`, `space`… ; `"none"` : pas de touche |
| `editor_joystick_button` (défaut `0` : aucun) | Numéro du bouton, comme dans `joy.cpl` (même numérotation que `toggle_joystick_button`), de 1 à 128 |
| `editor_joystick` (défaut `""`) | Morceau du nom du périphérique, sans tenir compte des majuscules ; vide : n'importe quel joystick |

Un seul gestionnaire à la fois : si le gestionnaire tourne déjà, la touche ne fait rien. Un bip signale le lancement. Le gestionnaire
ne peut pas tourner pendant qu'un jeu utilise le casque : lance-le **jeu fermé**. Le raccourci en cours est rappelé dans le menu de l'icône.

## Application de la barre d'état

Après l'installation, **OverlayVR** tourne dans la zone de notification de Windows (à côté de l'horloge ; au besoin, flèche « ^ »
pour la voir). L'icône est un **œil** qui donne l'état : iris **vert** = zones actives, **gris barré** = zones coupées, **rouge barré** = couche OpenXR désactivée.

| Action | Comment |
|---|---|
| Activer / désactiver les zones, tout de suite, même en jeu | **Clic gauche** sur l'icône (sans droits administrateur) |
| Lancer le gestionnaire de zones | Clic droit, **Lancer le gestionnaire de zones** (à faire jeu fermé, depuis le casque) |
| Charger / ne plus charger la couche OpenXR dans les jeux | Clic droit, **Couche OpenXR chargée par les jeux** (demande l'administrateur ; effet au prochain lancement du jeu) |
| Réglages, dossier des données | Clic droit, **Réglages** / **Ouvrir le dossier des données** |
| Démarrer avec Windows | Clic droit, **Lancer au démarrage de Windows** |

Pour savoir si la couche pèse sur tes FPS, désactive-la (case décochée), relance le jeu et compare.

## Limites et points à valider casque sur la tête

Ce code n'a **pas encore été testé avec un casque** : il compile, la logique mathématique est vérifiée, le reste est à valider.

- Les zones sont ancrées dans l'espace **STAGE** (repère de la pièce). Si VDXR ne le fournit pas, repli sur LOCAL
  (les zones seraient alors décalées d'une session à l'autre) ; l'éditeur le signale dans la console.
- Seuls les jeux **D3D11 et D3D12** sont pris en charge (Vulkan/OpenGL : couche inactive, voir `layer.log`).
- Les quads du volume sont des faces plates : on ne voit que les faces tournées vers la tête.
- Un gros nombre de zones ajoute des couches de composition (3 par volume au plus).
- Ancienne version SteamVR/Python : `legacy/`.

### Formes libres : limites

Dans le jeu, une forme libre est composée de rectangles : un par côté (les murs tournés vers toi) et des bandes de 5 cm au plus pour le dessus ou
le dessous. Un contour **oblique** est donc légèrement en escalier. Chaque rectangle est une couche de plus pour le casque : garde les formes
simples (une dizaine de sommets) si tes FPS baissent.

## Diagnostic de performance

Si les zones « nagent » quand tu bouges la tête ou si les FPS baissent :
- **Dans l'éditeur** : la 2e ligne du panneau d'état affiche `N img/s, calcul X ms (pire Y)` : le nombre d'images par seconde, le temps
  de calcul moyen de l'éditeur par image, et la pire image de la dernière seconde. La console de l'éditeur le répète toutes les 5 s.
- **En jeu** : `layer.log` reçoit une ligne `performances : …` toutes les 60 s : temps moyen et pire passé par la couche à chaque image,
  et nombre de couches ajoutées.

Un calcul de quelques millisecondes ou moins est normal. Le retard visible quand la tête bouge vite vient surtout de la diffusion par
Virtual Desktop : le passthrough est en temps réel dans le casque, alors que l'image virtuelle arrive avec quelques dizaines de
millisecondes de retard (VD corrige la rotation de la tête, pas le déplacement). C'est surtout visible **dans l'éditeur** (une image
calculée par le programme) ; en jeu, les zones sont des couches que le runtime recale lui-même à la bonne position. Pour savoir si une version a changé quelque chose,
installe une ancienne version depuis la page Releases et compare.
