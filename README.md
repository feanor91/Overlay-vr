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
| Volume (cyan) ou plan (jaune) | A |
| Déplacer / orienter une zone | Grip tenu sur la zone visée, puis bouge la main et **tourne le poignet** ; stick droit ◄► tourne, ▲▼ agrandit/réduit ; stick gauche ▲▼ change la hauteur seule |
| Supprimer | B sur la zone visée (ou B pendant qu'on la tient) |
| Hauteur du plan de visée | Stick ▲▼ (sans grip) pour le monter ou le descendre, ou pose la manette sur une surface et clique le stick ; mémorisée (75 cm par défaut) |

**Main gauche**

| Action | Commande |
|---|---|
| Annuler | X (n'importe quelle action, plusieurs fois de suite) ; **maintenu 2 s : tout effacer** (X annule aussi ça) |
| Affichage conditionnel | Y sur la zone visée : visible seulement si tu regardes au moins 25° vers le bas (repère blanc au-dessus) |
| Objet actif | Stick ◄► : aucun (blanc), orange, vert, violet ; les nouvelles zones lui appartiennent |
| Rattacher une zone à l'objet actif | Gâchette sur la zone visée |
| Déplacer un objet entier | Grip tenu sur une de ses zones ; bouge la main et tourne le poignet |
| **Recaler toutes les zones** (elles sont toutes décalées) | Grip gauche **dans le vide** (laser sur aucune zone) : toutes les zones suivent ta main d'un bloc, poignet compris ; pendant ce geste, stick droit ◄► pour tourner finement, ▲▼ pour avancer/reculer |
| Aide / quitter | Clic du stick : masque l'aide, ou la ré-affiche **devant toi** (les panneaux restent fixes dans la pièce, ils ne suivent pas la tête) ; Menu : quitter |

### Objets déplaçables (clavier, HOTAS…)

Une zone peut appartenir à un **objet** (`objet1` à `objet3`, champ `object` du JSON). Tout l'objet se déplace d'un bloc :
dessine les zones là où le matériel est au repos, puis recale-les quand tu le remets en place. Lance l'éditeur, regarde ton
clavier réel à travers le passthrough, attrape l'objet au grip gauche et glisse-le jusqu'à ce qu'il le recouvre. Il n'y a pas
de suivi automatique : rien n'est lu pendant le jeu.

Le fichier `zones.json` est modifiable à la main (`pos`, `size` en mètres, `yaw` en radians, `look` en degrés).

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

Le décalage est enregistré dans `settings.json` (`offset`, `offset_yaw_deg`) quelques instants après avoir relâché les touches ;
l'éditeur l'intègre aux zones au lancement suivant. `"hotkeys": false` désactive ces raccourcis.

## Application de la barre d'état

Après l'installation, **OverlayVR** tourne dans la zone de notification de Windows (à côté de l'horloge ; au besoin, flèche « ^ »
pour la voir). La pastille donne l'état : **verte** = zones actives, **grise** = zones coupées, **rouge** = couche OpenXR désactivée.

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
