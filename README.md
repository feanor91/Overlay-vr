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

| Action | Commande |
|---|---|
| Viser | Un **laser** part de la manette droite et touche un plan horizontal (le bureau). Le point visé et un petit carré blanc au sol indiquent où tu vises |
| Régler la hauteur du bureau | Pose la manette droite sur le bureau et clique le stick droit (valeur mémorisée, 75 cm par défaut) |
| Dessiner un **plan** | Gâchette droite maintenue : de l'angle A à l'angle opposé B au laser, relâche pour créer |
| Dessiner un **volume** | Même emprise, puis lève la main pour régler la hauteur (stick droit pour affiner), gâchette pour valider, B pour annuler |
| Volume (cyan) ou plan (jaune) | Bouton A (le bout de la manette change de couleur) |
| Déplacer | Grip droit près d'une zone (à moins de 8 cm), puis bouge la main |
| Tourner / agrandir-réduire | Grip maintenu + stick droit : X tourne, Y change la taille |
| Supprimer | Grip maintenu + B |
| Annuler la dernière zone | X (gauche) |
| Affichage conditionnel | Y (gauche) : la zone n'apparaît que si tu regardes au moins 25° vers le bas (repère blanc au-dessus) |
| Quitter | Menu (gauche) |

### Objets déplaçables (clavier, HOTAS…)

Une zone peut appartenir à un **objet** (`objet1` à `objet3`, champ `object` du JSON). Tout l'objet se déplace d'un bloc,
de sorte que tu peux dessiner les zones là où le matériel est au repos, puis les recaler quand tu le remets en place.

| Action | Commande |
|---|---|
| Choisir l'objet actif | Clic du stick gauche : blanc = aucun (fixe), orange, vert, violet. Le bout de la manette gauche prend la couleur, les nouvelles zones appartiennent à cet objet |
| Rattacher une zone existante | Gâchette gauche sur la zone (à l'objet actif ; blanc = la rendre fixe) |
| Déplacer / tourner tout l'objet | Grip gauche près d'une de ses zones, puis bouge la main : toutes les zones suivent d'un bloc |

Remise en place : lance l'éditeur, regarde ton clavier réel à travers le passthrough, attrape l'objet au grip gauche et
glisse-le jusqu'à ce qu'il recouvre le vrai clavier. Tu le fais une fois à chaque installation du matériel ; il n'y a pas de
suivi automatique, rien n'est lu pendant le jeu.

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

Pour ajouter un jeu, lance-le une fois puis ouvre `layer.log` (raccourci « Journal de la couche ») : la ligne
`couche chargée dans XXX.exe` donne le nom exact à copier dans `apps` (le nom de l'application OpenXR marche aussi).

## Limites et points à valider casque sur la tête

Ce code n'a **pas encore été testé avec un casque** : il compile, la logique mathématique est vérifiée, le reste est à valider.

- Les zones sont ancrées dans l'espace **STAGE** (repère de la pièce). Si VDXR ne le fournit pas, repli sur LOCAL
  (les zones seraient alors décalées d'une session à l'autre) ; l'éditeur le signale dans la console.
- Seuls les jeux **D3D11 et D3D12** sont pris en charge (Vulkan/OpenGL : couche inactive, voir `layer.log`).
- Les quads du volume sont des faces plates : on ne voit que les faces tournées vers la tête.
- Un gros nombre de zones ajoute des couches de composition (3 par volume au plus).
- Ancienne version SteamVR/Python : `legacy/`.
