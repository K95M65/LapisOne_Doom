#!/bin/sh
# Installs lapis-doom into your home folder only (kept across Lapis updates). No system files touched.
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
DEST="$HOME/.local/opt/lapis-doom"
mkdir -p "$DEST" "$HOME/.local/bin"
cp -r "$SRC/bin" "$SRC/share" "$DEST/"
cp "$SRC/fetch-shareware.sh" "$SRC/uninstall.sh" "$DEST/"
IWADS="$HOME/.local/share/lapis-doom/iwads"; mkdir -p "$IWADS"
chmod +x "$DEST/bin/"*
ln -sf "$DEST/bin/lapis-doom-launch" "$HOME/.local/bin/doom"
ln -sf "$DEST/bin/lapis-doom-padcheck" "$HOME/.local/bin/doom-padcheck"
ln -sf "$DEST/bin/lapis-doom-usb-allow" "$HOME/.local/bin/doom-usb-allow"
# Bring Your Own WAD; optionally fetch the free shareware episode (checksum-verified).
if ! ls "$IWADS"/*.[wW][aA][dD] >/dev/null 2>&1; then
  ans="$LAPIS_DOOM_FETCH_SHAREWARE"
  if [ -z "$ans" ] && [ -t 0 ]; then
    printf "No WAD in %s. Download the free DOOM shareware (Episode 1) now? [y/N] " "$IWADS"; read ans
  fi
  case "$ans" in
    y|Y|yes|1) sh "$SRC/fetch-shareware.sh" "$IWADS" ;;
    *) echo "Bring your own WAD: copy DOOM.WAD or DOOM2.WAD into $IWADS (or run $DEST/fetch-shareware.sh)." ;;
  esac
fi
echo "Installed to $DEST. Run: ~/.local/bin/doom   (or: doom, if ~/.local/bin is on PATH)"
ln -sf "$DEST/bin/lapis-doom-dock" "$HOME/.local/bin/doom-dock"
# DOOM launcher entry in "What's open": a user service that starts the dock at login (home folder only).
if [ -z "$LAPIS_DOOM_NO_DOCK" ] && command -v systemctl >/dev/null 2>&1; then
  mkdir -p "$HOME/.config/systemd/user"
  cat > "$HOME/.config/systemd/user/lapis-doom-dock.service" <<UNIT
[Unit]
Description=DOOM launcher entry for the Lapis screen
After=default.target

[Service]
ExecStart=%h/.local/opt/lapis-doom/bin/lapis-doom-dock
Restart=on-failure
RestartSec=10

[Install]
WantedBy=default.target
UNIT
  systemctl --user daemon-reload && systemctl --user enable lapis-doom-dock.service >/dev/null 2>&1 && echo "DOOM launcher enabled (systemctl --user disable --now lapis-doom-dock to turn off)"
fi
