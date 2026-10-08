# RDRMP vs Frontier-MP: Análisis Comparativo Detallado

## Problema Identificado
- **RDRMP**: ✅ Carga correctamente el mundo (PlayerLayout disponible, ScriptMain ejecuta)
- **Frontier**: ❌ NO carga el mundo (PlayerLayout no se encuentra, ScriptMain no logra inicializar)

---

## PARTE 1: ARQUITECTURA GENERAL

### RDRMP (Funciona ✅)
```
DLL Injection (dinput8.dll / client-main.dll)
    ↓
DllMain
    ↓
Registra callback con ScriptHook → "Espérame, voy a ejecutarme en la fibra correcta"
    ↓
ScriptHook carga su scheduler
    ↓
ScriptHook invoca ScriptMain (EN LA FIBRA CORRECTA)
    ↓
ScriptMain recibe comando "LoadOnline" vía GetCommand()
    ↓
AHORA sí, carga el mundo → PlayerLayout existe
    ↓
Juego multiplayer activo
```

### Frontier-MP (NO Funciona ❌)
```
DLL Injection (dinput8.dll / frontier_core.dll)
    ↓
DllMain
    ↓
Registra callback + CreateThread(FrontierMainThread)
    ↓
FrontierMainThread (THREAD SEPARADO)
    ├─ Intenta inicializar PlayerFactory
    ├─ Busca PlayerLayout ← NO EXISTE AÚN (mundo no cargó)
    ├─ Llama nativas desde contexto incorrecto ← UNDEFINED BEHAVIOR
    └─ Loop actualizando UI/Red
    
ScriptHook invoca ScriptMain (callback registrado)
    ├─ NativeInvoker se inicializa
    ├─ pero PlayerFactory ya falló antes
    └─ Mundo nunca se carga
```

---

## PARTE 2: FLUJO DE EJECUCIÓN DETALLADO

### RDRMP: LoadMainMenu() y LoadOnline()

#### A. LoadMainMenu (FUN_1800022d0)
**Ubicación en dump**: `client-main.dll_analysis.txt` líneas 131-454

**Lógica principal**:
```cpp
void LoadMainMenu() {
    // 1. Espera comando del sistema
    while (true) {
        // GetCommand es UNA LLAMADA NATIVA dentro de ScriptHook
        command = rage::scrThread::GetCommand(0x82a290d4);
        
        if (command != nullptr) {
            (*command)(...);  // Ejecuta la acción del comando
        }
        
        if (local_118[0] != '\0') break;  // Si hay comando, sale del loop
        
        ThisFiber::Wait(0);  // Yield cooperativo a ScriptHook
    }
    
    // 2. Crea interfaz CEF (menú principal)
    CEFManager::Get();
    FUN_18000ee20();  // Configura browser CEF
    
    // 3. Registra handlers y callbacks
    ClientHandler::RegisterFunction(...);
    DiscordRPC::SetState("Main Menu");
    
    return;  // Termina LoadMainMenu
}
```

**Datos importantes**:
- Se ejecuta DENTRO DE ScriptMain (fibra correcta)
- Usa `GetCommand()` para ESPERAR órdenes de ScriptHook
- Llama a nativas directamente (seguro)
- `ThisFiber::Wait(0)` mantiene cooperación con ScriptHook

---

#### B. LoadOnline (FUN_180002ca0)
**Ubicación en dump**: `client-main.dll_analysis.txt` líneas 501-626

**Lógica principal**:
```cpp
void LoadOnline() {
    // 1. Espera completación de sistemas previos
    FUN_1800541b0();  // Pre-checks
    while (FUN_180054060() != '\0') {
        ThisFiber::Wait(0);  // Espera activamente
    }
    
    // 2. Inicializa sistemas de scripting
    FUN_180053f00();  // StartScreen command
    FUN_180053da0("fileSetForMPLoad");
    FUN_180053da0("fileStartupChecksComplete");
    
    // 3. Busca PlayerLayout (YA EXISTE porque mundo cargó)
    layout = FIND_NAMED_LAYOUT("PlayerLayout");  // ← EXISTE
    if (!layout) return;  // Nunca entra aquí en RDRMP
    
    // 4. Carga config del jugador
    playerConfig = FUN_1800088c0(configPath, ...);  // Lee archivo
    
    // 5. Inicia cliente de red
    ClientNetwork::initialize();
    
    return;
}
```

