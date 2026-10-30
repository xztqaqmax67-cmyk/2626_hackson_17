param(
    [string]$Python = '',
    [string]$Compiler = 'g++',
    [int]$Cores = 4,
    [double]$SolverSeconds = 1,
    [string[]]$Cases = @(),
    [string]$Out = 'runs/local_v3',
    [switch]$TestsOnly
)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (-not $Python) {
        foreach ($candidate in @('python', 'python3', 'py')) {
            $found = Get-Command $candidate -ErrorAction SilentlyContinue
            if ($found) { $Python = $found.Source; break }
        }
    }
    if (-not $Python) {
        $bundledPython = 'C:\Program Files\Blender Foundation\Blender 4.4\4.4\python\bin\python.exe'
        if (Test-Path -LiteralPath $bundledPython) { $Python = $bundledPython }
    }
    if (-not $Python) { throw 'Python 3.10+ not found. Pass -Python with the full path to python.exe.' }
    & $Python -c 'import sys; assert sys.version_info >= (3, 10), "Python 3.10+ required"; print(sys.executable)'
    if ($LASTEXITCODE -ne 0) { throw 'Python check failed' }
    & $Compiler -O2 -std=c++17 -Wall -Wextra solver.cpp -o solver.exe
    if ($LASTEXITCODE -ne 0) { throw 'C++ compilation failed' }
    & $Python -m unittest discover -s tests -v
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    if (-not $TestsOnly) {
        $dataDir = Join-Path $PSScriptRoot '../hackathon_data'
        $scoreArgs = @('score.py', '--data', $dataDir, '--solver', '.\solver.exe',
            '--engine', 'local', '-n', $Cores, '--solver-time-limit', $SolverSeconds,
            '--timeout', '300', '--out', $Out)
        if ($Cases.Count) { $scoreArgs += '--cases'; $scoreArgs += $Cases }
        & $Python @scoreArgs
        if ($LASTEXITCODE -ne 0) { throw "Evaluation incomplete; inspect $Out\summary.json" }
    }
} finally {
    Pop-Location
}
