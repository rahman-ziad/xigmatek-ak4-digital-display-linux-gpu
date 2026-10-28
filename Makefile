CXX = g++
CXXFLAGS = -O2 -std=c++17 -Wall -pthread
LDFLAGS = -ldl

TARGET = ak4_tray

all: $(TARGET)

$(TARGET): ak4_tray.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) ak4_tray.cpp $(LDFLAGS)

install: $(TARGET)
	sudo install -m 755 $(TARGET) /usr/local/bin/ak4_tray
	sudo install -m 644 99-ak4-display.rules /etc/udev/rules.d/99-ak4-display.rules
	sudo install -m 644 ak4-display.service /etc/systemd/system/ak4-display.service
	sudo install -m 644 ak4-display.desktop /usr/share/applications/ak4-display.desktop
	sudo install -m 644 ak4-display.desktop /etc/xdg/autostart/ak4-display.desktop
	sudo udevadm control --reload-rules
	sudo udevadm trigger
	sudo systemctl daemon-reload
	sudo systemctl enable --now ak4-display.service

uninstall:
	sudo systemctl disable --now ak4-display.service || true
	sudo rm -f /etc/systemd/system/ak4-display.service
	sudo rm -f /usr/local/bin/ak4_tray
	sudo rm -f /etc/udev/rules.d/99-ak4-display.rules
	sudo rm -f /usr/share/applications/ak4-display.desktop
	sudo rm -f /etc/xdg/autostart/ak4-display.desktop
	sudo systemctl daemon-reload
	sudo udevadm control --reload-rules

clean:
	rm -f $(TARGET)