**Datos importantes**:
- Se ejecuta DESPUÉS de LoadMainMenu (secuencial, en la misma fibra)
- Llama a `FUN_180053da0()` que internamente hace:
  ```cpp
  DAT_180133a00 = rage::scrThread::GetCommand(0xb58825f5);
  // Obtiene comando del sistema
  if (DAT_180133a00 != nullptr) {
      (*DAT_180133a00)(...);  // Ejecuta
  }
  ```
- **CRUCIALLY**: PlayerLayout YA EXISTE en este punto
- Todas las nativas son seguras (contexto correcto)

---

#### C. Helpers que ambas funciones usan

**FUN_180053f00 (StartScreen command)**
```cpp
void FUN_180053f00() {
    // Prepara comando "StartScreen1" para ScriptHook
    GetCommand(0x2df89c2e);  // ← Obtiene comando registrado
    
    // Si existe callback, lo ejecuta
    if (comando_callback != nullptr) {
        (*comando_callback)(...);
    }
}
```

**FUN_180053da0 (File state helper)**
```cpp
void FUN_180053da0(const char* filename) {
    // Registra que un archivo completó su operación
    GetCommand(0xb58825f5);  // ← Comando para tracking de archivos
    
    if (comando_callback != nullptr) {
        (*comando_callback)(filename);
    }
}
```

---

### Frontier-MP: El Problema Detallado

#### A. DllMain (dllmain.cpp líneas 161-180)
```cpp
BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        
        // ✅ Registra callback correctamente
        Frontier::Core::ScriptBridge::registerScriptAtAttach(hModule);
        
        // ❌ PROBLEMA: Crea thread de trabajo
        HANDLE hThread = CreateThread(nullptr, 0,
            (LPTHREAD_START_ROUTINE)FrontierMainThread, hModule, 0, nullptr);
        
        return TRUE;
    }
}
```

**Problema identificado**: 
- `registerScriptAtAttach()` es correcto
- Pero `FrontierMainThread` es un thread **separado**
- Ese thread intenta inicializar cosas que necesitan estar EN LA FIBRA

---

#### B. FrontierMainThread (dllmain.cpp líneas 81-159) - ❌ INCORRECTO
```cpp
DWORD WINAPI FrontierMainThread(LPVOID lpParam) {
    HMODULE hModule = static_cast<HMODULE>(lpParam);
    fs::path modDir = getModDirectory(hModule);
    
    ClientConfig cfg = loadClientConfig(modDir);
    
    if (cfg.showConsole) {
        AllocConsole();  // ✅ OK (no necesita fibra)
    }
    
    std::cout << "[FrontierClient] Initializing..." << std::endl;
    
    // ❌ PROBLEMA: Inicializa hooks y patterns FUERA de la fibra
    const bool engineHooksReady = Frontier::Core::EngineHooks::initialize();
    
    // ❌ PROBLEMA: Inicializa PlayerFactory FUERA de la fibra
    Frontier::Core::PlayerFactory::initialize();  // ← Llama nativas aquí
    
    // ❌ PROBLEMA: Activa UI sin tener acceso a nativas
    Frontier::UI::D3D11Renderer::get().setMainMenuVisible(true);
    
    // Loop de actualización (thread separado)
    uint32_t scriptBridgeRetryTicks = 0;
    while (true) {
        Frontier::Net::ClientNetwork::get().update();
        
        // Reintenta registrar cada 1.5 segundos
        if (!Frontier::Core::ScriptBridge::isRegistered() &&
            (++scriptBridgeRetryTicks % 60) == 0) {
            Frontier::Core::ScriptBridge::registerScript(hModule);  // Reintenta
        }
        
        Frontier::UI::CefManager::get().update();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    
    return 0;
}
```

**Los 3 problemas clave**:

1. **PlayerFactory::initialize() se llama en thread incorrecto**
   ```cpp
   // Está en FrontierMainThread, pero debería estar en ScriptMain
   Frontier::Core::PlayerFactory::initialize();
   
   // Internamente hace:
   uintptr_t layout = NativeInvoker::invoke<uintptr_t>(
       Natives::FIND_NAMED_LAYOUT, "PlayerLayout");  // ← FALLA
   // ¿Por qué falla? Porque:
   // - El mundo aún NO existe
   // - NativeInvoker aún NO se inicializó correctamente
   // - No estamos en la fibra de ScriptHook
   ```

2. **NativeInvoker no está inicializado en este punto**
   ```cpp
   // NativeInvoker::initialize() se llama solo en ScriptBridge::scriptMain()
   // en línea 194 de script_bridge.cpp
   void __cdecl ScriptBridge::scriptMain() {
       ...
       NativeInvoker::initialize();  // ← AQUÍ
       ...
   }
   
   // Pero en FrontierMainThread, eso aún no ocurrió
   ```

