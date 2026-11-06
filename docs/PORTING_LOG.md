# Journal du portage Quest

Ce journal suit, phase par phase, l'exécution du plan [`PORTAGE_QUEST.md`](PORTAGE_QUEST.md) : ce qui a été fait, comment
cela a été vérifié, ce qui s'écarte du plan et ce qui reste ouvert. Il est mis à jour à la fin de chaque phase.

Branche de travail : `claude/phase-1-plan-y3v1g2`.

| Phase | État | Commits |
|---|---|---|
| 0. Préparation | Faite, vérifiée sous Linux | `aa3b0b3` |
| 1. Harnais headless | Faite, vérifiée | `9f54f3f` |
| 2. Backend Vulkan + harnais visuel | Faite, vérifiée sur lavapipe ; comparaison avec Metal à faire sur un Mac | `3486d5f`, `f8a370b` |
| 3. Build Android | Faite et compilée ; **pas encore lancée sur un casque** | `becc115` |
| 4 à 11 | À faire | — |

---

## Environnement de développement

Toutes les phases ont été menées dans un conteneur Linux x86-64 (Ubuntu 24.04), sans GPU, sans écran et sans casque.

- Compilateurs : gcc 13.3 et clang (les deux sont testés à chaque phase), CMake 3.28.3, Ninja, Python 3.13.
- Paquets ajoutés : `libsdl2-dev`, `libglew-dev`, `libcurl4-openssl-dev`, `zlib1g-dev`, `bsdextrautils` (pour `hexdump`),
  `libvulkan-dev`, `mesa-vulkan-drivers` (lavapipe : Vulkan logiciel), `glslang-dev`, `glslang-tools`,
  `libshaderc-dev`, `glslc`, `spirv-tools`, `vulkan-validationlayers`, `vulkan-tools`, `xvfb`, `xauth`.
- Android : SDK dans `/opt/android-sdk` (hors du dépôt), NDK 27.3.13750724, plateformes 32 et 34, build-tools 34.0.0,
  platform-tools ; Java 21, Gradle 8.14.3. La licence du SDK Android a été acceptée avec l'accord de l'utilisateur.
- ROM : la version européenne fournie au départ (MD5 `45676429…`) n'est pas acceptée par sm64coopdx ; la ROM US
  (MD5 `20b854b2…`, format `.n64`) a été convertie en `.z64` dans `local/baserom.us.z64` (dossier ignoré par git).

---

## Phase 0 — Préparation

**Objectif du plan** : compiler le jeu, l'adaptateur et le runtime sous Linux, sans Xcode ni Homebrew.

### Fait

- **Sous-module** `games/sm64/upstream` (sm64coopdx) récupéré au commit épinglé `8cd6e597…`.
- **CMake indépendant de macOS** :
  - `CMakeLists.txt` : `project()` en C/C++ seulement, `OBJC`/`OBJCXX` activés uniquement si `APPLE` ; le harnais macOS
    n'est ajouté que sur Apple.
  - `cmake/Toolchain.cmake` : la vérification de Xcode 26 ne s'applique qu'aux cibles Apple ; nouvelle variable
    `SM64AR_LINUX` (et, en phase 3, `SM64AR_ANDROID`).
  - `runtime/CMakeLists.txt` : `ar_render_metal`, `ar_png`, `ar_gc_pad`, `ar_harness` réservés à Apple.
  - `games/sm64/CMakeLists.txt` : hors Apple, le jeu est compilé « headless » comme pour visionOS (stub SDL, sans
    `audio_sdl`, `controller_sdl`, `update_checker`, `crash_handler`), sans `OSX_BUILD`, lié à `z`, threads, `dl`, `m`.
- **SHA-256 portable** : `sm64_pack_stamp.c` utilisait CommonCrypto (Apple). Ajout de
  `games/sm64/adapter/src/sha256_portable.{c,h}` (FIPS 180-4) utilisé hors Apple ; CommonCrypto reste sur Apple.
- **Scripts** :
  - `scripts/sm64ar_common.py` : Xcode et Homebrew exigés seulement sur macOS ; sur Linux, vérification des commandes et
    modules pkg-config, avec la commande `apt` à lancer (`missing_dependencies`).
  - `scripts/sm64_oracle.py` : sur Linux, compile le jeu desktop upstream avec `make` (les ressources `lang/`, `mods/`,
    `palettes/`, `dynos/` et les fichiers générés en dépendent).
  - `scripts/fetch_lua.py` : nouvelles tentatives du téléchargement (2, 4, 8 s) ; lua.org coupait la connexion.
  - `scripts/packs.py` : `sips` (macOS) remplacé par Pillow pour réencoder et redimensionner les PNG des packs.
  - Nouveau `scripts/linux_build.py` (option `--clang`) : toute la chaîne jusqu'à `build/linux`.
