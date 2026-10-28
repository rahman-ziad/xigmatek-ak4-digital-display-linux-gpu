Name:           ak4-display
Version:        1.0.0
Release:        1%{?dist}
Summary:        AK4 Digital Display USB Cooler Controller and Tray App for Linux

License:        MIT
URL:            https://github.com/user/ak4-display
Source0:        ak4_tray.cpp
Source1:        99-ak4-display.rules
Source2:        ak4-display.desktop
Source3:        ak4-display.service

BuildRequires:  gcc-c++
BuildRequires:  systemd-rpm-macros
Requires:       gtk3
Recommends:     libayatana-appindicator-gtk3

%description
Lightweight native C++17 system tray application and systemd boot service for
the AK4 Digital Display CPU cooler (0145:1002) on Fedora KDE Plasma and Linux.

%prep

%build
g++ -O2 -std=c++17 -Wall -pthread %{SOURCE0} -o ak4_tray -ldl

%install
install -D -m 0755 ak4_tray %{buildroot}%{_bindir}/ak4_tray
install -D -m 0644 %{SOURCE1} %{buildroot}%{_udevrulesdir}/99-ak4-display.rules
install -D -m 0644 %{SOURCE2} %{buildroot}%{_datadir}/applications/ak4-display.desktop
install -D -m 0644 %{SOURCE2} %{buildroot}%{_sysconfdir}/xdg/autostart/ak4-display.desktop
install -D -m 0644 %{SOURCE3} %{buildroot}%{_unitdir}/ak4-display.service

%post
%udev_rules_update
%systemd_post ak4-display.service
systemctl enable --now ak4-display.service >/dev/null 2>&1 || :

%preun
%systemd_preun ak4-display.service

%postun
%udev_rules_update
%systemd_postun_with_restart ak4-display.service

%files
%{_bindir}/ak4_tray
%{_udevrulesdir}/99-ak4-display.rules
%{_datadir}/applications/ak4-display.desktop
%{_sysconfdir}/xdg/autostart/ak4-display.desktop
%{_unitdir}/ak4-display.service