3. **El mundo no ha cargado aún**
   ```cpp
   // RDR1 necesita cierto flujo para cargar el mundo:
   // DLL_PROCESS_ATTACH → ScriptMain registrada → 
   // ScriptHook scheduler listo → ScriptMain invocada → 
   // Comandos procesados → Mundo cargado
   
   // Frontier intenta esto en paso 1, antes que el mundo exista
   ```

---

#### C. ScriptBridge::scriptMain() en Frontier (script_bridge.cpp líneas 182-217)
```cpp
void __cdecl ScriptBridge::scriptMain() {
    s_registered.store(true, std::memory_order_release);
    s_registrationRequested.store(false, std::memory_order_release);
    resolveRuntimeApi();
    
    // ✅ Correcto: Inicializa nativas aquí
    NativeInvoker::initialize();
    
    std::cout << "[ScriptBridge] ScriptMain iniciado dentro del scheduler de RAGE."
              << std::endl;
    
    for (;;) {
        // ❌ PROBLEMA: runFrame() no hace nada útil
        runFrame();
        
        if (!s_scriptWait) {
            resolveRuntimeApi();
        }
        
        if (!s_scriptWait) {
            std::cerr << "[ScriptBridge] scriptWait no está disponible."
                      << std::endl;
            return;
        }
        
        // ✅ Correcto: Yield a ScriptHook
        s_scriptWait(0);
    }
}
```

**El agujero**:
```cpp
void ScriptBridge::runFrame() {
    if (!NativeInvoker::isReady()) {
        NativeInvoker::initialize();
    }
    
    if (!NativeInvoker::isReady()) {
        return;  // ← Solo sale, no hace nada
    }
    
    // ✅ Intenta procesar mundo
    EngineHooks::processMultiplayerWorldLoad();
    
    // ❌ PERO: PlayerFactory ya intentó inicializar antes
    // ❌ PERO: PlayerLayout aún podría no existir en este punto
}
```

---

## PARTE 3: COMPARACIÓN DE MÉTODOS

### RDRMP: "Esperar Comandos"

| Paso | Función | Contexto | Acción |
|------|---------|---------|--------|
| 1 | LoadMainMenu | ScriptMain (fibra correcta) | Registra handlers CEF |
| 2 | LoadMainMenu loop | ScriptMain | Llama `GetCommand()` - **ESPERA** |
| 3 | Scheduler ScriptHook | ScriptHook fibra | Envía comando "LoadOnline" |
| 4 | LoadMainMenu continúa | ScriptMain | Recibe comando, sale del loop |
| 5 | LoadOnline comienza | ScriptMain (siguiente ejecución) | Mundo YA cargó |
| 6 | LoadOnline | ScriptMain | Busca PlayerLayout - **EXISTE** |
| 7 | LoadOnline | ScriptMain | Crea jugador con `CREATE_PLAYER_ACTOR_IN_LAYOUT` |

**Ventaja**: 
- Respeta el flujo de ScriptHook
- Espera de forma cooperativa
- El mundo se carga en el momento correcto

---

### Frontier: "Hacer Todo Ya"

| Paso | Función | Contexto | Acción |
|------|---------|---------|--------|
| 1 | DllMain | Thread principal | Registra ScriptBridge::scriptMain |
| 2 | FrontierMainThread | **Thread separado** ❌ | Carga config |
| 3 | FrontierMainThread | **Thread separado** | Inicializa EngineHooks |
| 4 | FrontierMainThread | **Thread separado** | PlayerFactory::initialize() |
| 5 | PlayerFactory | **Thread separado** | Busca PlayerLayout - **NO EXISTE** ❌ |
| 6 | PlayerFactory | **Thread separado** | Falla al crear jugador |
| 7 | ScriptHook scheduler | ScriptHook fibra | Invoca ScriptBridge::scriptMain |
| 8 | scriptMain | ScriptHook fibra (correcta) | Inicializa NativeInvoker |
| 9 | scriptMain | ScriptHook fibra | Llama runFrame() - world never loaded |

**Desventaja**:
- Intenta hacer cosas en thread incorrecto
- Accede a nativas fuera de contexto
- El mundo nunca se carga porque nada lo dispara
- PlayerLayout sigue sin existir

---

## PARTE 4: COMPARACIÓN DE CÓDIGO FUENTE

