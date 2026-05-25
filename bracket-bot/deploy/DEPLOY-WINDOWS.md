# Omega Gold-Bracket Bot - Windows VPS Deployment

Deploys the daily + Sunday gold-bracket strategies onto a Windows VPS so they run
independently of a Mac being awake.

IMPORTANT: every command below runs from the bracket-bot folder:
    C:\Omega\bracket-bot
The bot's files (live\, server.py, deploy\, requirements.txt) live there - NOT in
C:\Omega.

## Prerequisites on the VPS

- Windows Server / Windows 10+ with RDP access
- Python 3.12 and Git installed.
    `winget` is NOT present on many Windows Server builds. If `winget` works you may
    use it; otherwise install manually:
      Python 3.12 - https://www.python.org/downloads/  (tick "Add python.exe to PATH")
      Git         - https://git-scm.com/download/win
- IB Gateway or TWS installed, logged in, with the API enabled:
  Configure > Settings > API > Settings > "Enable ActiveX and Socket Clients".
  Ports the bots expect:
      daily_bracket.py  -> 4002  (IB Gateway, paper)
      sunday_bracket.py -> 7497  (TWS, paper)  /  7496 (TWS, live)

## Step 1 - Get the code

    cd C:\
    git clone https://github.com/Trendiisales/Omega.git
    cd C:\Omega\bracket-bot

## Step 2 - Environment setup

    powershell -ExecutionPolicy Bypass -File deploy\windows_setup.ps1

Creates .venv inside bracket-bot and installs ib_insync + flask. The script locates
Python automatically even if it is not on PATH.

## Step 3 - Set the VPS clock to UTC

    Set-TimeZone -Id "UTC"

## Step 4 - Schedule the strategies (Administrator PowerShell)

    cd C:\Omega\bracket-bot
    powershell -ExecutionPolicy Bypass -File deploy\register_tasks.ps1

Registers:
    Omega Daily Bracket 1300   Mon-Fri 13:00 UTC
    Omega Daily Bracket 1400   Mon-Fri 14:00 UTC
    Omega Sunday Bracket       Sunday  22:55 UTC

## Step 5 - Smoke test (places no orders)

    .venv\Scripts\python.exe live\daily_bracket.py --paper --dry-run

A clean run connects to IB, fetches a price, logs it, and exits. If it cannot
connect: IB Gateway is not running, the API is not enabled, or the port is wrong.

## Step 6 - Turn off the Mac cron job

On the Mac, run `crontab -e` and remove the cron_sunday_bracket.sh line if present.

## Notes

- Tasks default to --paper. To trade live, change --paper to --live in
  deploy\register_tasks.ps1 and re-run it.
- Dashboard: .venv\Scripts\python.exe server.py  then open http://localhost:5050
- Logs: bracket-bot\logs\ . Trade records: bracket-bot\data\trades.ndjson.
- The VPS must stay powered on and IB Gateway/TWS logged in for scheduled runs.
