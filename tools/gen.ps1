# Regenerates ChronoStretch's parameters, presets, art and layout (tools/gen_params.py, then art/make_art.py) in Docker.
#   .\gen.ps1
# The toolkit is mounted whole: the FX page's preset pictures use its Titillium font (tools/html_art/fonts).
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
docker run --rm -v "${repo}:/r" -w /r/ports/chrono python:3.11-slim sh -c "python3 -I tools/gen_params.py && pip install -q pillow >/dev/null 2>&1 && python3 -I art/make_art.py"
exit $LASTEXITCODE