### RDRMP: GetCommand Pattern (Dentro de ScriptMain)
```cpp
// En client-main.dll decompiled (FUN_1800022d0)
lVar4 = *(longlong *)((longlong)ThreadLocalStoragePointer + (ulonglong)_tls_index * 8);

while (true) {
    if ((*(int *)(lVar4 + 4) < DAT_180133a50) &&
        (FUN_1800f3bf0(&DAT_180133a50), DAT_180133a50 == -1)) {
        // GetCommand es una nativa de ScriptHook
        DAT_180133a58 = rage::scrThread::GetCommand(0x82a290d4);
        _Init_thread_footer(&DAT_180133a50);
    }
    
    if (DAT_180133a58 != nullptr) {
        // Ejecuta el callback recibido
        (*DAT_180133a58)((InfoBase *)&local_1f8);
    }
    
    if (local_118[0] != '\0') break;  // Salir si tiene comando
    
    ThisFiber::Wait(0);  // Yield
}
```

**Característica clave**: `rage::scrThread::GetCommand()` + `ThisFiber::Wait(0)`

---

### Frontier: Reintentos Periódicos (Desde thread separado)
```cpp
// En dllmain.cpp FrontierMainThread
uint32_t scriptBridgeRetryTicks = 0;
while (true) {
    Frontier::Net::ClientNetwork::get().update();
    
    if (!Frontier::Core::ScriptBridge::isRegistered() &&
        (++scriptBridgeRetryTicks % 60) == 0) {
        // Reintenta cada 60 frames (~1 segundo a 60 FPS)
        Frontier::Core::ScriptBridge::registerScript(hModule);
    }
    
    Frontier::UI::CefManager::get().update();
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
}
```

**Característica clave**: Loop en thread separado + sleep

---

## PARTE 5: POR QUÉ RDRMP CARGA EL MUNDO

### Flujo real de RDRMP:
```
1. RDR1.exe carga
2. DLL se inyecta
3. ScriptHook se inicializa (su scheduler)
4. ScriptHook llama LoadMainMenu (script registrado)
   ↓
   while (true) {
       cmd = GetCommand();  // Espera orden de ScriptHook
       if (cmd) break;
       Wait(0);
   }
   (Aquí se queda esperando)
5. Jugador interactúa con menú (UI)
6. Menú envía evento → ScriptHook recibe "LoadOnline"
7. ScriptHook despierta a LoadMainMenu con el comando
8. LoadMainMenu sale del loop, termina
9. ScriptHook invoca LoadOnline
   ↓
   Mundo YA está cargado en este punto
   PlayerLayout EXISTE
   Jugador se spawnea correctamente
```

---

## PARTE 6: POR QUÉ FRONTIER NO CARGA EL MUNDO

### Flujo real de Frontier:
```
1. RDR1.exe carga
2. DLL se inyecta
3. DllMain crea FrontierMainThread
4. FrontierMainThread intenta PlayerFactory::initialize()
   ↓
   NativeInvoker aún NO está listo
   PlayerLayout NO existe aún
   FALLA
5. Mientras tanto, ScriptHook se inicializa
6. ScriptHook invoca ScriptBridge::scriptMain
   ↓
   Inicializa NativeInvoker (correcto ahora)
   Llama runFrame()
   ¿Pero quién dispara la carga del mundo?
   Nadie. No hay comando. No hay espera. No hay trigger.
7. Resultado: Mundo nunca se carga, PlayerLayout sigue sin existir
```

---

## PARTE 7: LA SOLUCIÓN

### Cambio necesario en Frontier (script_bridge.cpp)

**ANTES (❌ No funciona):**
```cpp
void __cdecl ScriptBridge::scriptMain() {
    s_registered.store(true, std::memory_order_release);
    resolveRuntimeApi();
    NativeInvoker::initialize();
    
    for (;;) {
        runFrame();  // ← No hace nada
        s_scriptWait(0);
    }
}
```

