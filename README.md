# FrontierMP - Red Dead Redemption 1 Multiplayer Framework
Framework multijugador dedicado de código abierto para **Red Dead Redemption 1 (PC)**, con arquitectura modular inspirada en **FiveM / CitizenFX**, transporte UDP con **ENet**, interfaz web acelerada por hardware con **CEF** y scripting en **Lua**.

---

## 🏗️ Arquitectura del Sistema

```
FrontierMP/
├── shared/               # Biblioteca compartida (protocolo, BitStream, tipos y vectores)
├── server/               # Servidor dedicado de alto rendimiento (C++20, ENet, Lua, HTTP)
│   ├── config.toml       # Configuración del servidor (puertos, tickrate, recursos)
│   ├── resources/        # Recursos estilo FiveM (chat, freeroam, nametags, admin)
│   └── src/              # Core del servidor y gestor de entidades/jugadores
└── client/               # Mod cliente para RDR1 PC
    ├── launcher/         # Lanzador e inyector de DLL
    ├── core/             # Hooking de RAGE Engine, Pattern Scanner, Invocador de nativas
    ├── net/              # Cliente de red ENet y sincronización de actores
    ├── ui/               # Integración de Chromium Embedded Framework (CEF) y D3D11
    └── ui_assets/        # Interfaz Web HTML5/CSS/JS (Menú principal y Chat)
```

---

## 🎯 El Problema Resuelto: Ciclo de Vida del Actor (`PlayerLayout`)

A diferencia del modo para un jugador habitual de RDR1 (gobernado por scripts de campaña como `press_start.xsc` o `main.xsc`), FrontierMP resuelve la transición directa de **Frontend a Multijugador** sin colapsos de memoria mediante el siguiente flujo verificado:

1. **Obtención del Layout de Actores:**  
   Localiza el contenedor global `"PlayerLayout"` vía nativa `0x489E5F81` (`FIND_NAMED_LAYOUT`).
2. **Streaming asíncrono del Modelo:**  
   Carga el modelo del personaje (ej. hash `837` para John Marston) con `0xB0A79FEE` (`STREAMING_REQUEST_ACTOR`) y espera la confirmación en `0x7DF72579` (`STREAMING_IS_ACTOR_LOADED`).
3. **Creación del Actor del Jugador Local:**  
   Instancia el actor en las coordenadas dadas usando `0x6A307D5F` (`CREATE_PLAYER_ACTOR_IN_LAYOUT`).
4. **Activación de Control y Cámara:**  
   Asigna el puntero en `rage::sagPlayerMgr::s_LocalPlayer`, activa el control con `SET_PLAYER_CONTROL` y enlaza la cámara de juego.

---

## 📦 Sistema de Recursos (Estilo FiveM)

Cada carpeta dentro de `server/resources/` representa un recurso independiente con su archivo `manifest.toml`:

```toml
name = "freeroam"
description = "Modo Libre para FrontierMP"
version = "1.0.0"

server_scripts = [ "server.lua" ]
client_scripts = [ "client.lua" ]
files = [ "ui/index.html" ]
```

### Eventos Lua Disponibles:
* **`event.register(nombre)`** / **`event.add_handler(nombre, funcion)`**
* **`event.trigger_on_client(nombre, id, ...)`**
* **`event.trigger_on_all_clients(nombre, ...)`**
* **`natives.actor.*`**, **`natives.hud.*`**, **`natives.health.*`**

---

## 🛠️ Compilación

### Requisitos:
* **CMake 3.20+**
* Compilador **C++20** (MSVC en Visual Studio 2022 o Clang/GCC)
* Windows SDK (para compilación del cliente e inyector)

### Comandos de construcción:
```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Generará:
* `server/frontier_server.exe`: El servidor dedicado independiente.
* `client/frontier_launcher.exe`: El ejecutable para lanzar el juego con el mod.
* `client/frontier_core.dll`: La DLL inyectada en RDR1 PC.
