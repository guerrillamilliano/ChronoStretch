# Builds test/demo.c (under ASan/UBSan) and runs ChronoStretch's offline checks; audition WAVs go to ports/chrono/build/demo.
#   .\demo.ps1          everything
#   .\demo.ps1 quick    skip the preset renders
# Needs Docker Desktop (gcc:11).
param([string]$Which = "all")
$port = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$repo = (Resolve-Path -LiteralPath (Join-Path $port "..\..")).Path
docker run --rm -v "${repo}:/r" -w /r/ports/chrono gcc:11 sh -c "mkdir -p build && gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -I/r/wrapper -o build/demo_run test/demo.c src/pffft/pffft.c -lm -lpthread && ./build/demo_run $Which"
exit $LASTEXITCODE
