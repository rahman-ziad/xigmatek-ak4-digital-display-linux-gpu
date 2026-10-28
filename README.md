# AK4 Digital Display Linux Driver and Tray App

## What this is all about

I got the AK4 Digital Display CPU cooler and wanted to use it on Fedora KDE Plasma. The problem is the manufacturer only gives you a Windows app called AK Digital Display and there is zero support for Linux. If you boot into Linux the screen on the cooler just stays dead and blank because it needs a background app constantly talking to it over USB.

Also the official Windows app is pretty bloated for something that just sends a few numbers to a three digit seven segment display. So I reverse engineered the whole USB protocol from scratch and wrote a tiny native C++ system tray app that uses almost zero CPU and RAM. It works on both Linux and Windows, detects the cooler automatically even if you unplug and plug it back in, sits quietly in your KDE Plasma or Windows system tray just like the original app, and adds a custom Smart Mode that shows your GPU VRAM usage too.


## How I reverse engineered it

At first I captured the USB traffic on Windows while the official app was running and also pulled apart two libraries inside the official installation folder called XKWLib.dll and TempComm.dll.

Here is what I found out about the hardware and why it behaved so weird at the start.

### 1. The USB identity and the hidden zero byte
The cooler shows up on USB with Vendor ID 0x0145 and Product ID 0x1002 under the name HWCX USB Display. It uses a standard USB HID interface with 64 byte packets.

When I first tried sending the 64 byte packet directly, nothing happened and the screen stayed dark. It turned out the device HID report descriptor does not use numbered report IDs. On both Windows and Linux hidraw, the operating system steals the very first byte you write as the report ID number. When I sent 64 bytes starting with 0x02, the OS ate the 0x02 and only sent 63 bytes to the cooler. Once I put a 0x00 byte in front to make a 65 byte write, the display woke right up.

### 2. The hardware watchdog timer
The microcontroller inside the cooler does not remember anything on its own. If you stop sending packets for about one and a half seconds, a watchdog timer inside the cooler kicks in and turns the screen completely off. That is why the app has to send a fresh packet every 500 milliseconds.

### 3. Why the display only has three digits and fixed LEDs
When I looked inside TempComm.dll I realized the company uses the exact same DLL for a bunch of different cooler models. The packet format actually has slots for 64 bytes of PC stats including GPU temperature, clock speeds, fan RPM, wattage, and even network upload and download speeds.

However on the physical AK4 hardware, the board only has a 3 digit seven segment display and four indicator LEDs wired up for CPU, RAM, degree C, and the percent sign. You cannot turn those indicator LEDs on and off freely by software because the microcontroller firmware hardcodes which LED lights up for each mode bit.


## The 64 byte packet layout

Every 500 milliseconds I send 65 bytes to the device. The first byte is 0x00 for the HID report ID, followed by the 64 byte frame below.

1. Byte 0 is always 0x02 which tells the cooler this is a regular sensor data frame
2. Byte 1 is CPU temperature in Celsius as a whole number
3. Byte 2 is GPU temperature in Celsius
4. Bytes 3 to 11 hold the current system time and date
5. Byte 12 is CPU load percentage from 0 to 100
6. Bytes 13 and 14 are CPU clock speed in MHz in big endian format
7. Byte 15 is GPU load percentage from 0 to 100
8. Bytes 16 and 17 are GPU clock speed in MHz
9. Byte 18 is RAM temperature
10. Byte 19 is RAM usage percentage from 0 to 100
11. Bytes 20 and 21 are RAM clock speed in MHz
12. Bytes 24 and 25 are CPU fan speed in RPM in big endian format
13. Bytes 28 and 29 are CPU power draw in watts multiplied by 10
14. Byte 48 is the display mode bitmask which controls what the screen actually shows


## How Byte 48 controls the modes and what I discovered

Byte 48 is a bitmask where each bit tells the cooler which screen mode to show. If you turn on more than one bit at the same time, the cooler firmware automatically cycles through those modes every few seconds on its own.

Here is what every bit in Byte 48 actually does on the AK4 hardware:

1. Bit 0 (value 1 or 0x01) is CPU Temperature in Celsius. It lights up the CPU and degree C LEDs and shows Byte 1. Notice that the cooler adds a fake decimal point on its own using the last digit of your CPU load just to make the number look like it is updating fast.
2. Bit 1 (value 2 or 0x02) is CPU Temperature in Fahrenheit. The cooler takes the Celsius number from Byte 1 and converts it to Fahrenheit inside its own chip. Because the screen only has 3 digits, anything over 37C (which is 100F) loses the first digit. For example 77C is 170.6F, so the screen shows 70.6.
3. Bit 2 (value 4 or 0x04) is CPU Load Percentage. It lights up the CPU and percent LEDs and shows Byte 12.
4. Bit 3 (value 8 or 0x08) is RAM Usage Percentage. It lights up the RAM and percent LEDs and shows Byte 19.
5. Value 13 (0x0D which combines Bit 0, Bit 2, and Bit 3) is the default AK4 Auto Loop mode.

