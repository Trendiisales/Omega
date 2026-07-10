#!/bin/bash
# Archive VPS dated logs (>N days old) to Google Drive, then delete from VPS.
# INVARIANT: never delete a VPS file until its archive is verified-uploaded to gdrive.
# Targets the disk hogs: C:\Omega\logs\ibkr_l2\*dated* + l2_ticks_*dated* + dated *.log.
# These are RESEARCH DATA (L2 backtests) -> moved off-box, not destroyed.
# Mac-side daily cron; minimal VPS footprint (one tar, one scp).
#
# Usage:
#   archive_vps_logs.sh           # full cycle: tar -> upload -> verify -> delete
#   ARCHIVE_DRYRUN=1 ...          # tar+upload+verify, SKIP delete (proof run)
#   ARCHIVE_DAYS=3 ...            # change age cutoff (default 2)
set -euo pipefail

DAYS="${ARCHIVE_DAYS:-2}"
DRYRUN="${ARCHIVE_DRYRUN:-0}"
REMOTE="${ARCHIVE_REMOTE:-gdrive:Omega_log_archive}"
RCLONE="${RCLONE_BIN:-/opt/homebrew/bin/rclone}"
STAMP="$(date +%Y%m%d_%H%M%S)"
TARNAME="omega_logs_${STAMP}.tar.gz"
VPS_TMP="C:/Omega/logtmp"
LOCAL_TMP="/tmp/${TARNAME}"
LISTFILE="/tmp/omega_logarch_list_${STAMP}.txt"

log(){ echo "[$(date +%H:%M:%S)] $*"; }

ps_enc(){ /usr/bin/python3 -c "import sys,base64;print(base64.b64encode(sys.argv[1].encode('utf-16-le')).decode())" "$1"; }
vps_ps(){ ssh omega-new "powershell -NoProfile -EncodedCommand $(ps_enc "$1")"; }

# 1. VPS: select dated files older than DAYS, write a relative-path list + tar them.
log "selecting VPS files >${DAYS}d old + building tarball ${VPS_TMP}/${TARNAME}"
SELECT_PS='$ErrorActionPreference="Stop"
$root="C:\Omega\logs"
$cut=(Get-Date).AddDays(-'"$DAYS"')
$files = Get-ChildItem $root -Recurse -File | Where-Object {
  $_.LastWriteTime -lt $cut -and ($_.Name -match "_\d{4}-\d{2}-\d{2}")
}
if(-not $files){ Write-Output "NOFILES"; exit 0 }
New-Item -ItemType Directory -Force -Path "'"$VPS_TMP"'" | Out-Null
$rel = $files | ForEach-Object { $_.FullName.Substring($root.Length+1) }
$listPath = "'"$VPS_TMP"'\\filelist.txt"
$rel | Set-Content -Encoding ASCII $listPath
# tar.exe ships with Win10+; -T reads the file list; -C sets base dir so paths are relative
tar.exe -czf "'"$VPS_TMP/$TARNAME"'" -C $root -T $listPath
$sz=(Get-Item "'"$VPS_TMP/$TARNAME"'").Length
$mb=[math]::Round($sz/1MB,1)
$origMB=[math]::Round((($files|Measure-Object Length -Sum).Sum)/1MB,1)
Write-Output ("TARRED files={0} origMB={1} tarMB={2}" -f $files.Count,$origMB,$mb)'
RES="$(vps_ps "$SELECT_PS" 2>/dev/null | tr -d '\r')"
echo "$RES" | grep -v '^#<\|^<Objs\|CLIXML' || true
if echo "$RES" | grep -q "NOFILES"; then log "nothing to archive (no dated files >${DAYS}d). done."; exit 0; fi
echo "$RES" | grep -q "TARRED" || { log "ERROR: tar step did not report success"; exit 1; }

# 2. scp the single tarball to Mac /tmp.
log "pulling tarball -> ${LOCAL_TMP}"
scp -q "omega-new:${VPS_TMP}/${TARNAME}" "${LOCAL_TMP}"
[ -s "${LOCAL_TMP}" ] || { log "ERROR: scp produced empty/no file"; exit 1; }
LOCAL_SZ="$(stat -f%z "${LOCAL_TMP}")"
log "local tarball ${LOCAL_SZ} bytes"

# 3. rclone upload to Google Drive.
log "uploading -> ${REMOTE}/${TARNAME}"
"${RCLONE}" copyto "${LOCAL_TMP}" "${REMOTE}/${TARNAME}"

# 4. VERIFY: remote size must equal local size.
REMOTE_SZ="$("${RCLONE}" size "${REMOTE}/${TARNAME}" --json 2>/dev/null | /usr/bin/python3 -c "import sys,json;print(json.load(sys.stdin)['bytes'])")"
log "verify: local=${LOCAL_SZ} remote=${REMOTE_SZ}"
[ "${LOCAL_SZ}" = "${REMOTE_SZ}" ] || { log "ERROR: size mismatch -> NOT deleting VPS originals"; exit 1; }
log "upload verified OK"

# 5. delete: originals on VPS + the VPS-side tarball. Gated by DRYRUN.
if [ "${DRYRUN}" = "1" ]; then
  log "DRYRUN=1 -> skipping VPS delete. Verified archive at ${REMOTE}/${TARNAME}"
  log "VPS tarball left at ${VPS_TMP}/${TARNAME} (remove manually or re-run without DRYRUN)"
else
  log "deleting archived originals on VPS + VPS tarball"
  DELETE_PS='$ErrorActionPreference="Stop"
$root="C:\Omega\logs"
$listPath="'"$VPS_TMP"'\\filelist.txt"
$rel = Get-Content $listPath
$n=0
foreach($r in $rel){ $p=Join-Path $root $r; if(Test-Path $p){ Remove-Item -Force $p; $n++ } }
Remove-Item -Force "'"$VPS_TMP/$TARNAME"'"
Remove-Item -Force $listPath
$free=[math]::Round((Get-PSDrive C).Free/1GB,1)
Write-Output ("DELETED count={0} freeGB_now={1}" -f $n,$free)'
  vps_ps "$DELETE_PS" 2>/dev/null | tr -d '\r' | grep "DELETED" || { log "ERROR: delete step did not confirm"; exit 1; }
fi

# 6. clean Mac tmp (tarball already safe in gdrive).
rm -f "${LOCAL_TMP}" "${LISTFILE}"
log "done. archive=${REMOTE}/${TARNAME}"
