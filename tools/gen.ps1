# Regenerates ChronoStretch's parameters, presets, art and layout (tools/gen_params.py, then art/make_art.py) in Docker.
#   .\gen.ps1
$port = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
docker run --rm -v "${port}:/w" -w /w python:3.11-slim sh -c "python3 -I tools/gen_params.py && pip install -q pillow >/dev/null 2>&1 && python3 -I art/make_art.py"
exit $LASTEXITCODE
