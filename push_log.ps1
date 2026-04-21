# ==============================================================================
#  OMEGA - push_log.ps1  [CULLED / NO-OP STUB]
#
#  This script has been culled. State is now read via omega-proxy (direct
#  filesystem access over Cloudflare tunnel), not via git commits.
#
#  Reasons for removal:
#    1. Pushed to main every 5 minutes, polluting commit history with
#       'state: log+snapshot+atr' commits.
#    2. Caused every 'git reset --hard origin/main' on the VPS to race with
#       in-flight state pushes, occasionally leaving wedged git processes
#       and dangling credential-manager subprocesses.
#    3. Functionally replaced by omega-proxy v2 endpoints (/file, /tree).
#
#  All callers have been removed:
#    - include/quote_loop.hpp   (5-minute timer)
#    - include/omega_main.hpp   (startup call)
#    - DEPLOY_OMEGA.ps1         (post-build call)
#
#  The file itself should be deleted via:
#    git rm push_log.ps1
#    git commit -m "chore: remove culled push_log.ps1 stub"
#    git push
#
#  Until then this stub exits immediately so any lingering invocation is safe.
# ==============================================================================

param(
    [string]$RepoRoot = "C:\Omega"
)

Write-Host "[push_log] CULLED -- no-op stub. This script does nothing." -ForegroundColor DarkGray
exit 0
