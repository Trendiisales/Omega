#!/usr/bin/env bash
# scripts/setup-mac-shell.sh
#
# One-shot bootstrap for the developer shell on Jo's Mac. Adds two permanent
# quality-of-life fixes to ~/.zshrc:
#
#   1. `setopt interactive_comments` — lets you paste shell snippets that
#      contain `#` comments without zsh treating `#` as a literal token.
#
#   2. `git_unlock` shell function + `gunlock` alias — safely removes a
#      stale `.git/index.lock` if (and only if) it is older than 60 seconds,
#      so it never kills an in-progress git command. Also exposes `git-safe`
#      which auto-cleans before invoking git, for paranoid runs.
#
# Idempotent: re-running this script will not append duplicate blocks. The
# additions are wrapped in unique sentinel markers so we can detect them.
#
# Usage:
#   bash /Users/jo/omega_repo/scripts/setup-mac-shell.sh
#   source ~/.zshrc        # activate immediately in current shell
#
# Tested on macOS zsh 5.9 (default on Sonoma+). Uses BSD `stat -f`, not
# GNU `stat -c`, so it will NOT work on Linux as-is.

set -euo pipefail

ZSHRC="$HOME/.zshrc"
SENTINEL_BEGIN="# >>> omega shell setup (managed) >>>"
SENTINEL_END="# <<< omega shell setup (managed) <<<"

if [[ ! -f "$ZSHRC" ]]; then
    echo "Creating $ZSHRC (was missing)"
    touch "$ZSHRC"
fi

# Strip any previous managed block so we can re-write cleanly.
if grep -qF "$SENTINEL_BEGIN" "$ZSHRC"; then
    echo "Existing managed block found in $ZSHRC — replacing in place."
    # Use awk to drop the block bounded by the two sentinels.
    awk -v s="$SENTINEL_BEGIN" -v e="$SENTINEL_END" '
        $0 == s { skip = 1; next }
        $0 == e { skip = 0; next }
        !skip   { print }
    ' "$ZSHRC" > "$ZSHRC.tmp"
    mv "$ZSHRC.tmp" "$ZSHRC"
fi

# Append the fresh managed block. Heredoc is quoted so no expansion happens.
cat >> "$ZSHRC" <<'EOF'
# >>> omega shell setup (managed) >>>
# Managed by scripts/setup-mac-shell.sh — do not edit between sentinels.
# Re-run that script to refresh; manual edits here will be overwritten.

# Allow `#` comments in interactive zsh sessions.
setopt interactive_comments

# git_unlock: safely remove a stale .git/index.lock from a repo.
# Refuses if the lock is younger than 60 seconds (a real git op may be in
# flight). Defaults to the current repo if no path is given.
git_unlock() {
    local repo
    if [[ -n "${1:-}" ]]; then
        repo="$1"
    else
        repo="$(git rev-parse --show-toplevel 2>/dev/null)"
    fi
    if [[ -z "$repo" ]]; then
        echo "git_unlock: not a git repo and no path given" >&2
        return 1
    fi
    local lock="$repo/.git/index.lock"
    if [[ ! -f "$lock" ]]; then
        echo "git_unlock: no lock file at $lock"
        return 0
    fi
    local now mtime age
    now=$(date +%s)
    mtime=$(stat -f %m "$lock" 2>/dev/null || echo 0)
    age=$(( now - mtime ))
    if (( age < 60 )); then
        echo "git_unlock: lock is only ${age}s old — refusing to delete (a git command may be running)" >&2
        return 1
    fi
    if rm -f "$lock"; then
        echo "git_unlock: removed stale lock (age ${age}s) at $lock"
    else
        echo "git_unlock: failed to remove $lock" >&2
        return 1
    fi
}

# Short alias for the common case: clean lock in the current repo.
alias gunlock='git_unlock'

# git-safe: pre-flight stale-lock cleanup, then run git. Use whenever you
# want belt-and-braces protection against orphan locks.
#   git-safe pull --ff-only origin main
git-safe() {
    local repo
    repo="$(git rev-parse --show-toplevel 2>/dev/null)"
    if [[ -n "$repo" ]]; then
        local lock="$repo/.git/index.lock"
        if [[ -f "$lock" ]]; then
            local now mtime age
            now=$(date +%s)
            mtime=$(stat -f %m "$lock" 2>/dev/null || echo 0)
            age=$(( now - mtime ))
            if (( age >= 60 )); then
                rm -f "$lock" && echo "git-safe: cleaned stale lock (age ${age}s)"
            fi
        fi
    fi
    command git "$@"
}
# <<< omega shell setup (managed) <<<
EOF

echo
echo "Wrote managed block to $ZSHRC"
echo
echo "Activate now with:"
echo "    source ~/.zshrc"
echo
echo "Sanity check after sourcing:"
echo "    setopt | grep interactivecomments    # should print 'interactivecomments'"
echo "    type git_unlock                      # should print 'git_unlock is a shell function'"
echo "    type gunlock                         # should print 'gunlock is an alias for git_unlock'"
echo "    type git-safe                        # should print 'git-safe is a shell function'"
