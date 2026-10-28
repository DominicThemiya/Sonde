# Sonde

Stream your own music folder to any device on your network. `sonded` is a
small server that serves a folder of MP3 and FLAC files, with a web player
you open in a browser or install on your phone as an app. `sonde` is a
command line client that plays a song through a player like mpv.

- One file to run, no dependencies, no database: point it at a folder
- Web player with cover art, tags, search, shuffle and repeat
- Seek and volume dial, and a library wheel on phones
- Installs on a phone's home screen like an app
- Linux and Windows

Download the latest build from
[Releases](https://github.com/DominicThemiya/Sonde/releases).

## Linux

1. Download `sonde-linux-x86_64.tar.gz` and unpack it:

   ```sh
   tar xzf sonde-linux-x86_64.tar.gz
   cd sonde-linux-x86_64
   ```

2. Start the server with your music folder:

   ```sh
   ./sonded ~/Music
   ```

   It prints `Serving /home/you/Music on port 8080` and logs each request.
   Songs are the `.mp3` and `.flac` files directly inside that folder.

3. Open <http://localhost:8080> in a browser.

To open it from other devices, see [Phones and other devices](#phones-and-other-devices).
If you use a firewall such as ufw, allow the port: `sudo ufw allow 8080/tcp`.

### Start it automatically

To have it run in the background and start when you log in, copy `sonded`
to `~/.local/bin`, then create `~/.config/systemd/user/sonded.service`:

```ini
[Unit]
Description=Sonde music server

[Service]
ExecStart=%h/.local/bin/sonded %h/Music
Restart=on-failure

[Install]
WantedBy=default.target
```

Then turn it on:

```sh
systemctl --user enable --now sonded
journalctl --user -u sonded -f    # watch the log
```

## Windows

Needs Windows 10 or 11.

1. Download `sonde-windows-x86_64.zip`, right click it and choose
   **Extract All**.

2. Open the extracted folder, click the address bar, type `cmd` and press
   Enter. That opens a Command Prompt in the folder.

3. Start the server with your music folder:

   ```bat
   sonded.exe "C:\Users\you\Music"
   ```

   - **Windows protected your PC**: the program isn't signed, so SmartScreen
     may stop it the first time. Choose **More info**, then **Run anyway**.
   - **Windows Firewall** asks whether sonded may use the network. Allow it
     on **private networks** so your phone can reach it.

4. Open <http://localhost:8080> in a browser.

Keep the window open while you listen. Closing it stops the server.

### Start it automatically

1. Right click `sonded.exe` and choose **Show more options**, then
   **Create shortcut**.
2. Right click the shortcut, choose **Properties**, and add your music
   folder to the end of **Target**, for example:
   `"C:\Sonde\sonded.exe" "C:\Users\you\Music"`.
   Set **Run** to **Minimized**.
3. Press Win+R, type `shell:startup` and press Enter, then move the
   shortcut into the folder that opens.

## Phones and other devices

The phone and the computer running sonded need to be on the same Wi-Fi.

1. Find the computer's local address:
   - Linux: `ip -4 addr` and look for something like `192.168.1.20`
   - Windows: `ipconfig` and look for **IPv4 Address**
2. On the phone, open `http://192.168.1.20:8080` (with your address).

### Install it as an app

**iPhone**: open the address in Safari, tap **Share**, then
**Add to Home Screen**. It opens full screen with its own icon.

**Android**: Chrome only installs web apps from secure (https) addresses.
On plain Wi-Fi, **Add to Home screen** makes a bookmark that opens in a
browser tab. To install it properly, use Tailscale (below), or tell
Chrome to trust your server:

1. Open `chrome://flags/#unsafely-treat-insecure-origin-as-secure` on the phone
2. Add your address, for example `http://192.168.1.20:8080`, enable it and
   restart Chrome
3. Open the address, then **⋮** → **Add to Home screen** → **Install**

### Listening away from home

[Tailscale](https://tailscale.com) puts your devices on a private network
and gives the server an https address, which also makes it installable on
Android. Install Tailscale on the computer and the phone, sign in to the
same account on both, enable **HTTPS Certificates** in the Tailscale admin
console under DNS, then on the computer run:

```sh
sudo tailscale serve --bg 8080
```

On Windows, run `tailscale serve --bg 8080` from a Command Prompt opened as
administrator.

Then open `https://<computer name>.<your tailnet>.ts.net` on the phone.
`tailscale serve status` shows the exact address.

## The command line client

`sonde` lists the songs on the sonded running on the same computer, asks
which one to play and writes it to standard output, so pipe it into a
player:

```sh
./sonde | mpv -
```

On Windows, use Command Prompt or PowerShell 7.4 or later
(`sonde.exe | mpv -`). Windows PowerShell 5.1 damages audio passed through
a pipe.

## Building from source

Needs GCC 15 or newer (for `#embed`).

```sh
make              # sonded and sonde for Linux
make windows      # sonded.exe and sonde.exe, needs mingw-w64-gcc
```

On Windows itself, use the UCRT64 shell from [MSYS2](https://www.msys2.org):

```sh
pacman -S make mingw-w64-ucrt-x86_64-gcc
make windows WIN_CC=gcc WIN_WINDRES=windres
```

The web player in `web/` is built into `sonded`, so rebuild after changing
it. `make icons` redraws the app icons from `web/icon.svg`.
