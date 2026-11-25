#!/bin/sh
systemctl --user disable --now lapis-doom-dock.service >/dev/null 2>&1 || true
rm -f "$HOME/.config/systemd/user/lapis-doom-dock.service"; systemctl --user daemon-reload >/dev/null 2>&1 || true
pkill -x lapis-doom 2>/dev/null || true
rm -f "$HOME/.local/bin/doom" "$HOME/.local/bin/doom-padcheck" "$HOME/.local/bin/doom-usb-allow" "$HOME/.local/bin/doom-dock"
rm -rf "$HOME/.local/opt/lapis-doom"
echo "Removed lapis-doom. Saves/config remain in ~/.local/share/lapis-doom (delete it to remove them too)."
