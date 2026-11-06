# Portage de Diorama64 sur Meta Quest 2 et Quest 3

Plan de travail détaillé, établi à partir du dépôt `mist64/diorama64` (alpha, cible Apple Vision Pro).

Objectif : un APK unique, installable par sideload, qui affiche Super Mario 64 en diorama posé sur une vraie table,
en réalité mixte (passthrough), sur Quest 2 et Quest 3, avec une qualité graphique adaptée à chaque casque.

---

## 1. État des lieux du code

### 1.1 Ce qui se porte tel quel (C/C++ sans dépendance Apple)

| Bloc | Emplacement | Remarque |
|---|---|---|
| Jeu (sm64coopdx + 14 patches) | `games/sm64/upstream`, `games/sm64/patches` | Déjà compilé sans SDL réel (stub) pour visionOS : même approche sur Android. |
| Adaptateur SM64 | `games/sm64/adapter` (~40 fichiers C) | Capture des draws, HUD, packs, salles, réglages. |
| Runtime | `runtime/core`, `runtime/capture` | Boucle, ring buffers, audio ring, enregistrement/replay (`sarec`). |
| Rendu commun | `runtime/render/common` | Renderer, combiner N64, ombres, éclairage, « looks ». Parle à une interface abstraite `IRenderDevice`. |
| Logique « visionOS » portable | `runtime/platform/visionos/*.cpp` (lib `ar_vos_logic`) | Placement, plans, input, perfs, fovéation, audio de scène, rééchantillonnage : C++ pur, réutilisable. |
| Plateforme factice | `runtime/platform/fake` | Utile pour les tests headless. |

### 1.2 Ce qu'il faut remplacer

| Bloc Apple | Taille | Remplaçant Quest |
|---|---|---|
| `render/metal/metal_device.mm` + `msl_emitter.cpp` | ~2 500 lignes | Backend **Vulkan 1.1** + émetteur **GLSL → SPIR-V** |
| `vos_platform.mm`, `vos_contract.mm`, `vos_session.cpp` | ~1 600 lignes | Plateforme **OpenXR** (session, swapchains, frame loop) |
| `vos_placer.mm` (ARKit, plans) | ~340 lignes | **Scene API Meta** (`XR_FB_scene`, ancres spatiales) |
| `vos_audio.mm`, `vos_phase.mm` (Core Audio, PHASE) | ~600 lignes | **OpenAL Soft** (HRTF) sur **AAudio/Oboe** |
| `platform/apple/gc_pad.mm` (GameController) | petit | **Actions OpenXR** (manettes Touch) + manette Bluetooth via input Android |
| `apps/visionos` (SwiftUI, fenêtre 4 onglets) | ~2 250 lignes (Swift + glue) | **Panneau en réalité mixte** (Dear ImGui sur un calque quad OpenXR) |
| `apps/macos_harness` (AppKit + Metal) | — | **Harnais Linux** (SDL2 + Vulkan), pour développer sans casque |
| Build : Xcode, `cmake/Toolchain.cmake`, `scripts/visionos_*.py`, `sips` | — | **NDK Android + CMake + Gradle**, `scripts/quest_*.py`, Pillow |

### 1.3 Points favorables repérés

- `ar_platform.h` est déjà calqué sur OpenXR : `wait_frame` / `begin_frame` / `end_frame` correspondent à
  `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame`, les états `ar_session_state` à `XrSessionState`, et l'interface prévoit
  `set_passthrough`, `persist_anchor`, `share_anchor`. Les fonctions optionnelles (`get_environment`, `get_preview`)
  acceptent `NULL`.
- Le rendu Metal utilise des **attachements MSAA « memoryless »** (mémoire de tuile). Les GPU Adreno des Quest sont aussi
  des GPU à tuiles : l'équivalent Vulkan (`TRANSIENT_ATTACHMENT` + `LAZILY_ALLOCATED`) existe et le MSAA 4× y est peu coûteux.
- Les cibles « layered » (`render_target_array_index`) se transposent en **multiview** (`VK_KHR_multiview`), qui est le
  chemin rapide sur Quest (un seul passage pour les deux yeux).
