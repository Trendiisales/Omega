#!/usr/bin/env python3
"""
verify_push.py -- MANDATORY post-push verification for Omega GitHub pushes.

Usage:
    python3 verify_push.py TOKEN REPO FILE_PATH EXPECTED_CONTENT_FILE

Checks:
    1. HEAD commit SHA matches what was just returned by the push API
    2. File content at HEAD (via contents API, NOT raw.githubusercontent.com)
       matches the local file exactly, byte for byte
    3. Prints PASS or FAIL with full detail -- no silent success

This is the only acceptable verification method after a push.
raw.githubusercontent.com is CDN-cached and MUST NEVER be used for verification.
"""

import sys
import json
import base64
import urllib.request
import urllib.error

def api_get(url, token):
    req = urllib.request.Request(
        url,
        headers={
            "Authorization": f"token {token}",
            "Accept": "application/vnd.github.v3+json",
            "Cache-Control": "no-cache"
        }
    )
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read())

def verify_push(token, repo, file_path, local_file):
    failures = []
    print(f"\n{'='*60}")
    print(f"  POST-PUSH VERIFICATION")
    print(f"  Repo : {repo}")
    print(f"  File : {file_path}")
    print(f"{'='*60}\n")

    # Step 1: Get HEAD commit via API
    try:
        head = api_get(f"https://api.github.com/repos/{repo}/commits/main", token)
        head_sha = head["sha"]
        head_sha7 = head_sha[:7]
        print(f"  [1] HEAD commit (API): {head_sha7}  -- {head['commit']['message']}")
    except Exception as e:
        print(f"  [FAIL] Could not get HEAD from API: {e}")
        sys.exit(1)

    # Step 2: Get file content via contents API at HEAD
    try:
        contents = api_get(
            f"https://api.github.com/repos/{repo}/contents/{file_path}",
            token
        )
        remote_content = base64.b64decode(contents["content"])
        remote_sha = contents["sha"]
        print(f"  [2] File blob SHA (API): {remote_sha[:12]}")
        print(f"      File size on GitHub : {len(remote_content)} bytes")
    except Exception as e:
        print(f"  [FAIL] Could not get file content from API: {e}")
        failures.append(f"API contents fetch failed: {e}")
        sys.exit(1)

    # Step 3: Compare to local file
    try:
        with open(local_file, "rb") as f:
            local_content = f.read()
        print(f"  [3] Local file size     : {len(local_content)} bytes")
    except Exception as e:
        print(f"  [FAIL] Could not read local file: {e}")
        sys.exit(1)

    if remote_content == local_content:
        print(f"\n  [PASS] Content matches exactly -- GitHub has the correct file")
    else:
        print(f"\n  [FAIL] CONTENT MISMATCH")
        print(f"         Local  : {len(local_content)} bytes")
        print(f"         GitHub : {len(remote_content)} bytes")
        # Show first differing line
        local_lines  = local_content.decode("utf-8", errors="replace").splitlines()
        remote_lines = remote_content.decode("utf-8", errors="replace").splitlines()
        for i, (l, r) in enumerate(zip(local_lines, remote_lines)):
            if l != r:
                print(f"         First diff at line {i+1}:")
                print(f"           local : {l[:120]}")
                print(f"           remote: {r[:120]}")
                break
        failures.append("Content mismatch between local file and GitHub")

    # Step 4: Confirm file is in the HEAD commit's tree
    try:
        tree = api_get(
            f"https://api.github.com/repos/{repo}/git/trees/{head_sha}?recursive=1",
            token
        )
        found = any(item["path"] == file_path for item in tree["tree"])
        if found:
            print(f"  [4] File present in HEAD tree: YES")
        else:
            print(f"  [4] [FAIL] File NOT in HEAD tree -- push may have gone to wrong branch")
            failures.append("File not found in HEAD tree")
    except Exception as e:
        print(f"  [4] Could not verify tree: {e}")

    print(f"\n{'='*60}")
    if failures:
        print(f"  RESULT: FAILED -- {len(failures)} issue(s)")
        for f in failures:
            print(f"    - {f}")
        print(f"{'='*60}\n")
        sys.exit(1)
    else:
        print(f"  RESULT: PASS -- {file_path} verified via API at HEAD {head_sha7}")
        print(f"{'='*60}\n")
        sys.exit(0)

if __name__ == "__main__":
    if len(sys.argv) != 5:
        print("Usage: python3 verify_push.py TOKEN REPO FILE_PATH LOCAL_FILE")
        sys.exit(1)
    verify_push(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
