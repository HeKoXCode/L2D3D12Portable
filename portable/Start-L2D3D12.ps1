[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$ClientPath,

    [string]$LaunchExecutable,

    [string[]]$LaunchArguments,

    [switch]$InspectOnly,

    [switch]$InstallOnly,

    [switch]$Remove,

    [ValidateRange(1, 120)]
    [int]$VerifySeconds = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:PackageVersion = '0.1.0'
$script:PortableRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:ProxyPath = Join-Path $script:PortableRoot 'bin\x86\d3d9.dll'
$script:SmokePath = Join-Path $script:PortableRoot 'bin\x86\proxy_smoke_test.exe'
$script:SupportedImports = @(
    'D3DPERF_BeginEvent',
    'D3DPERF_EndEvent',
    'D3DPERF_GetStatus',
    'D3DPERF_QueryRepeatFrame',
    'D3DPERF_SetMarker',
    'D3DPERF_SetOptions',
    'D3DPERF_SetRegion',
    'Direct3DCreate9',
    'Direct3DCreate9Ex',
    '#27', '#28', '#29', '#30', '#31', '#32', '#33', '#37', '#38'
)

function Write-Stage {
    param([string]$Message)
    Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Write-Utf8File {
    param(
        [string]$Path,
        [string]$Text
    )
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Save-JsonFile {
    param(
        [string]$Path,
        [object]$Value
    )
    Write-Utf8File -Path $Path -Text (($Value | ConvertTo-Json -Depth 8) + "`r`n")
}

function Read-U16 {
    param([byte[]]$Bytes, [int]$Offset)
    if ($Offset -lt 0 -or $Offset + 2 -gt $Bytes.Length) {
        throw "Lectura PE fuera de rango (U16 en $Offset)."
    }
    return [BitConverter]::ToUInt16($Bytes, $Offset)
}

function Read-U32 {
    param([byte[]]$Bytes, [int]$Offset)
    if ($Offset -lt 0 -or $Offset + 4 -gt $Bytes.Length) {
        throw "Lectura PE fuera de rango (U32 en $Offset)."
    }
    return [BitConverter]::ToUInt32($Bytes, $Offset)
}

function Read-U64 {
    param([byte[]]$Bytes, [int]$Offset)
    if ($Offset -lt 0 -or $Offset + 8 -gt $Bytes.Length) {
        throw "Lectura PE fuera de rango (U64 en $Offset)."
    }
    return [BitConverter]::ToUInt64($Bytes, $Offset)
}

function Read-AsciiZ {
    param([byte[]]$Bytes, [int]$Offset)
    if ($Offset -lt 0 -or $Offset -ge $Bytes.Length) {
        throw "Cadena PE fuera de rango (en $Offset)."
    }
    $end = $Offset
    $limit = [Math]::Min($Bytes.Length, $Offset + 4096)
    while ($end -lt $limit -and $Bytes[$end] -ne 0) {
        $end++
    }
    if ($end -eq $limit) {
        throw 'Cadena PE sin terminador dentro del limite seguro.'
    }
    return [Text.Encoding]::ASCII.GetString($Bytes, $Offset, $end - $Offset)
}

function Convert-RvaToOffset {
    param(
        [uint32]$Rva,
        [object[]]$Sections,
        [uint32]$SizeOfHeaders
    )
    if ($Rva -lt $SizeOfHeaders) {
        return [int]$Rva
    }
    foreach ($section in $Sections) {
        $span = [Math]::Max([uint32]$section.VirtualSize, [uint32]$section.RawSize)
        if ($Rva -ge $section.VirtualAddress -and
            $Rva -lt ([uint64]$section.VirtualAddress + $span)) {
            return [int]([uint64]$section.RawPointer + ($Rva - $section.VirtualAddress))
        }
    }
    throw ('No se pudo mapear RVA 0x{0:X8}.' -f $Rva)
}

function Get-PeInfo {
    param([string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 256 -or (Read-U16 $bytes 0) -ne 0x5A4D) {
        throw 'No es un archivo PE valido (MZ ausente).'
    }
    $peOffset = [int](Read-U32 $bytes 0x3C)
    if ((Read-U32 $bytes $peOffset) -ne 0x00004550) {
        throw 'No es un archivo PE valido (firma PE ausente).'
    }

    $machine = Read-U16 $bytes ($peOffset + 4)
    $sectionCount = Read-U16 $bytes ($peOffset + 6)
    $optionalSize = Read-U16 $bytes ($peOffset + 20)
    $optionalOffset = $peOffset + 24
    $magic = Read-U16 $bytes $optionalOffset
    if ($magic -ne 0x10B -and $magic -ne 0x20B) {
        throw ('Formato PE opcional no compatible: 0x{0:X4}.' -f $magic)
    }
    $isPe32 = $magic -eq 0x10B
    $directoryOffset = $optionalOffset + $(if ($isPe32) { 96 } else { 112 })
    $importRva = Read-U32 $bytes ($directoryOffset + 8)
    $importSize = Read-U32 $bytes ($directoryOffset + 12)
    $sizeOfHeaders = Read-U32 $bytes ($optionalOffset + 60)

    $sections = @()
    $sectionOffset = $optionalOffset + $optionalSize
    for ($index = 0; $index -lt $sectionCount; $index++) {
        $offset = $sectionOffset + ($index * 40)
        $sections += [pscustomobject]@{
            VirtualSize    = Read-U32 $bytes ($offset + 8)
            VirtualAddress = Read-U32 $bytes ($offset + 12)
            RawSize        = Read-U32 $bytes ($offset + 16)
            RawPointer     = Read-U32 $bytes ($offset + 20)
        }
    }

    $imports = @()
    if ($importRva -ne 0 -and $importSize -ne 0) {
        $descriptorOffset = Convert-RvaToOffset $importRva $sections $sizeOfHeaders
        for ($descriptorIndex = 0; $descriptorIndex -lt 4096; $descriptorIndex++) {
            $current = $descriptorOffset + ($descriptorIndex * 20)
            $originalThunk = Read-U32 $bytes $current
            $nameRva = Read-U32 $bytes ($current + 12)
            $firstThunk = Read-U32 $bytes ($current + 16)
            if ($originalThunk -eq 0 -and $nameRva -eq 0 -and $firstThunk -eq 0) {
                break
            }

            $dllNameOffset = Convert-RvaToOffset $nameRva $sections $sizeOfHeaders
            $dllName = Read-AsciiZ $bytes $dllNameOffset
            $thunkRva = if ($originalThunk -ne 0) { $originalThunk } else { $firstThunk }
            $thunkOffset = Convert-RvaToOffset $thunkRva $sections $sizeOfHeaders
            $symbols = @()
            for ($thunkIndex = 0; $thunkIndex -lt 65536; $thunkIndex++) {
                if ($isPe32) {
                    $thunkValue = [uint64](Read-U32 $bytes ($thunkOffset + ($thunkIndex * 4)))
                    $ordinalMask = [uint64]2147483648
                    $addressMask = [uint64]0x7FFFFFFF
                }
                else {
                    $thunkValue = Read-U64 $bytes ($thunkOffset + ($thunkIndex * 8))
                    $ordinalMask = [uint64]::Parse(
                        '8000000000000000',
                        [Globalization.NumberStyles]::HexNumber)
                    $addressMask = [uint64]0x7FFFFFFFFFFFFFFF
                }
                if ($thunkValue -eq 0) {
                    break
                }
                if (($thunkValue -band $ordinalMask) -ne 0) {
                    $symbols += ('#{0}' -f ($thunkValue -band 0xFFFF))
                }
                else {
                    $symbolRva = [uint32]($thunkValue -band $addressMask)
                    $symbolOffset = Convert-RvaToOffset $symbolRva $sections $sizeOfHeaders
                    $symbols += Read-AsciiZ $bytes ($symbolOffset + 2)
                }
            }
            $imports += [pscustomobject]@{
                Dll     = $dllName
                Symbols = @($symbols)
            }
        }
    }

    $machineName = switch ($machine) {
        0x014C { 'x86' }
        0x8664 { 'x64' }
        default { '0x{0:X4}' -f $machine }
    }
    return [pscustomobject]@{
        Path         = [IO.Path]::GetFullPath($Path)
        Machine      = [int]$machine
        MachineName  = $machineName
        IsPe32       = $isPe32
        Imports      = @($imports)
    }
}

function Select-ClientFolder {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Selecciona la carpeta del cliente Lineage II o su carpeta system'
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        throw 'No se selecciono ningun cliente.'
    }
    return $dialog.SelectedPath
}

function Resolve-ClientLayout {
    param([string]$RequestedPath)

    if ([string]::IsNullOrWhiteSpace($RequestedPath)) {
        $RequestedPath = Select-ClientFolder
    }
    if (-not (Test-Path -LiteralPath $RequestedPath)) {
        throw "La ruta no existe: $RequestedPath"
    }

    $item = Get-Item -LiteralPath $RequestedPath
    $baseDirectory = if ($item.PSIsContainer) { $item.FullName } else { $item.Directory.FullName }
    $candidatePaths = New-Object 'System.Collections.Generic.List[string]'
    $candidatePaths.Add($baseDirectory)
    $candidatePaths.Add((Join-Path $baseDirectory 'system'))
    $parent = Split-Path -Parent $baseDirectory
    if ($parent) {
        $candidatePaths.Add((Join-Path $parent 'system'))
    }
    Get-ChildItem -LiteralPath $baseDirectory -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $candidatePaths.Add($_.FullName)
    }

    $seen = @{}
    $candidates = @()
    foreach ($candidatePath in $candidatePaths) {
        if (-not (Test-Path -LiteralPath $candidatePath -PathType Container)) {
            continue
        }
        $resolved = (Resolve-Path -LiteralPath $candidatePath).Path
        if ($seen.ContainsKey($resolved.ToLowerInvariant())) {
            continue
        }
        $seen[$resolved.ToLowerInvariant()] = $true
        $l2Path = Join-Path $resolved 'L2.exe'
        if (-not (Test-Path -LiteralPath $l2Path -PathType Leaf)) {
            continue
        }
        $score = 10
        foreach ($expected in @('Core.dll', 'Engine.dll', 'D3DDrv.dll')) {
            if (Test-Path -LiteralPath (Join-Path $resolved $expected) -PathType Leaf) {
                $score += 3
            }
        }
        $candidates += [pscustomobject]@{
            SystemDirectory = $resolved
            L2Path          = $l2Path
            Score           = $score
        }
    }
    if ($candidates.Count -eq 0) {
        throw 'No se encontro una carpeta system valida con L2.exe.'
    }
    $ordered = @($candidates | Sort-Object -Property @(
        @{ Expression = 'Score'; Descending = $true },
        @{ Expression = 'SystemDirectory'; Ascending = $true }
    ))
    if ($ordered.Count -gt 1 -and $ordered[0].Score -eq $ordered[1].Score) {
        $paths = ($ordered | ForEach-Object { $_.SystemDirectory }) -join "`r`n  - "
        throw "Se encontraron varios clientes equivalentes. Indica uno de forma explicita:`r`n  - $paths"
    }
    $selected = $ordered[0]
    $leaf = Split-Path -Leaf $selected.SystemDirectory
    $clientRoot = if ($leaf -ieq 'system') {
        Split-Path -Parent $selected.SystemDirectory
    }
    else {
        $selected.SystemDirectory
    }
    return [pscustomobject]@{
        ClientRoot      = $clientRoot
        SystemDirectory = $selected.SystemDirectory
        L2Path          = $selected.L2Path
    }
}

function Test-ClientCompatibility {
    param([object]$Layout)

    $l2Info = Get-PeInfo -Path $Layout.L2Path
    if ($l2Info.Machine -ne 0x014C -or -not $l2Info.IsPe32) {
        throw "L2.exe no es x86/PE32. Arquitectura detectada: $($l2Info.MachineName)."
    }

    $consumers = @()
    $parseWarnings = @()
    $files = Get-ChildItem -LiteralPath $Layout.SystemDirectory -File | Where-Object {
        $_.Extension -ieq '.exe' -or $_.Extension -ieq '.dll'
    }
    foreach ($file in $files) {
        try {
            $pe = Get-PeInfo -Path $file.FullName
            foreach ($import in $pe.Imports) {
                if ($import.Dll -ieq 'd3d9.dll') {
                    $consumers += [pscustomobject]@{
                        Module      = $file.Name
                        Machine     = $pe.MachineName
                        MachineCode = $pe.Machine
                        Symbols     = @($import.Symbols)
                    }
                }
            }
        }
        catch {
            if ($file.Name -ieq 'L2.exe' -or $file.Name -ieq 'D3DDrv.dll') {
                throw "No se pudo inspeccionar $($file.Name): $($_.Exception.Message)"
            }
            $parseWarnings += "$($file.Name): $($_.Exception.Message)"
        }
    }
    if ($consumers.Count -eq 0) {
        throw 'No se encontro ningun modulo con importacion estatica de d3d9.dll. El cliente puede estar empaquetado o usar otro renderer; no se modificara.'
    }

    $unsupported = @()
    foreach ($consumer in $consumers) {
        if ($consumer.MachineCode -ne 0x014C) {
            $unsupported += "$($consumer.Module): arquitectura $($consumer.Machine)"
        }
        foreach ($symbol in $consumer.Symbols) {
            if ($script:SupportedImports -notcontains $symbol) {
                $unsupported += "$($consumer.Module)!$symbol"
            }
        }
    }
    if ($unsupported.Count -gt 0) {
        throw ('El proxy no cubre imports requeridos por este cliente: ' + (($unsupported | Sort-Object -Unique) -join ', '))
    }

    return [pscustomobject]@{
        L2Architecture = $l2Info.MachineName
        Consumers      = @($consumers)
        ParseWarnings  = @($parseWarnings)
        Compatible     = $true
    }
}

function Invoke-WithProcessEnvironment {
    param(
        [hashtable]$Variables,
        [scriptblock]$Action
    )
    $previous = @{}
    try {
        foreach ($name in $Variables.Keys) {
            $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
            [Environment]::SetEnvironmentVariable($name, [string]$Variables[$name], 'Process')
        }
        & $Action
    }
    finally {
        foreach ($name in $Variables.Keys) {
            [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
        }
    }
}

function Invoke-D3D12Preflight {
    param([string]$SessionDirectory)

    if (-not (Test-Path -LiteralPath $script:ProxyPath -PathType Leaf)) {
        throw "Falta el proxy del paquete: $script:ProxyPath"
    }
    if (-not (Test-Path -LiteralPath $script:SmokePath -PathType Leaf)) {
        throw "Falta la prueba del paquete: $script:SmokePath"
    }
    $logDirectory = Join-Path $SessionDirectory 'preflight-logs'
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
    $output = Invoke-WithProcessEnvironment -Variables @{
        INTERLUDE_D3D_BACKEND = '9on12'
        L2D3D12_TELEMETRY     = '0'
        L2D3D12_SMOKE_HIDDEN  = '1'
        L2D3D12_LOG_DIR       = $logDirectory
    } -Action {
        $lines = & $script:SmokePath $script:ProxyPath 3 9on12 2>&1
        $code = $LASTEXITCODE
        [pscustomobject]@{
            ExitCode = $code
            Text     = ($lines | Out-String)
        }
    }
    Write-Utf8File -Path (Join-Path $SessionDirectory 'preflight.txt') -Text $output.Text
    if ($output.ExitCode -ne 0 -or $output.Text -notmatch 'effective_backend=9on12') {
        throw "Windows/GPU no completo la prueba D3D9On12. Revisa $SessionDirectory\preflight.txt"
    }
    return [pscustomobject]@{
        Passed   = $true
        ExitCode = $output.ExitCode
        Evidence = 'effective_backend=9on12'
    }
}

function Install-Proxy {
    param([object]$Layout)

    $target = Join-Path $Layout.SystemDirectory 'd3d9.dll'
    $manifestPath = Join-Path $Layout.SystemDirectory '.l2d3d12-portable.json'
    $proxyHash = (Get-FileHash -LiteralPath $script:ProxyPath -Algorithm SHA256).Hash
    $manifest = $null
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        if ([IO.Path]::GetFullPath([string]$manifest.targetPath) -ne [IO.Path]::GetFullPath($target)) {
            throw 'El manifiesto existente pertenece a otro destino. No se modificara.'
        }
    }

    $action = 'Installed'
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $currentHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        if ($null -eq $manifest) {
            throw "Ya existe system\d3d9.dll sin manifiesto portable (SHA256 $currentHash). Puede ser ReShade, dxwrapper o una proteccion del servidor; no se sobrescribira."
        }
        if ($currentHash -ne [string]$manifest.installedProxySha256) {
            throw 'system\d3d9.dll cambio desde la instalacion portable. No se sobrescribira ni se eliminara.'
        }
        if ($currentHash -eq $proxyHash) {
            return [pscustomobject]@{
                Action       = 'AlreadyInstalled'
                TargetPath   = $target
                ManifestPath = $manifestPath
                Sha256       = $proxyHash
            }
        }
        $action = 'Upgraded'
    }
    elseif ($null -ne $manifest) {
        throw 'Existe un manifiesto portable pero falta system\d3d9.dll. Estado inconsistente; no se modificara.'
    }

    $temporary = "$target.new.$PID"
    try {
        Copy-Item -LiteralPath $script:ProxyPath -Destination $temporary
        $temporaryHash = (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash
        if ($temporaryHash -ne $proxyHash) {
            throw 'La copia temporal del proxy no coincide por SHA-256.'
        }
        Move-Item -LiteralPath $temporary -Destination $target -Force
        $newManifest = [ordered]@{
            schemaVersion         = 1
            packageVersion        = $script:PackageVersion
            installedAtUtc        = [DateTime]::UtcNow.ToString('o')
            clientRoot            = $Layout.ClientRoot
            systemDirectory       = $Layout.SystemDirectory
            targetPath            = $target
            installedProxySha256  = $proxyHash
            hadOriginalD3D9       = $false
            originalD3D9Backup    = $null
        }
        Save-JsonFile -Path $manifestPath -Value $newManifest
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
    return [pscustomobject]@{
        Action       = $action
        TargetPath   = $target
        ManifestPath = $manifestPath
        Sha256       = $proxyHash
    }
}

function Remove-Proxy {
    param([object]$Layout)

    $target = Join-Path $Layout.SystemDirectory 'd3d9.dll'
    $manifestPath = Join-Path $Layout.SystemDirectory '.l2d3d12-portable.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw 'No existe un manifiesto de instalacion portable en este cliente. No se eliminara ningun archivo.'
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        throw 'El manifiesto existe, pero system\d3d9.dll ya no esta. No se eliminara el manifiesto automaticamente.'
    }
    $currentHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($currentHash -ne [string]$manifest.installedProxySha256) {
        throw 'system\d3d9.dll fue modificado despues de la instalacion. No se eliminara.'
    }
    if ([bool]$manifest.hadOriginalD3D9) {
        $backup = [string]$manifest.originalD3D9Backup
        if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
            throw 'El manifiesto requiere restaurar un original, pero su respaldo no existe.'
        }
        Remove-Item -LiteralPath $target -Force
        Move-Item -LiteralPath $backup -Destination $target
        $action = 'OriginalRestored'
    }
    else {
        Remove-Item -LiteralPath $target -Force
        $action = 'ProxyRemoved'
    }
    Remove-Item -LiteralPath $manifestPath -Force
    return [pscustomobject]@{
        Action     = $action
        TargetPath = $target
        Sha256     = $currentHash
    }
}

function Resolve-LaunchPath {
    param(
        [object]$Layout,
        [string]$RequestedExecutable
    )
    if ([string]::IsNullOrWhiteSpace($RequestedExecutable)) {
        return $Layout.L2Path
    }
    if ([IO.Path]::IsPathRooted($RequestedExecutable)) {
        $candidate = $RequestedExecutable
    }
    else {
        $systemCandidate = Join-Path $Layout.SystemDirectory $RequestedExecutable
        $rootCandidate = Join-Path $Layout.ClientRoot $RequestedExecutable
        $candidate = if (Test-Path -LiteralPath $systemCandidate -PathType Leaf) {
            $systemCandidate
        }
        else {
            $rootCandidate
        }
    }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "No existe el ejecutable solicitado: $candidate"
    }
    $full = [IO.Path]::GetFullPath($candidate)
    $root = [IO.Path]::GetFullPath($Layout.ClientRoot).TrimEnd('\') + '\'
    if (-not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'El ejecutable a iniciar debe estar dentro de la carpeta del cliente.'
    }
    return $full
}

function Start-ClientAndVerify {
    param(
        [object]$Layout,
        [string]$SessionDirectory,
        [string]$RequestedExecutable,
        [string[]]$Arguments,
        [int]$TimeoutSeconds
    )
    $launchPath = Resolve-LaunchPath $Layout $RequestedExecutable
    $runtimeLogs = Join-Path $SessionDirectory 'runtime-logs'
    New-Item -ItemType Directory -Force -Path $runtimeLogs | Out-Null

    $process = Invoke-WithProcessEnvironment -Variables @{
        INTERLUDE_D3D_BACKEND = '9on12'
        L2D3D12_TELEMETRY     = '0'
        L2D3D12_LOG_DIR       = $runtimeLogs
    } -Action {
        $parameters = @{
            FilePath         = $launchPath
            WorkingDirectory = Split-Path -Parent $launchPath
            PassThru         = $true
        }
        if ($null -ne $Arguments -and $Arguments.Count -gt 0) {
            $parameters.ArgumentList = $Arguments
        }
        Start-Process @parameters
    }

    $logPath = Join-Path $runtimeLogs 'd3d9-proxy.log'
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $status = 'NotObserved'
    $evidence = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $logPath -PathType Leaf) {
            $text = Get-Content -LiteralPath $logPath -Raw
            if ($text -match 'CreateDevice effective_backend=9on12') {
                $status = 'Confirmed9On12Device'
                $evidence = 'CreateDevice effective_backend=9on12'
                break
            }
            if ($text -match 'CreateDevice effective_backend=native9') {
                $status = 'FallbackNative9Device'
                $evidence = 'CreateDevice effective_backend=native9'
                break
            }
            if ($text -match 'Direct3DCreate9(?:Ex)?[^\r\n]*effective_backend=9on12') {
                $status = 'Initialized9On12Object'
                $evidence = $Matches[0]
            }
            elseif ($text -match 'Direct3DCreate9(?:Ex)?[^\r\n]*effective_backend=native9') {
                $status = 'FallbackNative9Object'
                $evidence = $Matches[0]
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return [pscustomobject]@{
        Status         = $status
        Evidence       = $evidence
        ProcessId      = $process.Id
        Executable     = $launchPath
        RuntimeLogPath = $logPath
    }
}

try {
    if (-not (Test-Path -LiteralPath $script:PortableRoot -PathType Container)) {
        throw 'No se pudo resolver la carpeta del paquete portable.'
    }
    $sessionRoot = Join-Path $script:PortableRoot 'sessions'
    New-Item -ItemType Directory -Force -Path $sessionRoot | Out-Null
    $sessionDirectory = Join-Path $sessionRoot ([DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Path $sessionDirectory | Out-Null

    Write-Stage 'Detectando cliente Lineage II'
    $layout = Resolve-ClientLayout -RequestedPath $ClientPath
    Write-Host "Cliente: $($layout.ClientRoot)"
    Write-Host "System : $($layout.SystemDirectory)"

    if ($Remove) {
        Write-Stage 'Eliminando solo la instalacion identificada por manifiesto y hash'
        $removal = Remove-Proxy -Layout $layout
        $report = [ordered]@{
            packageVersion = $script:PackageVersion
            operation      = 'Remove'
            timestampUtc   = [DateTime]::UtcNow.ToString('o')
            layout         = $layout
            removal        = $removal
            result         = 'Success'
        }
        Save-JsonFile -Path (Join-Path $sessionDirectory 'session.json') -Value $report
        Write-Host "Listo: $($removal.Action)" -ForegroundColor Green
        Write-Host "Reporte: $sessionDirectory\session.json"
        exit 0
    }

    Write-Stage 'Inspeccionando arquitectura e imports D3D9'
    $compatibility = Test-ClientCompatibility -Layout $layout
    foreach ($consumer in $compatibility.Consumers) {
        Write-Host ("Compatible: {0} ({1}) -> {2}" -f $consumer.Module, $consumer.Machine, ($consumer.Symbols -join ', '))
    }

    Write-Stage 'Probando D3D9On12/DX12 en este Windows y GPU'
    $preflight = Invoke-D3D12Preflight -SessionDirectory $sessionDirectory
    Write-Host 'D3D9On12 confirmado por IDirect3DDevice9On12.' -ForegroundColor Green

    if ($InspectOnly) {
        $report = [ordered]@{
            packageVersion = $script:PackageVersion
            operation      = 'InspectOnly'
            timestampUtc   = [DateTime]::UtcNow.ToString('o')
            layout         = $layout
            compatibility  = $compatibility
            preflight      = $preflight
            result         = 'Compatible'
        }
        Save-JsonFile -Path (Join-Path $sessionDirectory 'session.json') -Value $report
        Write-Host 'Cliente compatible; no se modifico ningun archivo.' -ForegroundColor Green
        Write-Host "Reporte: $sessionDirectory\session.json"
        exit 0
    }

    Write-Stage 'Instalando el proxy de forma reversible'
    $installation = Install-Proxy -Layout $layout
    Write-Host "$($installation.Action): $($installation.TargetPath)" -ForegroundColor Green

    $runtime = $null
    if (-not $InstallOnly) {
        Write-Stage 'Iniciando cliente con backend solicitado 9on12'
        $runtime = Start-ClientAndVerify -Layout $layout -SessionDirectory $sessionDirectory -RequestedExecutable $LaunchExecutable -Arguments $LaunchArguments -TimeoutSeconds $VerifySeconds
        switch ($runtime.Status) {
            'Confirmed9On12Device' {
                Write-Host 'DX12 confirmado en el dispositivo real del cliente.' -ForegroundColor Green
            }
            'Initialized9On12Object' {
                Write-Host 'Objeto D3D9On12 inicializado; el cliente aun no creo el dispositivo durante la ventana de observacion.' -ForegroundColor Yellow
            }
            'FallbackNative9Device' {
                Write-Host 'El cliente creo un dispositivo D3D9 nativo: hubo fallback.' -ForegroundColor Yellow
            }
            'FallbackNative9Object' {
                Write-Host 'La inicializacion cayo a D3D9 nativo.' -ForegroundColor Yellow
            }
            default {
                Write-Host 'El cliente fue iniciado, pero aun no se observo inicializacion D3D9. Puede seguir cargando o usar otro proceso.' -ForegroundColor Yellow
            }
        }
        Write-Host "PID iniciado: $($runtime.ProcessId)"
    }

    $report = [ordered]@{
        packageVersion = $script:PackageVersion
        operation      = if ($InstallOnly) { 'InstallOnly' } else { 'InstallAndLaunch' }
        timestampUtc   = [DateTime]::UtcNow.ToString('o')
        layout         = $layout
        compatibility  = $compatibility
        preflight      = $preflight
        installation   = $installation
        runtime        = $runtime
        result         = 'Success'
    }
    Save-JsonFile -Path (Join-Path $sessionDirectory 'session.json') -Value $report
    Write-Host "Reporte: $sessionDirectory\session.json"
    Write-Host 'Para usar DX12 nuevamente, inicia siempre desde Start-L2D3D12.cmd o este script.' -ForegroundColor Cyan
    exit 0
}
catch {
    Write-Host "`nERROR: $($_.Exception.Message)" -ForegroundColor Red
    if (Get-Variable -Name sessionDirectory -ErrorAction SilentlyContinue) {
        $failure = [ordered]@{
            packageVersion = $script:PackageVersion
            operation      = if ($Remove) { 'Remove' } elseif ($InspectOnly) { 'InspectOnly' } else { 'InstallAndLaunch' }
            timestampUtc   = [DateTime]::UtcNow.ToString('o')
            result         = 'Failed'
            error          = $_.Exception.Message
        }
        Save-JsonFile -Path (Join-Path $sessionDirectory 'session.json') -Value $failure
        Write-Host "Reporte: $sessionDirectory\session.json"
    }
    exit 1
}