- L'API de l'app (`apps/visionos/vos_api.h`, fonctions `arvos_*`) est une API C : le panneau de réglages Quest peut
  l'appeler directement, sans réécrire la logique des réglages.

### 1.4 Points d'attention

- `ar_environment.cube_texture` est un `id<MTLTexture>` venant de la sonde d'environnement ARKit. Il n'y a pas
  d'équivalent sur Quest : `get_environment = NULL`, reflets désactivés ou cube statique.
- Trois kernels de calcul Metal (`mip_gamma`, `env_reduce`, `ao_fill`) sont à réécrire en compute shaders Vulkan.
- Les combiners N64 sont générés dynamiquement (clé `fc_combiner_key`) : il faut **compiler les shaders à l'exécution**
  (glslang/shaderc embarqué) et **persister un `VkPipelineCache`** pour éviter les saccades.
- La fovéation de visionOS (rate maps) devient la fovéation fixe Meta (`XR_FB_foveation`). Pas d'eye tracking sur Quest 2/3.
- `scripts/packs.py` utilise `sips` (outil macOS) pour redimensionner : à remplacer par Pillow.

---

## 2. Cibles matérielles et profils de qualité

| | Quest 2 | Quest 3 / 3S |
|---|---|---|
| SoC / GPU | XR2 Gen 1 / Adreno 650 | XR2 Gen 2 / Adreno 740 (~2,5×) |
| RAM | 6 Go | 8 Go |
| Écran par œil | 1832 × 1920 | 2064 × 2208 (3S : 1832 × 1920) |
| Fréquences | 72 / 90 / 120 Hz | 72 / 90 / 120 Hz |
| Passthrough | Niveaux de gris, basse définition | Couleur, haute définition |
| Profondeur | Non | Oui (Depth API → occlusion) |
| Détection de table | Room setup manuel | Room setup automatique + maillage |

Détection au lancement : `xrGetSystemProperties` (`systemName`) ou la propriété Android `ro.product.model`.

### Profils proposés (valeurs de départ, à ajuster après mesures)

| Réglage | Quest 2 | Quest 3 |
|---|---|---|
| Fréquence cible | 72 Hz | 90 Hz |
| Échelle de rendu | 0,8–0,9 | 1,0 |
| MSAA | 4× | 4× |
| Fovéation fixe | Haute | Moyenne |
| Carte d'ombre | 1024, PCF réduit | 2048 |
| Occlusion ambiante (AO) | Désactivée | Activée (bake ponctuel) |
| Effet de découpe (cutaway) | Simplifié | Complet |
| Reflets / probe | Désactivés | Cube statique |
| Textures HD | Mip le plus fin sauté (÷4 mémoire) | Complètes |
| Packs de modèles 3D | Désactivés ou pack léger | Activés |
| Occlusion par la profondeur | — | Optionnelle (mains devant Mario) |

**Un seul jeu d'assets** : textures converties une fois en ASTC avec mipmaps ; le profil choisit le niveau de mip de
départ et les packs actifs. Version décimée d'un pack de modèles générée au build seulement si un pack précis est
indispensable sur Quest 2.

---

## 3. Architecture cible

```
apps/quest/                     ← nouvelle app Android (NativeActivity / GameActivity)
  AndroidManifest.xml, build.gradle, quest_main.cpp, quest_panel.cpp (ImGui)
runtime/render/vulkan/          ← nouveau backend IRenderDevice
  vk_device.cpp, glsl_emitter.cpp, vk_compute.cpp, vk_pipeline_cache.cpp
runtime/platform/openxr/        ← nouvelle plateforme ar_platform
  xr_platform.cpp, xr_session.cpp, xr_passthrough.cpp, xr_scene.cpp, xr_input.cpp, xr_profiles.cpp
runtime/platform/android/       ← audio, input manette, fichiers, logs
  aaudio_out.cpp, openal_scene.cpp, android_pad.cpp, android_paths.cpp
runtime/platform/linux/         ← harnais de développement sans casque
  linux_harness.cpp (SDL2 + Vulkan, caméra libre, capture PNG)
runtime/platform/visionos/*.cpp ← renommer la partie portable en runtime/platform/xr_common/ (ar_xr_logic)
scripts/quest_build.py, quest_install.py, quest_logs.py
```

