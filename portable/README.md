# L2 D3D12 Portable

Inicializador experimental para clientes **Lineage II C4, Interlude y derivados x86** que usan Direct3D 9.

No convierte Unreal Engine 2 en un motor DX12 nativo. Coloca una capa `d3d9.dll` junto al cliente y solicita al componente **D3D9On12 de Windows** que traduzca D3D9 a D3D12. El cliente y su logica de CPU siguen siendo los originales.

## Uso normal

1. Ejecuta `Start-L2D3D12.cmd`.
2. Selecciona la carpeta completa del servidor o su carpeta `system`.
3. El programa inspecciona `L2.exe` y todos los EXE/DLL de `system`, prueba D3D9On12 con la GPU y solo entonces instala el proxy.
4. El cliente se inicia con `INTERLUDE_D3D_BACKEND=9on12` y se genera evidencia en `sessions`.

Para volver a usar DX12 en sesiones futuras, inicia el juego desde `Start-L2D3D12.cmd`. Abrir `L2.exe` directamente no activa la variable de backend y el proxy usa D3D9 nativo como opcion segura.

Para un launcher concreto dentro de la carpeta del cliente:

```powershell
.\Start-L2D3D12.ps1 -ClientPath 'D:\Juegos\MiL2' -LaunchExecutable 'MiLauncher.exe'
```

Para comprobar compatibilidad sin modificar el cliente:

```powershell
.\Start-L2D3D12.ps1 -ClientPath 'D:\Juegos\MiL2' -InspectOnly
```

Para instalar sin iniciar:

```powershell
.\Start-L2D3D12.ps1 -ClientPath 'D:\Juegos\MiL2' -InstallOnly
```

Para quitarlo, usa `Remove-L2D3D12.cmd` o:

```powershell
.\Start-L2D3D12.ps1 -ClientPath 'D:\Juegos\MiL2' -Remove
```

## Limites deliberados

- Solo admite clientes PE32/x86. Los clientes x64 requieren otro binario.
- Si ya existe `system\d3d9.dll` y no pertenece a este paquete con manifiesto y SHA-256 coincidentes, **no lo sobrescribe**. Puede ser ReShade, dxwrapper o una proteccion del servidor.
- Si el cliente importa funciones D3D9 no implementadas por este proxy, se detiene antes de modificar nada.
- Un anticheat puede rechazar DLL locales o comprobar hashes. Este paquete no intenta eludirlo.
- D3D9On12 puede mejorar compatibilidad con herramientas modernas y permitir escenarios de frame generation compatibles, pero no garantiza mas FPS. La traduccion tambien puede agregar sobrecarga.
- AFMF depende del driver AMD, la GPU, el modo de pantalla y la configuracion del perfil. Que el log confirme DX12 no confirma por si solo que AFMF este activo.

## Que significa el resultado

- `Confirmed9On12Device`: el dispositivo real del cliente expuso `IDirect3DDevice9On12`; DX12 quedo confirmado.
- `Initialized9On12Object`: se creo el objeto D3D9On12, pero el juego no alcanzo `CreateDevice` durante la espera.
- `FallbackNative9*`: Windows o el cliente cayeron a D3D9 nativo.
- `NotObserved`: el launcher puede haber abierto otro proceso o el juego aun no llego al renderer.

Cada ejecucion guarda `sessions\<fecha-hora>\session.json`, la salida de la prueba previa y el log del proxy. La desinstalacion solo elimina un `d3d9.dll` cuyo hash coincide con el manifiesto que creo el paquete.