- README : section « Linux build ».

### Vérifié

- Depuis un dépôt vierge, avec gcc et avec clang : les 14 patches s'appliquent, `sm64_game`, `sm64_adapter`, `ar_core`
  et les autres bibliothèques portables (`ar_render_common`, `ar_vos_logic`, `ar_platform_fake`…) compilent.
- Une édition de liens d'essai de toute l'archive du jeu et de l'adaptateur ne laisse aucun symbole non résolu.
- Le SHA-256 portable donne le même résultat que `sha256sum` sur des tailles limites (0, 1, 55, 56, 63, 64, 65, 119,
  120, 1000, 100000 octets).
- La réécriture Pillow de `packs.py` a été testée sur des PNG synthétiques (redimensionnement, palette, fichier non PNG
  refusé), pas sur un vrai pack.

### Écarts

- Pas de branche `quest-port` : le travail est sur la branche imposée par la session.
- Dépendance supplémentaire sous Linux : `hexdump` (paquet `bsdextrautils`), utilisé par le Makefile upstream.

---

## Phase 1 — Harnais Linux headless

**Objectif du plan** : faire tourner le jeu sans fenêtre ni GPU, rejouer un enregistrement `sarec`, sortir le son en WAV.

### Fait

- `runtime/render/null/null_device.{h,cpp}` : un `IRenderDevice` qui ne dessine rien mais compte passes, draws, vertices
  et textures envoyées ; `read_target` rend une image vide de la bonne taille.
- `runtime/platform/headless/headless_platform.{h,cpp}` : une plateforme `ar_platform` portable (horloge, pose de la tête
  et manette fixées par l'appelant, table fixe), équivalent de `HarnessPlatform` sans Metal ni GameController.
- `runtime/platform/headless/headless_session.{h,cpp}` : équivalent portable de `HarnessSession` (runtime, renderer
  commun, couche AR) dessinant dans n'importe quel `IRenderDevice`.
- `apps/linux_headless` → exécutable `sm64ar_headless` : lecture et enregistrement de replays (hash d'état vérifié à
  chaque tick), manette scriptée (`--pad TICK:BOUTONS[:LX,LY]`, `--pad-file`), WAV (`--audio-out`), compteurs de draws
  par frame (`--stats-every`), événements de jeu (`--story-events`), mêmes options de configuration de partie que le
  harnais macOS (`--level`, `--start`, `--game-arg`).
- `apps/linux_headless/walk.pad` : un script de manette (marche, course, sauts, coup de poing).
- `scripts/headless_check.py` : enregistre `walk.pad`, rejoue dans une sauvegarde vierge et compare tout.
- Correction du build : GNU ld lit chaque archive une seule fois ; hors Apple, les stubs (`link_stubs*.c`) forment une
  archive `sm64_link_stubs` liée en entier après `sm64_game`.

### Vérifié (ROM US)

- Un enregistrement de 840 ticks se rejoue jusqu'au bout : 840 ticks rejoués, 840 hash vérifiés, aucune divergence.
- Les 600 frames ont exactement les mêmes nombres de draws, vertices et passes à l'enregistrement et au replay.
- Le WAV du replay est identique octet pour octet à celui de l'enregistrement (20 s, pic 15752 sur 32767 : son audible).
- Une partie sans entrée aboutit à un autre état final : la manette scriptée agit bien sur le jeu.
- Le binaire clang atteint le même hash final que le binaire gcc.
- Mode histoire (écran titre → sélection de fichier → château) et stéréo fonctionnent ; un replay tronqué est signalé
  (`ended_early`).

---

## Phase 2 — Backend Vulkan et harnais visuel

**Objectif du plan** : un `IRenderDevice` Vulkan fidèle au backend Metal, et un harnais Linux visuel.

### Fait

- `runtime/render/vulkan/glsl_emitter.{h,cpp}` : traduction en GLSL 4.50 de `msl_emitter.cpp` : mêmes blocs d'uniformes
  (layouts std140 identiques aux structures MSL : ViewU 160 octets, DrawU 80, LightU 704), mêmes combiners N64,
  éclairage de pièce, ombres PCF, AO, matériau « toy », liquides, découpe (cutaway), transitions, encre ; plus les
  shaders des passes de profondeur, de composition et de résolution.
- `runtime/render/vulkan/vk_device.{h,cpp}` : `IRenderDevice` sur Vulkan 1.1 + `VK_KHR_create_renderpass2` (cœur en 1.2) :
  - passes enregistrées par cible puis encodées dans une seule render pass, comme sur Metal ;
  - reverse-Z, stencil pour les silhouettes, biais de profondeur des decals ;
  - viewport inversé (hauteur négative) pour garder la convention Metal (NDC +y vers le haut, face avant CCW) ;
  - MSAA avec résolution couleur et profondeur (`VK_KHR_depth_stencil_resolve`, mode MAX si disponible),
    attachements transitoires (`LAZILY_ALLOCATED`) sur les GPU à tuiles ;
  - cartes d'ombre, de dessous de table et d'empreinte de l'eau ;
  - AO : cartes de profondeur dans un tableau 2D, réduites par un compute shader `ao_fill` en volume 3D (R8, ou flottant
    à défaut) ;
  - composition, résolution vers une image externe (`VulkanExternalImage`, prévue pour les swapchains OpenXR),
    relecture couleur et profondeur ;
  - shaders compilés à l'exécution par shaderc ; pipelines indexés comme sur Metal (plus l'état profondeur/cull quand
    `VK_EXT_extended_dynamic_state` manque), construits à la demande ou par des threads de préchauffage, mode
    asynchrone, `VkPipelineCache` sauvegardé sur disque, listes de pipelines compatibles avec celles de Metal ;
  - deux frames en vol, destruction différée des ressources ;
  - création possible sur un device fourni par l'appelant (`create_vulkan_device_on`, pour OpenXR).
