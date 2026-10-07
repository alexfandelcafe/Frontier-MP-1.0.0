# Frontier-MP — CONTEXT FOR NEXT CHAT

## Fecha
2026-10-07

## Objetivo actual
Hacer que el cliente/launcher descargue y prepare automáticamente ScriptHookRDR para que FrontierMP pueda usar la API pública de ScriptHookRDR sin exigir que el usuario instale manualmente la DLL.

## Repositorio
- GitHub: alexfandelcafe/Frontier-MP-1.0.0
- Branch: main

## Diagnóstico confirmado
El cliente llegaba al juego pero no encontraba/cargaba ScriptHookRDR:
- Ruta esperada: D:\Red Dead Redemption\ScriptHookRDR.dll
- Error de Windows: GetLastError=126
- También fallaba el intento junto a frontier_core.dll.

El error 126 se producía antes de resolver exports: no había ScriptHookRDR.dll disponible para LoadLibraryEx.

## Evidencia externa verificada
- ScriptHookRDR 1.5.2 es la versión publicada más reciente visible actualmente del ScriptHookRDR oficial para la versión PC/remaster de RDR1.
- La página de Nexus identifica 1.5.2 y documenta que cada .asi registrado recibe su propio script thread.
- Una guía actualizada el 20 de junio de 2026 publica mirrors de 1.5.2; el proyecto está usando como fuente de descarga un mirror Dropbox del archivo:
  ScriptHookRDR-164-1-5-2-1738573417.zip
- No se debe subir ni redistribuir el binario dentro del repositorio. El launcher lo descarga en runtime y conserva una caché local.

Fuentes:
- https://www.nexusmods.com/reddeadredemption/mods/164
- https://gamedecide.com/scripthookrdr/

## Cambios recientes
### Native/script bridge
La arquitectura actual ya no usa sm_CommandsRegistration para invocar natives.
Se usa la API pública de ScriptHookRDR:
- scriptRegister(HMODULE, void(*)())
- scriptWait(DWORD)
- nativeInit(uint64_t)
- nativePush64(uint64_t)
- nativeCall() -> uint64_t*

Los exports pueden estar decorados por MSVC x64, por lo que ScriptBridge/NativeInvoker intentan resolver nombres decorados y también nombres simples.

La ejecución de natives quedó restringida al script thread de ScriptHookRDR:
1. ScriptBridge registra ScriptMain con scriptRegister.
2. ScriptMain llama runFrame().
3. runFrame() inicializa NativeInvoker si hace falta.
4. EngineHooks::processMultiplayerWorldLoad() ejecuta la transición.
5. scriptWait(0) cede al scheduler de RAGE.

El hilo persistente de Frontier ya NO llama directamente processMultiplayerWorldLoad().

### Commits relevantes anteriores
- 3921f53e420c9ed092fa89bf3cb62da1d17b058b — Restore multiplayer world load implementation
- 1dc4927c322587232091115fe27872260089f7c7 — Defer ScriptHook registration until client thread startup
- eac7d10dd79f851e41ff75d8077253ddf9a22695 — Use ScriptHookRDR native invocation API instead of internal command table
- b58eaeea1a31f5e18d6cc5aa65768448d66e9713 — native invoker implementation using ScriptHook API
- a0df774a2d4c11f9f8f3acb456e34587e39b1b39 — Stop persistent loop from directly executing world-load natives
- 188b29f25ab87c11e4a8641f35af671f7c31efdd — Align ScriptBridge with native invoker API
- 184c5e0cf09809d8a6c6dc3a8c5f9846f3ab4ce0 — Added runtime LoadLibrary diagnostics for ScriptHookRDR

### Bootstrap de ScriptHookRDR
Nuevos archivos:
- client/launcher/src/dependency_bootstrap.hpp
- client/launcher/src/dependency_bootstrap.cpp

Commits:
- 997fa31e47f43d48d9fb7a8cbbc4ffe20a564a9e
- ddf0b7bb42f0741a351269e80031f89392a44215
- aa925690eab2ec331dbcd28dc3c5495b715d8e6e
- 5c449b7badb44eaf26abb22b4c997aff9feb458e
- 96d10ee00bdaa850254dcafbca8ebbfcdd192105
- ad2be8eb31773b12838918f992ae7b36e5121b60