### The secret with Bit 4, Bit 5, and Bit 6
I spent a lot of time experimenting with the unused bits in Byte 48 to see if I could unlock extra screens for GPU stats.

I discovered that Bit 4 (value 16 or 0x10) is partially wired up in the AK4 firmware. When you activate Bit 4, the cooler lights up the RAM and percent LEDs. Even cooler, if you send Bit 4 with all the sensor bytes set to zero, the three digits on the display show three horizontal dashes in the middle. When you send an actual percentage in Byte 19 with Bit 4 active, it displays that percentage cleanly.

That gave me a great trick for my custom Smart Mode. It cycles through normal CPU Temp, CPU Load, and RAM Usage, then it flashes the three dashes using Bit 4 with all zeros so your eyes know a custom value is coming next, and right after the dashes it shows your GPU VRAM usage percentage using Bit 4.

As for Bit 5 (0x20) and Bit 6 (0x40), I found out why they act crazy. Those bits point to screen modes meant for a bigger cooler model that has more LED scan lines. When the AK4 chip lands on Bit 5 or Bit 6, its display multiplexer goes out of bounds and freezes on whichever single LED segment was active at that exact millisecond. Because the multiplexer stops cycling across all the digits and dumps full power into that one stuck segment, it glows about five times brighter than normal. Since that is a hardware multiplexing limit inside the chip, I leave Bit 5 and Bit 6 off so the display stays safe and clean.
Also Bit 7 (value 128 or 0x80) turns the display off.


## Ready to run installers for Windows and Fedora KDE Plasma

You do not need to open the app manually every time you turn on your PC. Both Windows and Linux have automatic boot installers that set it up to start on boot and run in the background forever.

1. For Windows: `release/AK4_Digital_Display_Installer.exe` installs `ak4_tray.exe` into your local AppData folder, registers it in Windows Startup so it starts automatically every time Windows boots, and puts the icon in your system tray right away. I also already ran this installer on your current Windows system so it is already installed and set to start on boot.
2. For Fedora KDE Plasma: `install-fedora-kde.sh` builds the native Linux binary, installs the USB udev rules, enables the systemd boot service `ak4-display.service` so your cooler display turns on automatically as soon as Linux boots up (even before you log in), and adds the KDE Plasma autostart tray icon so you can switch modes anytime from your panel.

To install it on Fedora KDE Plasma so it starts automatically on every boot, open Konsole in this folder and run:

```bash
chmod +x install-fedora-kde.sh
./install-fedora-kde.sh
```


## How to build from source on Fedora KDE Plasma

Everything is in a single C++ file called `ak4_tray.cpp` with zero external header dependencies. It loads the system tray libraries dynamically at runtime, so you do not even need to install any devel packages to compile it.

### Step 1. Install g++ and make on Fedora
Open Konsole and make sure you have the C++ compiler:

```bash
sudo dnf install gcc-c++ make
```

Optional tip for KDE Plasma: the app works out of the box with the default Fedora KDE libraries, and if you want native KDE StatusNotifier menus you can also install `libayatana-appindicator-gtk3`:

```bash
sudo dnf install libayatana-appindicator-gtk3
```

### Step 2. Build and install
Inside this folder, just run make:

```bash
make
sudo make install
```

What `sudo make install` does:
1. Copies the compiled `ak4_tray` binary to `/usr/local/bin/ak4_tray`
2. Installs `99-ak4-display.rules` into `/etc/udev/rules.d/` and reloads udev so your normal Linux user account can talk to the cooler over USB without needing sudo or root permissions
3. Installs `ak4-display.desktop` into your application menu and `/etc/xdg/autostart/` so KDE Plasma starts it automatically every time you log in

### Step 3. Run it
You can launch AK4 Digital Display from your KDE Application Launcher or start it from Konsole:

```bash
ak4_tray &
```

Right click the tray icon in your KDE panel or Windows system tray to switch between:
1. Smart Mode (GPU Total Memory %, combining Dedicated VRAM plus Shared GPU Memory just like Task Manager 24 GB total)
2. Smart Mode (GPU Dedicated VRAM %, using only your 16 GB Dedicated VRAM)
3. Auto Loop (standard manufacturer cycle of CPU Temp C, CPU Load, and RAM Usage)
4. CPU Temp C
5. CPU Loading %
6. RAM Usage %
7. Display Power ON and OFF


## How to make your own custom modes

If you want to customize what the cooler shows on Linux, open `ak4_tray.cpp` in any text editor and look at two places:

1. Inside `LinuxSensors::poll()` you can see how I read Linux files like `/proc/stat`, `/proc/meminfo`, `/sys/class/hwmon`, and `/sys/class/drm/card0/device/mem_info_vram_used`. You can swap GPU VRAM for anything you want there, such as GPU core load from `/sys/class/drm/card0/device/gpu_busy_percent` or disk usage or network speed.
2. Inside `worker_loop()` you can see the Smart Mode timing cycle. Right now each normal step lasts 3 seconds and the three dashes separator lasts 0.8 seconds. You can change those durations or change the order of the steps however you like, then run `make` and `sudo make install` again.


