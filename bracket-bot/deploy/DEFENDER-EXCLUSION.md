# Windows Defender exclusion for bracket-bot venv

## Why

`pip install flask` (and any other package with a console-script entry point)
fails on this VPS with:

    ValueError: Unable to find resource t64.exe in package pip._vendor.distlib

The root cause is Windows Defender quarantining pip's launcher stub
(`t64.exe`) the moment pip stages it. `ib_insync` installed cleanly because
it has no console scripts — no launcher stub to quarantine.

`pip install --upgrade pip` is also affected and corrupts the venv on this
VPS. **Do not** run it. The bundled pip works.

## Fix (Administrator PowerShell)

    # Exclude the bracket-bot venv from Defender real-time scanning.
    Add-MpPreference -ExclusionPath "C:\Omega\bracket-bot\.venv"

    # Verify the exclusion landed.
    (Get-MpPreference).ExclusionPath

    # Install the dashboard now that the venv is excluded.
    cd C:\Omega\bracket-bot
    .\.venv\Scripts\python.exe -m pip install -r requirements.lock --no-deps

## Verify

    .\.venv\Scripts\python.exe -c "import flask, ib_insync; print(flask.__version__, ib_insync.__version__)"

Expected:  `3.1.3 0.9.86`

## Rollback

Remove the exclusion if/when this VPS is repurposed:

    Remove-MpPreference -ExclusionPath "C:\Omega\bracket-bot\.venv"

## Notes

- The exclusion narrows scanning, not write protection. Defender still scans
  files moved *out* of the excluded path.
- Do not exclude `C:\Omega` wholesale — the Omega C++ engine writes
  unsigned binaries; whole-tree exclusion would mask real malware.