La implementación final actual:
1. Comprueba si gameDir/ScriptHookRDR.dll ya existe y tiene tamaño razonable.
2. Si falta, crea:
   launcherDir/dependencies/scripthookrdr/1.5.2/
3. Descarga automáticamente el ZIP 1.5.2 desde el mirror Dropbox configurado.
4. Guarda primero como .part.
5. Extrae mediante PowerShell Expand-Archive.
6. Busca ScriptHookRDR.dll dentro del ZIP.
7. Guarda una copia en la caché.
8. Copia ScriptHookRDR.dll al directorio del juego.
9. Crea scripthookrdr.runtime.txt con la versión/ruta.
10. NO instala ni sobrescribe dinput8.dll. Frontier carga ScriptHookRDR.dll explícitamente mediante LoadLibraryEx, y así se evita modificar el loader ASI del usuario.

El launcher aborta antes de CreateProcess si ScriptHookRDR no queda disponible.

## CMake actual
client/CMakeLists.txt ahora agrega:
- launcher/src/dependency_bootstrap.cpp
- urlmon en target_link_libraries(frontier_launcher ...)

MinHook continúa funcionando mediante vendor/minhook si existe o FetchContent en caso contrario.

## Flujo esperado en el próximo test
Al lanzar Frontier:
[Launcher] Descargando ScriptHookRDR 1.5.2...
[Launcher] ScriptHookRDR 1.5.2 listo en: D:\Red Dead Redemption\ScriptHookRDR.dll
[Launcher] ScriptHookRDR instalado en el directorio del juego.
[Launcher] Launching RDR.exe suspended...

Después, dentro del juego:
[ScriptBridge] ScriptHookRDR.dll ya estaba cargado...
o:
[ScriptBridge] ScriptHookRDR.dll cargado desde el directorio del juego: D:\Red Dead Redemption\ScriptHookRDR.dll
[ScriptBridge] Export count: ...
[NativeInvoker] ScriptHook native API lista. ...
[ScriptBridge] Hilo de script FrontierMP registrado en ScriptHookRDR.
[ScriptBridge] ScriptMain iniciado dentro del scheduler de RAGE.

## Próximo diagnóstico si falla
1. Si falla la descarga: registrar HRESULT de URLDownloadToFileA.
2. Si descarga pero no extrae: registrar exit code de PowerShell.
3. Si ScriptHookRDR.dll existe pero LoadLibraryEx sigue devolviendo 126:
   - comprobar si el problema es una dependencia de ScriptHookRDR;
   - registrar arquitectura PE x64 y errores de carga;
   - no volver a tocar sm_CommandsRegistration.
4. Si registra el script pero no aparece ScriptMain:
   - verificar la firma/resolución de scriptRegister/scriptWait.
5. Si ScriptMain funciona pero nativeCall falla:
   - usar frontier_native_crash.log;
   - verificar cada export y la firma real de nativePush64/nativeCall antes de cambiar hashes.
6. Si nativeCall funciona pero la transición no cambia de frontend:
   - revisar los hashes/argumentos de EngineHooks y contrastarlos con el código descomprimido de RDRMP.
   - La investigación anterior NO pudo leer el repositorio privado/no indexado DLL-descomprimidos; no inventar resultados sobre ese repositorio.

## Estado de los hashes
client/core/include/core/native_hashes.hpp mantiene:
- MULTIPLAYER_LOAD_PREPARE = 0xB0B4296A
- MULTIPLAYER_LOAD_READY_CHECK = 0xE5CC6F08
- FILE_SET_FOR_MP_LOAD = 0xB58825F5
- START_SCREEN_1 = 0x2DF89C2E
- FIND_NAMED_LAYOUT = 0x489E5F81
No cambiar estos hashes sin evidencia adicional. Una búsqueda histórica externa identificó 0xB58825F5 como UI_SEND_EVENT en otro header público, pero eso NO prueba que el RDRMP transition mapping sea incorrecto.

## Limitaciones
No se puede ejecutar aquí el build de Visual Studio/Windows ni comprobar el launcher en la PC del usuario. La validación final requiere que el usuario configure/compile y ejecute el launcher en Windows.

## Nota importante
ScriptHookRDR es un binario de terceros. El diseño deliberadamente no lo embebe en el repositorio ni lo descarga durante cada arranque una vez que está en caché. Solo se obtiene cuando falta.
