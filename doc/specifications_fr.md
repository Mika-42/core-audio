# MKA Core Audio

## 1. Objectif

MKA Core Audio est une couche d’abstraction audio temps réel destinée à fournir une interface unifiée avec différents systèmes et backends audio.

Elle masque les API spécifiques aux backends tout en fournissant une interface simple, prévisible et indépendante du backend aux couches supérieures.

Core Audio est responsable du transport de l’audio entre l’application et le système audio sous-jacent. Il ne traite ni n’interprète le signal audio.

## 2. Objectifs principaux

Core Audio doit :

* fournir une interface commune aux différents backends audio ;
* énumérer les périphériques disponibles ;
* exposer leurs capacités ;
* permettre à l’application de choisir une configuration de stream ;
* ouvrir et gérer les streams audio ;
* fournir des buffers audio temps réel ;
* rester indépendant de toute API de backend particulière ;
* garantir un comportement déterministe et compatible avec les contraintes temps réel.

Core Audio doit rester petit, stable et prévisible.

## 3. Modèle conceptuel

Core Audio repose sur trois concepts principaux :

```text
Backend
   │
   ├── Devices
   │
   └── Stream
        ├── Input Channels
        └── Output Channels
```

### Device

Un device représente une ressource ou un point d'accès audio sélectionnable exposé par un backend.

Il ne représente pas nécessairement du matériel physique. Le backend traduit ses propres concepts vers le modèle générique `Device`.

### Stream

Un stream représente une session audio active ouverte avec une configuration précise pour un device.

La configuration du stream est immuable pendant toute sa durée de vie.

Modifier la configuration nécessite de fermer le stream actuel et d'en ouvrir un nouveau.

### Channel

Un channel représente un signal audio transporté par un stream.

Les concepts propres aux backends, tels que les channels PCM d’ALSA, les ports de PipeWire ou les éléments de WASAPI/CoreAudio, ne doivent pas apparaître dans l’interface publique de Core Audio.

## 4. Découverte des périphériques et de leurs capacités

La découverte des périphériques et l’ouverture d’un stream sont deux opérations distinctes.

L’application commence par récupérer les périphériques disponibles :

```cpp
getDevices()
```

Elle peut ensuite récupérer les capacités d’un périphérique donné :

```cpp
getCapabilities(device)
```

Les capacités peuvent notamment décrire :

* les fréquences d’échantillonnage supportées ;
* les formats audio supportés ;
* le nombre de channels d’entrée et de sortie supportés ;
* les tailles de buffer supportées ou leurs limites ;
* les autres contraintes nécessaires à l’ouverture d’un stream.

Les capacités décrivent ce que le backend est capable de fournir. Elles ne constituent pas une configuration de stream.

## 5. Configuration du stream

C’est l’application qui choisit la configuration souhaitée.

Par exemple :

```cpp
StreamConfig {
    device,
    sampleRate,
    bufferSize,
    format,
    inputChannels,
    outputChannels
}
```

Cette configuration constitue une demande explicite adressée au backend.

### Règle de configuration stricte

`open(config)` doit demander **exactement** la configuration spécifiée.

Core Audio ne doit pas modifier, approximer ou négocier automatiquement la configuration demandée.

Si le backend ne peut pas fournir cette configuration, `open()` doit échouer avec une erreur appropriée.

Par exemple :

```text
Demandé :
    48000 Hz
    Float32
    256 frames
    2 channels d'entrée
    2 channels de sortie

Backend :
    Compatible

Résultat :
    OPEN
```

Si la configuration n'est pas supportée :

```text
Demandé :
    48000 Hz
    Float32
    256 frames

Backend :
    Configuration non supportée

Résultat :
    ERROR
```

Le backend ne doit pas remplacer silencieusement la configuration par une autre, par exemple `44100 Hz` ou `512 frames`.

Cela garantit que la configuration réellement utilisée est connue et contrôlée par l’application.

L’application peut utiliser `getCapabilities()` avant `open()` afin de vérifier que la configuration demandée est supportée.

Le backend doit néanmoins effectuer une nouvelle validation lors de `open()`, car les capacités peuvent avoir changé entre leur découverte et l’ouverture du stream.

## 6. Cycle de vie d’un stream

Le cycle de vie minimal est :

```text
CLOSED
   │ open()
   ▼
OPEN
   │ start()
   ▼
RUNNING
   │ stop()
   ▼
OPEN
   │ close()
   ▼
CLOSED
```

L’interface publique minimale est :

```cpp
getDevices()
getCapabilities(device)

open(config)
start()
stop()
close()
```

La configuration ne peut pas être modifiée pendant qu’un stream est ouvert.

Une modification de configuration nécessite :

```text
stop()
   ↓
close()
   ↓
open(newConfig)
   ↓
start()
```

Tout mécanisme de hot-swap ou de reconfiguration transparente est de la responsabilité de l’`AudioEngine`, et non de Core Audio.

## 7. Buffer audio

Core Audio fournit les buffers audio aux composants de niveau supérieur.

Un buffer générique peut être représenté ainsi :

```cpp
struct AudioBuffer {
    float* const* input;
    float* const* output;

    uint32_t inputChannels;
    uint32_t outputChannels;
    uint32_t frames;
};
```

