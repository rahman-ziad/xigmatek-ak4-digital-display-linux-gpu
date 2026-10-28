#!/usr/bin/env bash
set -e

echo "Checking Fedora C++ compiler and KDE Plasma tray libraries..."
if ! command -v g++ >/dev/null 2>&1; then
    sudo dnf install -y gcc-c++ make libayatana-appindicator-gtk3
fi

mkdir -p release
echo "Building native Fedora KDE Plasma release binary..."
g++ -O2 -std=c++17 -Wall -pthread ak4_tray.cpp -o release/ak4_tray -ldl
cp release/ak4_tray ak4_tray

echo "Installing binary, USB udev rules, systemd boot service, and KDE Plasma autostart..."
sudo install -m 755 release/ak4_tray /usr/local/bin/ak4_tray
sudo install -m 644 99-ak4-display.rules /etc/udev/rules.d/99-ak4-display.rules
sudo install -m 644 ak4-display.service /etc/systemd/system/ak4-display.service
sudo install -m 644 ak4-display.desktop /usr/share/applications/ak4-display.desktop
sudo install -m 644 ak4-display.desktop /etc/xdg/autostart/ak4-display.desktop
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo systemctl daemon-reload
sudo systemctl enable --now ak4-display.service

echo "Starting AK4 Digital Display tray icon in your KDE Plasma panel..."
pkill -f "/usr/local/bin/ak4_tray$" 2>/dev/null || true
nohup /usr/local/bin/ak4_tray >/dev/null 2>&1 &
echo "Installed! The display service is now enabled on boot and running in your KDE system tray."
