# Build build\upd\totk.exe for XCX DE 2.0.0 with the llvm-mingw toolchain and dependencies of a TotK Recomp setup
# folder, then copy the DLLs and the shader translator next to it. Run from the repository root.
param([string]$Setup = "$PSScriptRoot\..\..\..\totk-pc-recomp\TotK-Setup-0.1.1-windows")
$ErrorActionPreference = 'Stop'
$Setup = (Resolve-Path $Setup).Path
& "$PSScriptRoot\..\.venv\Scripts\python.exe" "$PSScriptRoot\build_game.py" --version upd `
    --expected "$PSScriptRoot\data\expected-xcx-2.0.0.json" --toolchain "$Setup\llvm" --deps "$Setup\runtime\deps" `
    --ninja "$Setup\bin\ninja.exe" 2>&1 | Select-String -NotMatch 'warning|note:|^\s+\||^\s*\d+ \||^E:/Game|^\s*\^|_Nonnull|_Nullable|^\[\d+/\d+\] CC'
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$out = "$PSScriptRoot\..\build\upd"
Copy-Item "$Setup\bin\SDL3.dll", "$Setup\bin\libopus-0.dll", "$Setup\bin\totk_xlate.exe", "$Setup\bin\totk_xlate.id" $out -Force
Copy-Item "$Setup\llvm\bin\libc++.dll", "$Setup\llvm\bin\libunwind.dll", "$Setup\llvm\bin\libwinpthread-1.dll" $out -Force