- `runtime/platform/headless/png_write.{h,cpp}` (stb_image_write de l'arbre upstream).
- `sm64ar_headless` : `--device vulkan`, `--png-at`, `--out`, `--look original|roomlit`, `--look-set KEY=VALUE`,
  `--msaa`, `--validation`, `--pipelines` (liste préchauffée), `--pipeline-cache`, AO dans le résumé.
- `apps/linux_window` → `sm64ar_window` : fenêtre SDL2, caméra libre (W/A/S/D/Q/E, clic droit), orbite, stéréo, capture,
  jeu à la manette ou au clavier, enregistrement optionnel ; l'image Vulkan est relue puis affichée par SDL.
- `apps/common/sm64_launch.{h,cpp}` : configuration de partie partagée par les harnais (et, depuis la phase 3, l'app Quest).
- `scripts/compare_images.py` : PSNR et images de différence entre deux séries de PNG (pour Vulkan contre Metal).

### Vérifié (lavapipe, couche de validation activée)

- Bob-omb Battlefield s'affiche correctement en look Original N64 et en look éclairé (ombres portées, ombre de contact,
  relighting, AO), en mono et en stéréo, MSAA 1 et 4 ; Lethal Lava Land aussi (lave, HUD sur la table).
- Aucune erreur de validation ; un seul avertissement de performance sans conséquence (variable de sortie du vertex
  shader non lue par certains fragment shaders).
- Après préchauffage d'une liste de pipelines : `draw_time_builds=0` (aucune compilation sur le thread de rendu).
- Un même replay rendu deux fois donne des PNG identiques octet pour octet.
- AO : un calcul par niveau, sans échec ; comparé à AO désactivée, 0,5 % des pixels changent (PSNR 46 dB), dans les
  zones abritées.
- Harnais fenêtré : tourne sous Xvfb (environ 8 images/s sur lavapipe), capture correcte.
- Builds gcc et clang ; la vérification de la phase 1 passe toujours.

### Non vérifié, à faire

- **Comparaison avec le harnais macOS** : impossible sans Mac. Les commandes à lancer des deux côtés sont dans
  l'en-tête de `scripts/compare_images.py`.
- **Jolly Roger Bay** apparaît flottant au-dessus de la table, à grande échelle : placement normal du niveau ou défaut du
  backend, à trancher par la comparaison avec Metal.
- La résolution vers une image externe est écrite mais n'a jamais été exécutée (phase 4).
- Performances : non mesurables sur lavapipe (phase 10).

### Reporté

- Rendu multiview / cibles en couches : phase 4 (le stéréo passe aujourd'hui par deux vues côte à côte).
- Mipmaps corrigés en gamma (`mip_gamma`) : ne servent qu'avec les packs HD, phase 9.
- Reflets de la sonde d'environnement (`env_reduce`, cube) : pas de sonde sur Quest ; désactivés.

---

## Phase 3 — Build Android

**Objectif du plan** : un APK qui s'installe et démarre sur Quest ; le jeu tourne (logs, son) même sans image.

### Fait

- **CMake** : `SM64AR_ANDROID` ; le jeu est compilé comme sous Linux, miniaudio réduit à son backend nul, logs vers
  `liblog`.
- **Patch upstream `0015-Build-with-the-Android-NDK.patch`** : `bcopy`/`bzero` pris dans `<strings.h>` sur bionic,
  `stop_thread` sans `pthread_cancel` sur Android (aucun appelant), `<string.h>` manquant dans
  `controller_bind_mapping.c`. La série de 15 patches se réapplique proprement.
- **Projet Gradle** `apps/quest` : AGP 8.13.2, wrapper Gradle 8.14.3, `arm64-v8a`, `minSdk 29`, `targetSdk 32`,
  `c++_shared`, NativeActivity sans code Java, loader OpenXR `org.khronos.openxr:openxr_loader_for_android:1.1.54`
  (prefab), ressources du jeu copiées en assets avec un index, lint désactivé en release (pas de code Java ; ses
  dépendances étaient refusées par Maven Central).
- **Manifeste** : catégorie `com.oculus.intent.category.VR`, `android.hardware.vr.headtracking`, passthrough requis,
  Vulkan 1.1 requis, `USE_SCENE`, `USE_ANCHOR_API`, `HAND_TRACKING`, permissions et requêtes du loader OpenXR,
  `com.oculus.supportedDevices = quest2|quest3|quest3s`, `focusaware`.
- **Code natif** `apps/quest/native` (`libdiorama64.so`) :
  - copie des ressources vers le stockage interne au premier lancement (et à chaque nouvel index) ;
  - ROM lue dans le dossier externe de l'app (`/sdcard/Android/data/diorama64.quest/files/baserom.us.z64`) ;
  - jeu sur le runtime avec la plateforme portable et le renderer nul (pas encore d'image) ;
  - son stéréo via AAudio (32 kHz demandés ; un écart de fréquence est signalé dans le journal) ;
  - cycle de vie Android → état de session (le jeu se fige en pause, l'horloge ne compte pas le temps de pause) ;
  - stdout/stderr redirigés vers logcat ; sonde OpenXR qui liste les extensions du runtime utiles aux phases 4 et 5 ;
  - ligne d'état toutes les 5 s (ticks, draws, textures, son).
- **Scripts** : `quest_build.py` (vérifie SDK, NDK, Java, CMake, ninja ; prépare le jeu ; lance Gradle ; `--release`,
  `--install`), `quest_install.py` (installation adb avec `-g`, conversion et vérification SHA-1 de la ROM, envoi, lancement),
  `quest_logs.py` (logcat filtré, sauvegarde dans un fichier), `quest_common.py`.
- `HeadlessPlatform` accepte un état de session ; `apps/linux_common` renommé `apps/common`.
- README : section « Meta Quest 2 / 3 ».

### Vérifié (sans casque)

- `python3 scripts/quest_build.py --release` depuis un dépôt vierge : APK de 83 Mo, signé (clé de debug), contenant
  `libdiorama64.so`, `libopenxr_loader.so`, `libc++_shared.so` et les 420 fichiers de ressources ; l'APK debug aussi.
- La sonde OpenXR est bien compilée (paquet prefab trouvé).
- Tous les symboles de `libdiorama64.so` se résolvent avec les bibliothèques système de l'API 29.
- `aapt2 dump badging` : paquet `diorama64.quest`, `sdkVersion` 29, `targetSdkVersion` 32, fonctionnalités et permissions
  attendues.
- Les scripts d'installation et de logs signalent proprement l'absence de casque.
- Pas de régression Linux (build, vérification de la phase 1, rendu Vulkan).

### Non vérifié, à faire sur le casque

- Installation, démarrage, copie des ressources, démarrage du jeu, son et journal : rien de tout cela n'a tourné (ni
  casque ni émulateur : pas de KVM).
- Commandes : `quest_build.py`, `quest_install.py`, `quest_logs.py --save local/quest.log` ; renvoyer le journal.

### Notes pour la phase 4

- shaderc a été compilé pour Android (`build/android-deps/shaderc`, `libshaderc_combined`) avec `c++_static` ; il faudra
  le recompiler avec `c++_shared`, comme l'app.
- Le backend Vulkan n'est pas encore lié dans l'APK ; il faudra l'y ajouter avec shaderc.

---

## Prochaine étape

Phase 4 : plateforme OpenXR (session Vulkan, swapchains couleur et profondeur, boucle de frames, fréquence, fovéation),
avec les premiers essais sur le casque.
