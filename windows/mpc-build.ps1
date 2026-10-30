# Runs mpc-vst-plugins' bash tools from Windows using only Docker Desktop.
#   .\mpc-build.ps1 poc/polltest/vst.json                  -> tools/build_port.sh <vst.json>
#   .\mpc-build.ps1 -Tool test_port.sh poc/polltest/vst.json
# Needs the mpc-builder image (bash + python3 + docker CLI). Paths are relative to the repo.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Tool = "build_port.sh",
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ToolArgs
)
# The repo is the mpc-vst-plugins folder next to this script's folder.
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\mpc-vst-plugins")).Path
# Mount the repo at Docker Desktop's own path for that drive (D:\x -> /run/desktop/mnt/host/d/x) so the containers
# build_port.sh starts can see the same files.
$inner = "/run/desktop/mnt/host/" + $repo.Substring(0, 1).ToLower() + ($repo.Substring(2) -replace '\\', '/')
docker run --rm -v //var/run/docker.sock:/var/run/docker.sock -v "${repo}:${inner}" -w "$inner" mpc-builder bash "tools/$Tool" @ToolArgs