Le code Apple reste en place et continue de compiler : on ajoute des cibles, on n'en retire pas. Les options CMake
`SM64AR_QUEST` et `SM64AR_LINUX` s'ajoutent à `SM64AR_VISIONOS`.

---

## 4. Phases de travail

Chaque phase a un livrable et un critère de validation. Les phases 0 à 3 se font sans casque ; à partir de la phase 4,
chaque itération demande un test sur l'appareil.

### Phase 0 — Préparation (≈ 1 jour)

- Fork du dépôt, branche `quest-port`, sous-module sm64coopdx récupéré.
- Rendre le build indépendant de macOS : `cmake/Toolchain.cmake` ne vérifie Xcode que pour les cibles Apple ;
  `project()` n'active `OBJC/OBJCXX` que sur Apple.
- Porter `scripts/sm64_oracle.py`, `sm64_prepare.py`, `fetch_lua.py` sur Linux (génération des assets depuis la ROM).
- Remplacer `sips` par Pillow dans `packs.py`.

**Validation** : sur Linux, la ROM est extraite, les patches s'appliquent, `sm64_game` + `sm64_adapter` + `ar_core`
compilent en bibliothèques statiques (gcc/clang x86-64).

### Phase 1 — Harnais Linux headless (≈ 2 jours)

- Plateforme factice (`platform/fake`) + backend de rendu nul : le jeu tourne, les frames sont capturées
  (`fc_builder`), un replay `sarec` défile.
- Sortie audio dans un `.wav` (`wav_io` existe déjà).

**Validation** : un replay enregistré se rejoue jusqu'au bout, mêmes compteurs de draws/frames qu'attendu, son audible
dans le WAV.

### Phase 2 — Backend Vulkan (≈ 8 à 12 jours, le plus gros morceau)

1. **Squelette** : instance, device, file graphique, `create_target` / `resize_target` / `release_target`, upload de
   textures RGBA + génération des mips, samplers (wrap/mirror/clamp, anisotropie).
2. **Émetteur GLSL** : traduire `msl_emitter.cpp` (prélude, structures `VIn/VOut`, uniformes `ViewU/DrawU/LightU`,
   combiners) en GLSL 4.5 ; compilation à l'exécution via glslang/shaderc ; distances de clip pour la world clip.
3. **Pipelines** : `PipeKey` → `VkPipeline` (blend, profondeur reverse-Z, decal avec biais en pente, cull,
   alpha-to-coverage, variantes silhouette/découpe) ; préchauffage en tâche de fond (`warm_pipelines_background`),
   mode asynchrone (`set_async_pipelines`), cache disque.
4. **Passes** : `encode_pass` avec MSAA transitoire et résolution en fin de passe ; multiview pour les deux yeux.
5. **Passes fixes** : carte d'ombre, carte de dessous (« underside »), empreinte de l'eau, AO (`ao_fill` en compute),
   `mip_gamma` en compute ; `env_reduce` seulement si un cube statique est retenu.
6. **Composition et lecture** : `encode_composite`, `encode_resolve` vers une image externe, `read_target` /
   `read_target_depth` pour les tests.
