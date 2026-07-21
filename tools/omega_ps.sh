#!/usr/bin/env bash
# omega_ps.sh — run PowerShell on omega-new WITHOUT cmd/quote/unicode mangling.
#
# WHY (recurring pain, memory reference-omega-new-ssh-powershell):
#   `ssh omega-new` lands in cmd.exe. Hand-nested  cmd /c "powershell -Command "...""
#   gets its inner quotes/commas/pipes eaten by cmd's parser before PowerShell sees
#   them, AND scp'ing a .ps1 mangles non-ASCII (em-dash -> junk). Every ad-hoc box
#   query re-hit this. STOP hand-nesting: this helper base64-encodes the script as
#   UTF-16LE and runs it via `powershell -EncodedCommand`, which is immune to BOTH
#   cmd quote-stripping (base64 is quote-free) and charset mangling (UTF-16LE).
#
# Usage:
#   bash tools/omega_ps.sh script.ps1        # run a local .ps1 on omega-new
#   echo 'Get-Date' | bash tools/omega_ps.sh -   # run stdin
#   HOST=omega-new bash tools/omega_ps.sh -  # HOST override (omega-new default; dead box blocked)
set -euo pipefail
HOST="${HOST:-omega-new}"
case "$HOST" in
  omega-vps|185.167.119.59)
    echo "[omega_ps][FATAL] HOST=$HOST is the RETIRED dead box. Live desk is omega-new." >&2
    exit 1;;
esac
SRC="${1:--}"
[[ "$SRC" == "-" ]] && SRC=/dev/stdin
B64=$(iconv -f UTF-8 -t UTF-16LE "$SRC" | base64 | tr -d '\n')
# -OutputFormat Text keeps stdout clean text, not the #< CLIXML serialization ssh
# emits when it captures PowerShell's streams. Put $ProgressPreference='SilentlyContinue'
# at the top of your .ps1 to also drop Invoke-WebRequest progress spam.
exec ssh "$HOST" "powershell -NoProfile -NonInteractive -OutputFormat Text -ExecutionPolicy Bypass -EncodedCommand $B64"
