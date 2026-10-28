# build.ps1 -- build 1cmcp.dll (1C 7.7 native component, MCP server) and selftest.exe with portable VC6.
# ASCII-only on purpose: WinPS 5.1 reads BOM-less .ps1 as ANSI and mangles non-ASCII.
# Layout assumed:  <1C>\SQLite\{tools,_1Common}   and   <1C>\VK\1CAPI (this folder)
# Usage:
#   powershell -File build.ps1              # component + selftest
#   powershell -File build.ps1 -SelfTest    # selftest only (no 1C headers needed)
param([string]$SqliteRoot = "", [switch]$SelfTest)

$ErrorActionPreference = 'Stop'
$proj = $PSScriptRoot
if (-not $SqliteRoot) { $SqliteRoot = Join-Path (Split-Path (Split-Path $proj)) 'SQLite' }
$vc     = Join-Path $SqliteRoot 'tools\MSVC600\VC98'
$common = Join-Path $SqliteRoot 'tools\MSVC600\Common'
$onec   = Join-Path $SqliteRoot '_1Common'
$libs   = Join-Path $onec '1CHEADERS\LIBS'
$obj    = Join-Path $proj 'obj'
$out    = Join-Path $proj 'build'

foreach ($p in @($vc, $onec, $libs)) { if (-not (Test-Path $p)) { throw "not found: $p" } }

$env:PATH    = "$vc\Bin;$common\MSDev98\Bin;$env:PATH"
$env:INCLUDE = "$vc\ATL\Include;$vc\Include;$vc\MFC\Include;$onec;$proj\src;$proj\core"
$env:LIB     = "$vc\Lib;$vc\MFC\Lib;$libs"

New-Item -ItemType Directory -Force $obj | Out-Null
New-Item -ItemType Directory -Force $out | Out-Null

function Compile($src, $flags) {
  $dst = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($src) + '.obj')
  Write-Host "=== CL $src ===" -ForegroundColor Cyan
  $o = & "$vc\Bin\CL.EXE" @flags "/Fo$dst" $src 2>&1
  $rc = $LASTEXITCODE
  $o | ForEach-Object { Write-Host $_ }
  if ($rc -ne 0) { Write-Host "  FAILED ($rc)" -ForegroundColor Red; return 1 }
  return 0
}

function Link($la, $name) {
  Write-Host "=== LINK $name ===" -ForegroundColor Cyan
  $o = & "$vc\Bin\LINK.EXE" @la 2>&1; $rc = $LASTEXITCODE
  $o | ForEach-Object { Write-Host $_ }
  if ($rc -ne 0) { Pop-Location; Write-Host "LINK $name failed ($rc)." -ForegroundColor Red; exit 1 }
}

Push-Location $proj
$fail = 0
# core: plain Win32, no MFC
$coreFlags = @('/nologo','/c','/MD','/GX','/GR','/O2','/W3','/DWIN32','/DNDEBUG','/DWIN32_LEAN_AND_MEAN')
$core = @('core\json.cpp','core\textconv.cpp','core\mcp_core.cpp')
foreach ($s in $core + @('test\selftest.cpp')) { $fail += Compile $s $coreFlags }
if ($fail) { Pop-Location; Write-Host "Compile errors: $fail" -ForegroundColor Red; exit 1 }
$coreObj = @('obj\json.obj','obj\textconv.obj','obj\mcp_core.obj')

Link (@('/nologo','/subsystem:console','/OUT:build\selftest.exe','obj\selftest.obj') + $coreObj + @('kernel32.lib','user32.lib','ws2_32.lib')) 'selftest.exe'
if ($SelfTest) { Pop-Location; Write-Host "BUILD OK (selftest)" -ForegroundColor Green; exit 0 }

# component: MFC + 1C headers, flags mirror 1sqlite Release
$vkFlags = @('/nologo','/c','/MD','/GX','/GR','/O2','/Zm200','/DWIN32','/DNDEBUG','/D_NDEBUG','/D_AFXEXT')
foreach ($s in @('src\1cmcp.cpp','src\forms.cpp','src\optpage.cpp','src\StdAfx.cpp')) { $fail += Compile $s $vkFlags }
if ($fail) { Pop-Location; Write-Host "Compile errors: $fail" -ForegroundColor Red; exit 1 }

# version + "\0sd" resources (1C 7.7 unloads a component DLL without them) + options page dialog
$o = & "$common\MSDev98\Bin\RC.EXE" /i "$vc\Include" /i "$vc\MFC\Include" /i 'src' /i 'core' /fo 'obj\1cmcp.res' 'src\1cmcp.rc' 2>&1
if ($LASTEXITCODE -ne 0) { $o | ForEach-Object { Write-Host $_ }; Pop-Location; Write-Host "RC failed." -ForegroundColor Red; exit 1 }

# link cwd = project dir so the relative lib pragmas in StdAfx.cpp resolve
$syslibs = @('kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','oleaut32.lib','uuid.lib','comctl32.lib','version.lib','ws2_32.lib')
Link (@('/nologo','/DLL','/subsystem:windows','/IGNORE:4089','/OUT:build\1cmcp.dll','obj\1cmcp.obj','obj\forms.obj','obj\optpage.obj','obj\StdAfx.obj') + $coreObj + @('obj\1cmcp.res') + $syslibs) '1cmcp.dll'
Pop-Location
Write-Host "BUILD OK -> $out" -ForegroundColor Green
