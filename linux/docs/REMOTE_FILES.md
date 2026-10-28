# Remote Files (FTP / SFTP)

A plugin bundled with the GTK port, in the spirit of
[NppFTP](https://ashkulz.github.io/NppFTP/): browse a server in a docked
panel, open its files in Notepad++, and send them back when you save. It is
not a port of NppFTP's code. It is written for this port on the plugin SDK,
GTK and POSIX only: FTP is spoken directly on sockets, SFTP goes through
the system's OpenSSH `sftp` client, and the shell link (as in Midnight
Commander) through its `ssh` client.

Source: `linux/plugins/NppRemote`. It is installed with the editor as
`<libdir>/notepad-plus-plus-gtk/plugins/NppRemote/NppRemote.so` and shows up
under **Plugins ▸ Remote Files (FTP/SFTP)**.

## Using it

1. **Plugins ▸ Remote Files ▸ Show Remote Files Panel** (or the toolbar
   button) docks the panel on the right.
2. The gear button (or **Edit Profiles…**) opens the profile editor: name,
   protocol, host, port, user, password, initial folder (absolute, or
   relative to the login folder: `~`, `~/www`, `www`), timeout, and per
   protocol FTP active mode or the SSH key and extra ssh options.
3. The Connect button lists the profiles. The tree opens on the initial
   folder (or the login folder). The path field above it goes to any folder,
   and the up arrow goes to the parent.
4. **Double-click a file** to download it and open it. **Save** it
   (Ctrl+S) to upload it back to the same place. The log at the bottom
   shows every command and reply. The upload comes first: if the server
   refuses it (a file owned by root or read-only there: "Permission
   denied"), nothing is saved, the tab stays modified, and the error says
   why. Notepad++ takes no input while the upload runs; after a moment a
   dialog shows it, with Stop.

The context menu of the tree has: Open, Download Again (reloads an open
copy, asks first if it has unsaved changes), Save a Copy As…, Upload Current
Document Here, Upload File Here… (several files at once), New File…, New
Folder…, Rename… (F2), Delete (Del; a folder with everything in it, after
asking), Refresh (F5), Copy Path and Make It the Top Folder. The Stop button
cancels the running transfer.

Opened files are kept in a cache that mirrors the server:
`~/.cache/notepad-plus-plus-gtk/NppRemote/<profile>/<remote path>`. The
path is the link back to the server: a cached file reopened from the last
session still uploads on save. So that a save is never only local, the
plugin uploads the document before Notepad++ writes it and stops the save
when the upload fails (the port's `NPPM_GTKCANCELSAVE`, see
[PLUGINS.md](PLUGINS.md)). If no connection is open, saving connects to
its profile. If another profile is connected, Notepad++ asks before
switching. An idle connection that the server dropped is reopened, and the
command is tried once more.

## Protocols

| | |
|---|---|
| **FTP** | Built in. Passive mode (EPSV, then PASV, connecting to the control connection's address so that servers behind NAT work) or active mode (PORT, EPRT on IPv6); binary transfers; `MLSD` listings when the server has them, else `LIST` (Unix and Windows/IIS formats); UTF-8 names. Leave the user empty for anonymous FTP. |
| **SFTP** | Runs OpenSSH's `sftp` (package `openssh-client`), so `~/.ssh/config`, host aliases, keys, `ssh-agent`, `known_hosts`, `ProxyJump` and the like work as in a terminal. Password and passphrase prompts and unknown host keys are asked in Notepad++ dialogs. Extra arguments go in the profile's **sftp options** field (for example `-o ProxyJump=gateway`). |
| **Shell link** | Midnight Commander's "Shell link" (FISH): `ssh` runs `/bin/sh` on the server and files go through plain shell commands, `ls -lan`, `cat`, `printf`, `mkdir`, `rmdir`, `mv`, `rm`. For servers without the SFTP subsystem: routers, busybox and dropbear systems, locked-down hosts. Logs in like SFTP (same key, options and prompts). Saving writes the file in place, so it keeps its mode and owner. Whole files pass through memory, so it is meant for files you edit, not for large transfers. |
| FTPS (FTP over TLS) | Not supported: it needs a TLS library. Use SFTP. |

How SFTP works: `sftp` reads its commands from a socket, and its output and
errors go to pipes. A pseudo-terminal (`posix_openpt`) is its controlling
terminal, which is where ssh asks its questions. Every command is followed
by a comment line, and `sftp` echoes each line it reads, so the echo marks
where the command's output ends. Whatever arrived on stderr by then is the
command's error. `SSH_ASKPASS_REQUIRE` is set to `never`, so desktop askpass
helpers never pop up in its place.

How the shell link works: `ssh -T` (no terminal on the server, so nothing
is echoed and bytes pass unchanged) runs `exec /bin/sh`, whatever the login
shell is, with the same pipes and pseudo-terminal. Every command reads
`/dev/null` and is followed by `printf '\n###<nonce>-N %s\n' "$?"`: that
line, with a random nonce, ends the command's output (a downloaded file's
too) and carries its exit status. Uploads are `printf` commands with octal
escapes for the bytes that need them; `printf` is a builtin of dash, bash and
busybox, so there is no argument length limit. Names are single-quoted, so
nothing in them is expanded.

## Passwords

A password is asked when the server wants one and kept for the rest of the
session. It is written to disk only when **Save the password** is ticked,
and then as plain text in
`~/.config/notepad-plus-plus-gtk/plugins/config/NppRemote.ini`, a file
created readable by its owner only (mode 0600). For SFTP, prefer keys and
`ssh-agent`.

## Sandboxes

In Flatpak and snap builds, SFTP needs an `sftp` binary inside the sandbox.
FTP works everywhere the sandbox allows network access.

## Tests

- `remote_files`: listing and reply parsers. The FTP client against an FTP
  server run by the test (EPSV/MLSD, PASV/LIST with a NAT address, active
  mode, wrong password, refused connection). The worker reconnecting after
  the server drops the line. With OpenSSH installed, the SFTP client against
  a private `sshd` started on a free port, with a passphrase-protected key
  and an unknown host key (accepted, cancelled, rejected); the shell link
  against the same `sshd` (names with quotes, `$(...)` and line breaks,
  files without a final line break, the file mode kept on save), and its
  upload script run by `/bin/sh`, dash, bash and busybox on every byte
  value.
- `remote_files_panel`: the plugin loaded into a live window. Saving a
  cached file connects and uploads it, the panel lists the server, a
  double-click opens a file in a tab, and saving it uploads the change
  before the save returns; a save the server refuses leaves the tab
  modified and both copies as they were, and goes through once allowed.