La représentation interne peut varier selon les backends.

Core Audio est responsable de traduire les données audio natives du backend vers la représentation générique.

Une conversion de format peut être effectuée lorsque cela est nécessaire à l’abstraction.

Core Audio ne doit pas imposer une taille fixe de bloc DSP. Le nombre de frames fourni par le backend peut varier.

La gestion des blocs DSP de taille fixe appartient au DSP Engine.

## 8. Contraintes temps réel

Le callback audio doit être compatible avec les contraintes temps réel.

Le chemin d’exécution temps réel doit notamment éviter :

* les allocations dynamiques ;
* les désallocations ;
* les mutex bloquants ;
* l’accès au système de fichiers ;
* les opérations réseau ;
* les appels système potentiellement bloquants ;
* les opérations dont la durée d’exécution n’est pas bornée ;
* les logs lourds ;
* toute opération présentant une latence imprévisible.

Chaque implémentation de backend doit également respecter les contraintes temps réel qui lui sont propres.

## 9. Responsabilités de Core Audio

Core Audio est responsable de :

* l’énumération des périphériques ;
* la découverte de leurs capacités ;
* la configuration des streams ;
* la création des streams ;
* leur cycle de vie ;
* le transport des données audio ;
* l’abstraction des buffers audio ;
* l’abstraction et, si nécessaire, la conversion des formats audio ;
* la remontée d’erreurs indépendante du backend ;
* la gestion du callback temps réel ;
* l’isolation des implémentations backend.

## 10. Hors périmètre

Core Audio ne doit pas gérer :

* le traitement DSP ;
* la planification des blocs DSP ;
* les plugins ;
* VST/AU ;
* les mixeurs ;
* les graphes audio ;
* les nodes ;
* les tracks ;
* les bus ;
* le routage logique audio ;
* le transport ;
* la timeline ;
* l’automation ;
* le MIDI ;
* la gestion des projets ;
* l’interface utilisateur ;
* le réseau ;
* la logique métier du DAW.

Le routage audio physique ou virtuel appartient à une couche audio de niveau supérieur.

Le traitement et la planification DSP appartiennent au DSP Engine.

## 11. Séparation des responsabilités

L’architecture suit le principe suivant :

```text
DAW Engine
    ↓
DSP Engine
    ↓
Audio Routing
    ↓
Audio Engine
    ↓
Core Audio
    ↓
Backend
    ↓
Système d’exploitation / Matériel
```

Les responsabilités sont séparées ainsi :

**Core Audio**

> Abstrait le backend et transporte l’audio.

**Audio Engine**

> Orchestre l’exécution audio temps réel et les opérations audio de niveau supérieur.

**Audio Routing**

> Détermine comment les channels physiques et virtuels sont connectés.

**DSP Engine**

> Traite l’audio et gère l’exécution DSP.

**DAW**

> Détermine ce qui doit être traité.

## 12. Indépendance vis-à-vis des backends

Core Audio ne doit exposer aucun type ou concept spécifique à un backend.

L’interface publique doit utiliser des concepts génériques tels que :

```cpp
DeviceID
DeviceInfo
DeviceCapabilities
StreamConfig
AudioBuffer
SampleFormat
Error
```

Les implémentations peuvent utiliser en interne :

* ALSA ;
* PipeWire ;
* JACK ;
* WASAPI ;
* CoreAudio ;
* ou toute autre API native.

Ces détails doivent rester isolés du reste de l’application.

## 13. Gestion des erreurs

Les erreurs doivent être explicites et déterministes.

Exemples :

```cpp
None
InvalidArgument
NotFound
AlreadyExists
UnsupportedConfiguration
DeviceOpenFailed
HardwareSetupFailed
PollSetupFailed
PollDescriptorsFailed
WouldBlock
XRun
```

En particulier, une configuration de stream non supportée doit produire une erreur explicite plutôt qu’une modification automatique de la configuration demandée.

## 14. Critères d’acceptation

Une implémentation de Core Audio est conforme si elle :

* énumère les périphériques disponibles ;
* expose leurs capacités ;
* permet à l’application de construire une configuration explicite ;
* valide cette configuration lors de l’ouverture ;
* n’ouvre le stream qu’avec la configuration demandée ;
* échoue explicitement lorsque cette configuration n’est pas supportée ;
* supporte `open/start/stop/close` ;
* fournit des buffers audio temps réel ;
* n’impose pas de taille fixe aux blocs DSP ;
* respecte les contraintes temps réel ;
* n’expose aucun concept spécifique à un backend dans son interface publique ;
* permet à plusieurs backends d’implémenter la même interface ;
* impose la recréation du stream lorsque sa configuration change.

## 15. Principe architectural

> **Core Audio doit rester petit, stable, déterministe et prévisible.**

Le backend transporte l’audio.

Core Audio abstrait le backend.

L’Audio Engine orchestre l’exécution temps réel.

Le DSP Engine traite l’audio.

Le DAW décide de ce qui doit être traité.

Toute fonctionnalité qui n’est pas directement liée à la découverte des périphériques, à la découverte de leurs capacités, à la configuration des streams, à leur cycle de vie ou au transport audio temps réel appartient à un composant de niveau supérieur.
