# Omega Gold-Bracket Bot - Windows VPS Deployment

Deploys the daily + Sunday gold-bracket strategies onto a Windows VPS so they run
independently of a Mac being awake.

## Prerequisites on the VPS

- Windows Server / Windows 10+ with RDP access
- Python 3.12 - python.org installer, tick "Add python.exe to PATH"
- Git for Windows
- IB Gateway **or** TWS installed, logged in, and with the API enabled.
  In IB Gateway/TWS: Configure > Settings > API > Settings >
  "Enable ActiveX and Socket Clients". Default ports the bots expect:
    - daily_bracket.py  -> port 4002  (IB Gateway, paper)
    - sunday_bracket.py -> port 7497  (TWS, paper)  /  7496 (TWS, live)
  Whichever gateway you run, make sure its socket port matches the bot, or the
  bot will fail to connect.

## Step 1 - Get the code onto the VPS

Open PowerShell:

    cd C:\
    git clone https://github.com/Trendiisales/Omega.git
    cd Omega\bracket-bot

## Step 2 - One-time environment setup

    powershell -ExecutionPolicy Bypass -File deploy\windows_setup.ps1

Creates `.venv` and installs ib_insync + flask.

## Step 3 - Set the VPS clock to UTC (recommended)

The strategies fire at fixed UTC times. The simplest reliable setup is to put the
VPS on UTC so Task Scheduler local time equals UTC:

    Set-TimeZone -Id "UTC"

If you keep a non-UTC timezone, edit the `-At` times in `deploy\register_tasks.ps1`.

## Step 4 - Schedule the strategies (Administrator PowerShell)

    powershell -ExecutionPolicy Bypass -File deploy\register_tasks.ps1

Registers three Task Scheduler jobs:

    Omega Daily Bracket 1300   Mon-Fri 13:00 UTC
    Omega Daily Bracket 1400   Mon-Fri 14:00 UTC
    Omega Sunday Bracket       Sunday  22:55 UTC

## Step 5 - Smoke test (places no orders)

    .venv\Scripts\python.exe live\daily_bracket.py --paper --dry-run

A clean run connects to IB, fetches a price, logs it, and exits. If it cannot
connect: IB Gateway is not running, the API is not enabled, or the port is wrong.

## Step 6 - Turn off the Mac cron job

On the Mac, run `crontab -e` and remove the `cron_sunday_bracket.sh` line if it is
present, so the strategy does not run on both machines.

## Notes

- Tasks default to `--paper`. To trade live, change `--paper` to `--live` in
  `deploy\register_tasks.ps1` and re-run it.
- Logs: `bracket-bot\logs\` . Trade records: `bracket-bot\data\trades.ndjson`.
- Dashboard: `.venv\Scripts\python.exe server.py` then open http://localhost:5050
- The VPS must stay powered on; IB Gateway/TWS must stay logged in for the
  scheduled runs to execute.