7. **Harnais Linux visuel** : fenêtre SDL2, caméra libre (porter `free_fly.cpp` et `room.cpp`), table virtuelle,
   capture PNG (stb_image_write à la place d'ImageIO).

**Validation** :
- Comparaison d'images avec le harnais macOS d'origine sur une série de vues fixes (même replay, même caméra) :
  écart visuel faible, mêmes nombres de draws.
- Validation layers Vulkan sans erreur.
- Aucune compilation de pipeline sur le thread de rendu après le préchauffage.

### Phase 3 — Build Android (≈ 2 à 3 jours)

- Toolchain NDK (r27 ou plus récent), ABI `arm64-v8a`, `minSdk 29`, `targetSdk 32` (exigence Quest).
- Sur Android, exclure les mêmes sources que pour visionOS (`audio_sdl`, `controller_sdl`, `update_checker`,
  `crash_handler`) ; miniaudio compilé sans backend inutilisé.
- Loader OpenXR Khronos pour Android (paquet Maven `org.khronos.openxr:openxr_loader_for_android`).
- `AndroidManifest.xml` : catégorie `com.oculus.intent.category.VR`, `android.hardware.vr.headtracking`,
  `com.oculus.supportedDevices = quest2|quest3|quest3s`, permissions `com.oculus.permission.USE_SCENE`
  (et `USE_ANCHOR_API`), fonctionnalités passthrough.
- Gestion du cycle de vie Android (pause, reprise, perte de focus) → `ar_session_state`.
- Logs via `__android_log_print`, récupérables par `adb logcat`.

**Validation** : l'APK s'installe et démarre ; le jeu tourne (logs, audio) même si rien n'est encore affiché.

### Phase 4 — Plateforme OpenXR, affichage VR (≈ 4 à 5 jours, premiers tests sur casque)

- Session OpenXR avec liaison Vulkan (`XR_KHR_vulkan_enable2`), espace `STAGE` ou `LOCAL_FLOOR`.
- Swapchains couleur et profondeur (`XR_KHR_composition_layer_depth`, utile au reprojection du compositeur).
- Implémenter `wait_frame` / `begin_frame` / `end_frame` / `session_state` de `ar_platform`, avec les vues
  (poses, FOV) et le near plane en reverse-Z infini.
- Fréquence d'affichage (`XR_FB_display_refresh_rate`), niveaux CPU/GPU (`XR_EXT_performance_settings`),
  fovéation fixe (`XR_FB_foveation`, `XR_FB_swapchain_update_state`).
- Table provisoire : posée à 70 cm devant le joueur, à hauteur fixe.

**Validation** : Mario s'affiche en stéréo, stable, à 72 Hz sur Quest 2 et 90 Hz sur Quest 3 dans un niveau simple
(château extérieur), sans erreur du compositeur.

### Phase 5 — Passthrough et placement sur la table (≈ 4 à 6 jours)

- **Passthrough** (`XR_FB_passthrough`) : calque passthrough sous le calque de projection, fond transparent ;
  implémenter `set_passthrough`.
- **Table** : lire les entités de scène (`XR_FB_scene`, `XR_FB_spatial_entity`, label sémantique `TABLE`), en déduire
  `ar_table` (pose + demi-dimensions). Remplacer l'interface de `vos_planes` par cette source.
- **Placement manuel** : rayon de la manette sur le plan (réutiliser `vos_placement` et `vos_place_gesture`),
  aperçu via `get_preview`, validation à la gâchette ou bouton A.
- **Persistance** : ancre spatiale (`XR_FB_spatial_entity_storage`) → `persist_anchor` ; la table retrouve sa place au
  relancement. `share_anchor` laissé à `NULL` (demande les services de la plateforme Meta).
- **Repli** : pas de room setup → proposer de le lancer (`xrRequestSceneCaptureFB`) ou placer à la main sur le sol.
- **Quest 3 seulement, optionnel** : occlusion par la profondeur (`XR_META_environment_depth`).

**Validation** : sur les deux casques, le diorama se pose sur une vraie table détectée, ne dérive pas quand on tourne
autour, et revient au même endroit après redémarrage de l'app.

### Phase 6 — Entrées (≈ 2 jours)

- **Manettes Touch** via actions OpenXR, profil `oculus/touch_controller`. Proposition de mapping :

  | N64 | Touch |
  |---|---|
  | Stick | Stick gauche |
  | A / B | A / B (manette droite) |
  | Z | Gâchette gauche |
  | R | Gâchette droite |
  | C-boutons | Stick droit |
  | Start | Menu (manette gauche) |

- **Manette Bluetooth** (Xbox, PlayStation…) via les événements d'entrée Android → `ar_pad` ; prioritaire si connectée.
- Remplacer `gc_pad.mm` ; réutiliser `pad_map.c` et `vos_input.cpp`.
- Retour haptique (`xrApplyHapticFeedback`) en option.
- Optionnel : suivi des mains (`XR_EXT_hand_tracking`) pour le placement et le panneau.

**Validation** : niveau jouable de bout en bout aux deux types de manette ; pas de latence perceptible.

### Phase 7 — Audio spatial (≈ 3 jours)

- Sortie bas niveau via **Oboe/AAudio** (faible latence), alimentée par `audio_ring`.
- Spatialisation : **OpenAL Soft** avec HRTF à la place de PHASE ; réutiliser `vos_scene_audio.cpp`,
  `vos_scene_pos.cpp`, `vos_resample.cpp`, `vos_polyphase.cpp` (déjà portables) pour positionner les voies
  (`sfx_lanes`) dans le diorama.
- Repli stéréo simple si la spatialisation échoue (comme le repli RemoteIO actuel).
- Alternative à évaluer : Meta XR Audio SDK (meilleure HRTF, mais binaire propriétaire).

**Validation** : les sons viennent du diorama et suivent la tête ; aucun craquement à 72/90 Hz.

### Phase 8 — Panneau de réglages en MR (≈ 3 à 4 jours)

- Recréer les 4 onglets (Niveau, Monde, Look, Audio) avec **Dear ImGui**, rendus dans une swapchain affichée en
  calque quad OpenXR (`XrCompositionLayerQuad`), positionné par `ar_float_panel_anchor` (déjà dans le runtime).
- Interaction au rayon de la manette ; ouverture/fermeture par le bouton Menu.
- Brancher directement sur l'API C `arvos_*` (à renommer `ar_app_*` et déplacer hors de `apps/visionos`).
- Sauvegarde des réglages dans le stockage de l'app ; ajout d'un réglage « Profil » (Auto / Quest 2 / Quest 3 / Perso).

**Validation** : chaque réglage s'applique en direct et persiste après relancement, comme sur Vision Pro.

### Phase 9 — Données : ROM, textures, packs (≈ 2 à 3 jours)

- **La ROM n'est jamais dans l'APK distribué** : `scripts/quest_install.py` la pousse avec `adb push` dans le dossier
  de l'app (`/sdcard/Android/data/<paquet>/files/`) ; vérification de la somme de contrôle au démarrage.
- Les packs HD et modèles 3D suivent le même chemin (préparés sur le PC par `packs.py`, puis poussés).
- **Compression ASTC** des textures de packs au moment de la préparation (encodeur `astcenc`, licence Apache 2),
  avec mipmaps ; ajouter au backend Vulkan un chemin d'upload compressé (nouvelle méthode optionnelle de
  `IRenderDevice`, l'upload RGBA reste par défaut).
- Budget mémoire par profil, mesuré avec `pack_costs.py` et `mem_stats`.

**Validation** : sur Quest 2, mémoire stable sous le budget avec textures HD ; sur Quest 3, tous les packs actifs.

### Phase 10 — Optimisation et réglage des profils (≈ 4 à 6 jours, itératif)

- Mesures sur casque avec **OVR Metrics Tool** (fps, temps GPU/CPU, frames ratées) et **RenderDoc Meta Fork** ou
  **Android GPU Inspector** pour les passes lourdes.
- Leviers connus : nombre de passes de rendu (une seule passe multiview si possible), taille des cartes d'ombre,
  AO bakée une fois par salle, fovéation, échelle de rendu, réduction des changements de pipeline.
- Ajuster les valeurs du tableau de la section 2 à partir des mesures, niveau par niveau (les niveaux les plus lourds :
  Bowser, Tick Tock Clock, Rainbow Ride).

**Validation** : aucune frame ratée en continu dans les 15 niveaux principaux, au profil par défaut de chaque casque.

### Phase 11 — Finition et documentation (≈ 2 jours)

- `scripts/quest_build.py` guidé, sur le modèle de `build.py` : vérifie NDK/SDK, demande la ROM et les packs,
  compile, installe via `adb`, lance l'app.
- README Quest : activation du mode développeur, installation, room setup, mapping des manettes, dépannage.
- Tests de non-régression : le build visionOS et le harnais macOS compilent toujours (CI GitHub Actions macOS + Linux).

---

## 5. Calendrier indicatif

| Phase | Durée | Casque nécessaire |
|---|---|---|
| 0. Préparation | 1 j | Non |
| 1. Harnais headless | 2 j | Non |
| 2. Backend Vulkan + harnais visuel | 8–12 j | Non |
| 3. Build Android | 2–3 j | Oui (démarrage) |
| 4. OpenXR, affichage VR | 4–5 j | Oui |
| 5. Passthrough et table | 4–6 j | Oui |
| 6. Entrées | 2 j | Oui |
| 7. Audio spatial | 3 j | Oui |
| 8. Panneau de réglages | 3–4 j | Oui |
| 9. Données et ASTC | 2–3 j | Oui |
| 10. Optimisation | 4–6 j | Oui |
| 11. Finition | 2 j | Oui |
| **Total** | **≈ 37–49 jours de travail** | |

Ces durées comptent le développement ; le temps réel dépend surtout de la rapidité des allers-retours de test sur le
casque (installer l'APK, essayer, renvoyer les logs). Un premier jalon visible (Mario en stéréo sur Quest, phase 4)
arrive après environ 17 à 23 jours.

---

## 6. Organisation des tests

- **Sans casque (dans l'environnement de développement)** : harnais Linux, rendu logiciel Vulkan (lavapipe/SwiftShader)
  pour les comparaisons d'images, replays `sarec` déterministes.
- **Avec casque (côté utilisateur)** :
  1. Mode développeur activé (application Meta Horizon sur téléphone), câble USB-C ou `adb` sans fil.
  2. `python3 scripts/quest_install.py` installe l'APK, la ROM et les packs.
  3. `python3 scripts/quest_logs.py` récupère le journal d'exécution et les statistiques de perf à renvoyer.
- Le Meta XR Simulator n'existe pas sous Linux : il pourra servir en complément sur un PC Windows ou un Mac si disponible.

---

## 7. Risques et parades

| Risque | Impact | Parade |
|---|---|---|
| Backend Vulkan plus long que prévu (fidélité du combiner N64, effets) | Retard | Comparaisons d'images automatiques dès la phase 2 ; effets avancés désactivables. |
| Saccades à la compilation des pipelines | Confort | Préchauffage au chargement de niveau, cache disque persistant, mode asynchrone existant. |
| Performances insuffisantes sur Quest 2 | Qualité | Profil Quest 2 sobre ; fréquence 72 Hz ; packs de modèles désactivés. |
| Table non détectée (room setup absent ou incomplet) | Usage | Placement manuel au rayon, invitation à lancer le room setup. |
| Passthrough gris du Quest 2 | Rendu de l'effet « maquette » | Limite matérielle : à documenter, rien à corriger. |
| Évolutions des extensions Meta (dépréciation de `XR_FB_*` au profit de `XR_META_*`) | Maintenance | Couche `xr_scene.cpp` isolée ; tester la présence des extensions au démarrage. |
| Mémoire des packs HD | Plantages | Budget par profil, ASTC, saut du mip 0 sur Quest 2. |

---

## 8. Cadre légal

- Le projet est sous **AGPL-3.0** : le fork Quest reste AGPL et son code source doit être publié avec tout binaire
  distribué.
- **Aucune donnée Nintendo** dans le dépôt ni dans l'APK distribué : la ROM et les packs restent sur la machine de
  l'utilisateur et sont poussés sur son casque.
- Distribution par **sideload uniquement** (mode développeur, SideQuest) : une publication sur le Meta Horizon Store
  n'est pas envisageable pour un projet qui utilise la propriété intellectuelle de Nintendo.

---

## 9. Prochaine étape concrète

Démarrer les phases 0 et 1 : rendre le build indépendant de macOS et faire tourner le jeu en headless sur Linux.
Elles valident le socle (jeu, adaptateur, runtime) avant d'attaquer le backend Vulkan.