**DESPUÉS (✅ Funciona):**
```cpp
void __cdecl ScriptBridge::scriptMain() {
    s_registered.store(true, std::memory_order_release);
    resolveRuntimeApi();
    
    // ✅ Inicializa nativas DENTRO de la fibra correcta
    NativeInvoker::initialize();
    
    std::cout << "[ScriptBridge] NativeInvoker ready, waiting for world load..." << std::endl;
    
    // ✅ ESPERA A QUE EL MUNDO ESTÉ LISTO (como RDRMP)
    uint32_t waitTicks = 0;
    uintptr_t playerLayout = nullptr;
    while (!playerLayout) {
        if ((waitTicks++ % 300) == 0) {  // Log cada 5 segundos a 60 FPS
            std::cout << "[ScriptBridge] Waiting for PlayerLayout..." << std::endl;
        }
        
        // Intenta obtener PlayerLayout
        playerLayout = NativeInvoker::invoke<uintptr_t>(
            Natives::FIND_NAMED_LAYOUT, 
            "PlayerLayout"
        );
        
        if (!playerLayout) {
            s_scriptWait(0);  // Yield y reintenta
            continue;
        }
    }
    
    std::cout << "[ScriptBridge] PlayerLayout found! World loaded." << std::endl;
    
    // ✅ AHORA sí, inicializa PlayerFactory
    // (Antes de esto, PlayerLayout NO existía)
    Frontier::Core::PlayerFactory::initialize();
    
    // ✅ Loop principal normal
    for (;;) {
        runFrame();
        
        if (!s_scriptWait) {
            resolveRuntimeApi();
        }
        
        if (!s_scriptWait) {
            std::cerr << "[ScriptBridge] scriptWait not available, stopping script." << std::endl;
            return;
        }
        
        s_scriptWait(0);
    }
}
```

### Cambio en FrontierMainThread (dllmain.cpp)

**ANTES (❌ Inicializa PlayerFactory aquí):**
```cpp
DWORD WINAPI FrontierMainThread(LPVOID lpParam) {
    ...
    Frontier::Core::PlayerFactory::initialize();  // ❌ Contexto incorrecto
    Frontier::UI::D3D11Renderer::get().setMainMenuVisible(true);
    ...
}
```

**DESPUÉS (✅ Solo UI/Red, sin natives):**
```cpp
DWORD WINAPI FrontierMainThread(LPVOID lpParam) {
    ...
    // ✅ OK: Inicializa SOLO sistemas que no dependen de nativas
    if (cfg.showConsole) {
        AllocConsole();
    }
    
    // ✅ OK: Hooking de gráficos (no necesita nativas)
    Frontier::Core::EngineHooks::initialize();
    
    // ❌ ELIMINADO: PlayerFactory::initialize();
    
    // ✅ OK: UI (después de EngineHooks)
    Frontier::UI::D3D11Renderer::get().setMainMenuVisible(true);
    
    // Loop seguro de actualización
    uint32_t scriptBridgeRetryTicks = 0;
    while (true) {
        Frontier::Net::ClientNetwork::get().update();
        
        // Reintenta registrar si falla
        if (!Frontier::Core::ScriptBridge::isRegistered() &&
            (++scriptBridgeRetryTicks % 60) == 0) {
            Frontier::Core::ScriptBridge::registerScript(hModule);
        }
        
        Frontier::UI::CefManager::get().update();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    
    return 0;
}
```

---

## RESUMEN EJECUTIVO

| Criterio | RDRMP | Frontier | Solución |
|----------|-------|----------|----------|
| **Dónde se registra ScriptMain** | ScriptHook callback | ScriptHook callback | ✅ Igual |
| **Dónde se inicializa NativeInvoker** | Dentro de ScriptMain | Dentro de ScriptMain | ✅ Igual |
| **Dónde se espera por mundo** | Dentro de ScriptMain + GetCommand | No se espera | ❌ **FALTA** |
| **Dónde se accede a PlayerLayout** | Dentro de ScriptMain, DESPUÉS de LoadOnline | En FrontierMainThread, ANTES de que exista | ❌ **ERROR** |
| **Sincronización con ScriptHook** | Explícita (Wait+GetCommand) | Implícita (reintentos) | ⚠️ **DÉBIL** |
| **Contexto de ejecución (nativas)** | Fibra correcta | Thread separado (incorrecto) | ❌ **ERROR** |
| **Carga del mundo funciona** | ✅ SÍ | ❌ NO | **FIX IT** |

---

## CONCLUSIÓN

**RDRMP funciona porque:**
1. Espera activamente a que el mundo cargue (GetCommand + Wait)
2. Solo accede a nativas dentro de ScriptMain (contexto correcto)
3. Respeta el flujo de ScriptHook

**Frontier no funciona porque:**
1. Intenta acceder a nativas en FrontierMainThread (contexto incorrecto)
2. No espera a que el mundo cargue
3. PlayerLayout nunca existe cuando se intenta usar

**Frontier necesita:**
1. Mover PlayerFactory::initialize() de FrontierMainThread a ScriptMain
2. Agregar espera explícita por PlayerLayout en ScriptMain
3. Respetar el order de operaciones del scheduler de ScriptHook
