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
| Dessiner une zone | Gâchette droite maintenue : tire de l'angle A à l'angle opposé B (aligné sur ton regard) |
| Volume (cyan) ou plan (jaune) | Bouton A (le bout de la manette change de couleur) |
| Déplacer | Grip droit près d'une zone (à moins de 8 cm), puis bouge la main |
| Tourner / agrandir-réduire | Grip maintenu + stick droit : X tourne, Y change la taille |
| Supprimer | Grip maintenu + B |
| Annuler la dernière zone | X (gauche) |
| Affichage conditionnel | Y (gauche) : la zone n'apparaît que si tu regardes au moins 25° vers le bas (repère blanc au-dessus) |
| Quitter | Menu (gauche) |

Le fichier `zones.json` est modifiable à la main (`pos`, `size` en mètres, `yaw` en radians, `look` en degrés).

## Limites et points à valider casque sur la tête

Ce code n'a **pas encore été testé avec un casque** : il compile, la logique mathématique est vérifiée, le reste est à valider.

- Les zones sont ancrées dans l'espace **STAGE** (repère de la pièce). Si VDXR ne le fournit pas, repli sur LOCAL
  (les zones seraient alors décalées d'une session à l'autre) ; l'éditeur le signale dans la console.
- Seuls les jeux **D3D11 et D3D12** sont pris en charge (Vulkan/OpenGL : couche inactive, voir `layer.log`).
- Les quads du volume sont des faces plates : on ne voit que les faces tournées vers la tête.
- Un gros nombre de zones ajoute des couches de composition (3 par volume au plus).
- Ancienne version SteamVR/Python : `legacy/`.
