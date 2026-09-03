# Scan button (brscan-scand)

The printer's physical **Scan** button can send a scan to this Mac. You press
Scan on the device, pick this Mac and a destination (File, Image, OCR, or
E-mail), and the scan is pulled and handled here — no software running on the
Mac's screen at the time.

`brscan-scand` is the background agent that makes this work. It registers this
Mac with the printer, waits for a button press, pulls the scan, and performs
the destination's action.

## What each destination does

The destination you choose on the printer selects both the scan settings (from
your config) and what happens to the finished scan:

| Destination | Action |
|---|---|
| **File** | Save the scan to your save directory. |
| **Image** | Save it, then open it (in its default app -- usually Preview -- or an app you configure). |
| **OCR** | Recognize the text with the macOS Vision framework and save a searchable PDF — the image with a selectable, copyable text layer over it. |
| **E-mail** | Save it, then open a new Mail message with the scan attached, ready for you to review and send. It is never sent automatically. |

## How it works

The Scan button uses Brother's own registration-and-notification mechanism,
which this project reverse-engineered clean-room from packet captures (see
[PROTOCOL.md](PROTOCOL.md) and `PROVENANCE.md`). In outline:

1. **Register.** The daemon sends the printer an SNMP Set (UDP 161) for each
   destination, telling it this Mac's name, address, and UDP listening port.
   Registrations expire, so the daemon re-sends them every few minutes.
2. **Notify.** When you press Scan and pick this Mac, the printer sends a UDP
   notification to the daemon on port 54925, naming the destination you chose.
3. **Acknowledge.** The daemon echoes the notification back to confirm receipt.
   It ignores any notification that doesn't come from the configured printer.
4. **Pull and act.** The daemon opens the scan connection (TCP 54921), pulls
   the image, saves it, and runs the destination's action.

## Setup

### 1. Build the daemon

Build the project as described in the [README](../README.md). This produces
`build/brscan-scand` alongside `build/brscan-cli`.

### 2. Install the binary

Copy the binary somewhere stable that the LaunchAgent can point at. The
example plist uses `/usr/local/bin`:

```bash
sudo cp build/brscan-scand /usr/local/bin/brscan-scand
```

If you'd rather not use `sudo`, put it anywhere you own (for example
`~/bin/brscan-scand`) and use that absolute path in the plist's
`ProgramArguments` in the next steps.

### 3. Write the config

Copy the example config and edit it. At minimum, set `printer_host`:

```bash
mkdir -p ~/.config
cp config/brscan-scand.conf.example ~/.config/brscan-scand.conf
```

Find your printer's name with `dns-sd -B _scanner._tcp` (see the README's
"Finding your scanner"), then set `printer_host` in the config. Every other
setting has a default; see the comments in the example file for the full list,
including per-destination mode, resolution, and source, and the Image and
E-mail action settings.

You can check your config before installing the agent by running the daemon in
the foreground:

```bash
build/brscan-scand --config ~/.config/brscan-scand.conf
```

It prints its startup settings and logs each registration and button press.
Press Ctrl-C to stop. Approve the network prompts described under
[Firewall](#firewall) the first time.

### 4. Install the LaunchAgent

`brscan-scand` runs as a **LaunchAgent** — an agent inside your GUI login
session — because the Image, E-mail, and OCR actions open apps and windows on
your desktop. Install it for just your user (no admin rights needed):

```bash
cp config/com.brscan.scand.plist.example ~/Library/LaunchAgents/com.brscan.scand.plist
```

Or install it for all users, which needs admin rights:

```bash
sudo cp config/com.brscan.scand.plist.example /Library/LaunchAgents/com.brscan.scand.plist
```

Open the copied plist and set the first `ProgramArguments` entry to the path
where you installed the binary in step 2, and replace `YOUR_USERNAME` in the
two log paths with your short user name (launchd doesn't expand `~`). Load it
only after your config from step 3 is in place: the agent is set to relaunch if
it exits, so with a missing or invalid `printer_host` it exits at startup and
launchd keeps restarting it (every few seconds) until the config is valid. The
log names the reason each time.

Then load it, using whichever path you copied the plist to in step 4:

```bash
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.brscan.scand.plist
```

The agent starts immediately and again at every login. Within a few seconds,
this Mac appears in the printer's Scan menu.

To stop and unload it:

```bash
launchctl bootout gui/$(id -u)/com.brscan.scand
```

After editing the plist or the config, unload and reload it (bootout, then
bootstrap) for the change to take effect.

## Firewall

The daemon needs three network paths, so approve it if macOS or a third-party
firewall prompts on first run:

- Outbound **UDP 161** (SNMP) — to register with the printer.
- Outbound **TCP 54921** — to pull each scan.
- Inbound **UDP 54925** — to receive button-press notifications.

Without the inbound path, registration still succeeds and this Mac appears in
the Scan menu, but button presses never arrive and nothing scans.

## Troubleshooting

- **This Mac doesn't appear in the printer's Scan menu.** Check that the daemon
  is running (`launchctl list | grep brscan`) and that `printer_host` is
  correct and reachable. Watch the log for `[register] ... -> sent`.
- **The Mac appears, but pressing Scan does nothing.** The notification isn't
  reaching the daemon — check the inbound UDP 54925 firewall path above, and
  that both devices are on the same network.
- **Nothing in the logs at all.** Confirm the plist's `ProgramArguments` path
  points at the actual binary, and check the `StandardErrorPath` you set (the
  example uses `~/Library/Logs/brscan-scand.log`).
- **A press is logged but the scan fails.** The log line names the reason (for
  example, the scanner is busy or the document feeder is empty). The daemon
  keeps running and handles the next press.
