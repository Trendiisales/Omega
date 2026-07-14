#!/bin/sh
# PreToolUse hook (Claude Code, wired in .claude/settings.json): block git
# add/stage/commit/push commands whose stderr is suppressed.
#
# WHY (S-2026-07-14ap incident): `git add <list> 2>/dev/null; git commit ...`
# -- the add errored (pathspec), stderr was suppressed, so the commit silently
# captured ONLY pre-staged content. No symptom until post-deploy git status.
# Suppressed stderr on a state-changing git verb = the only failure signal is
# discarded. This hook denies the tool call before it runs.
#
# Reads the Claude hook JSON on stdin, inspects .tool_input.command.
# Exit 0 with no output = allow. Exit 0 with permissionDecision deny = block.

cmd=$(jq -r '.tool_input.command // empty' 2>/dev/null)
[ -z "$cmd" ] && exit 0

# state-changing git verb present? (git add / git -C x add / git stage|commit|push)
printf '%s' "$cmd" | grep -Eq 'git([[:space:]]+-C[[:space:]]+[^[:space:]]+)?[[:space:]]+(add|stage|commit|push)\b' || exit 0

# stderr suppression present? 2>/dev/null, 2> /dev/null, 2>&-, &>/dev/null
printf '%s' "$cmd" | grep -Eq '(2>[[:space:]]*/dev/null|2>&-|&>[[:space:]]*/dev/null)' || exit 0

cat <<'EOF'
{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"BLOCKED by scripts/git_hooks/claude_block_git_stderr_suppress.sh (S-2026-07-14ap trap): git add/commit/push with suppressed stderr silently no-ops on error -- the 8a97427c commit captured only pre-staged content this way. Re-run WITHOUT 2>/dev/null, then verify staging with `git status --short` (no unexpected M lines) before committing."}}
EOF
exit 0
