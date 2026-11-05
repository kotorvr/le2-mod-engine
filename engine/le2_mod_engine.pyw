"""Lone Echo II Mod Engine.

Installs the loader (winmm.dll) next to loneecho2.exe, and edits:
  * bin\\win10\\le2mods.ini                    which mods the loader applies (god mode is picked up live)
  * %LOCALAPPDATA%\\rad\\loneecho2\\settings.json the game's own graphics settings (backed up before the first change)
  * the NVIDIA driver profile "Lone Echo II"   optional, through nvidia_profile.py (asks for admin)

Standard library only. Run with pythonw (double-click) or python.
"""
import configparser
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP = "Lone Echo II Mod Engine"
HERE = pathlib.Path(__file__).resolve().parent
LOADER = HERE / "winmm.dll"
FEATURES_FILE = HERE / "features.json"
STATE_DIR = pathlib.Path(os.environ.get("APPDATA", HERE)) / "LE2ModEngine"
STATE_FILE = STATE_DIR / "state.json"
SETTINGS = pathlib.Path(os.environ.get("LOCALAPPDATA", "")) / "rad" / "loneecho2" / "settings.json"
SETTINGS_BACKUP = SETTINGS.with_name("settings.le2modengine-backup.json")

# (id, label) in the order of the original Mod Engine window.
MODS = [
    ("god_mode", "God mode"),
    ("cure_gun_permanent", "Cure Gun: permanent neutralization"),
    ("all_gadgets", "All gadgets"),
    ("max_upgrades", "Maximum upgrades"),
    ("boost_brake_movement", "Boost, brake, movement"),
    ("unlimited_thrusters", "Unlimited thrusters"),
    ("signal_boundary_limit", "Signal and boundary limit"),
    ("web_receivers_disabled", "Web receivers stay disabled"),
    ("tool_range_1000", "Tool range 1000"),
    ("keypad_any_code", "Any four digit keypad code"),
]

PERF = [
    ("perf_default_caps", "mods",
     "Don't cap default texture quality on 6-8 GB GPUs / 8-12 GB RAM",
     "When the game builds its default settings it forces textures to Medium below 8000 MB VRAM, Low below 6000 MB, "
     "and lowers the preset below 16 GB RAM. This removes those caps."),
    ("no_power_throttling", "perf",
     "Stop Windows power throttling (EcoQoS) for the game",
     "Windows 11 can run background game threads (streaming, audio) on efficiency settings. This opts the game out."),
]

# Graphics settings the game reads from settings.json. Values are the ones the game itself uses, plus clearly marked
# experimental steps. key path -> (label, choices[(value, text)])
LOOKS = [
    (("graphics", "quality", "textures"), "Texture quality", [(0, "Low"), (1, "Medium"), (2, "High")]),
    (("graphics", "quality", "meshes"), "Mesh quality", [(0, "Low"), (1, "Medium"), (2, "High")]),
    (("graphics", "quality", "shadowresolution"), "Shadow map size",
     [(768, "768 (game default)"), (1024, "1024"), (2048, "2048 (experimental)")]),
    (("graphics", "quality", "cascaderesolution"), "Sun shadow cascade size",
     [(1024, "1024"), (2048, "2048 (game default)"), (4096, "4096 (experimental)")]),
    (("graphics", "quality", "cascadedistance"), "Sun shadow distance",
     [(1.0, "1.0x (game default)"), (1.5, "1.5x (experimental)"), (2.0, "2.0x (experimental)")]),
    (("graphics", "msaa"), "MSAA", [(0, "Off (game default)"), (2, "2x"), (4, "4x (experimental)")]),
    (("graphics", "multires"), "Lower-res edges of view (multires)",
     [(True, "On (game default, faster)"), (False, "Off (sharper edges)")]),
    (("graphics", "adaptiveresolution"), "Adaptive resolution",
     [(True, "On (game default)"), (False, "Off (fixed resolution)")]),
    (("graphics", "adaptiveresminscale"), "Adaptive resolution floor",
     [(0.7, "70% (game default)"), (0.8, "80%"), (0.9, "90%")]),
    (("graphics", "sharpening"), "Sharpening",
     [(0.0, "Off"), (1.0, "1.0"), (2.0, "2.0 (game default)")]),
]


# ------------------------------------------------------------------ game folder
def find_game_candidates():
    roots = [
        pathlib.Path(r"C:\Program Files\Oculus\Software\Software\ready-at-dawn-lone-echo-2"),
        pathlib.Path.home() / "Desktop" / "ready-at-dawn-lone-echo-2",
    ]
    for drive in "CDEFG":
        roots.append(pathlib.Path(f"{drive}:\\Oculus\\Software\\ready-at-dawn-lone-echo-2"))
        roots.append(pathlib.Path(f"{drive}:\\Oculus Apps\\Software\\ready-at-dawn-lone-echo-2"))
    return [r for r in roots if (r / "bin" / "win10" / "loneecho2.exe").is_file()]


def load_state():
    try:
        return json.loads(STATE_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_state(state):
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    STATE_FILE.write_text(json.dumps(state, indent=2), encoding="utf-8")


def load_features():
    try:
        return json.loads(FEATURES_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


# ------------------------------------------------------------------ settings.json
def read_settings():
    return json.loads(SETTINGS.read_text(encoding="utf-8"))


def get_path(data, path):
    for key in path:
        if not isinstance(data, dict) or key not in data:
            return None
        data = data[key]
    return data


def same_value(a, b):
    """The game writes floats as float32 (0.7 -> 0.69999999), so compare numbers loosely."""
    if isinstance(a, bool) or isinstance(b, bool):
        return a is b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) < 1e-4
    return a == b


def set_path(data, path, value):
    for key in path[:-1]:
        data = data.setdefault(key, {})
    data[path[-1]] = value


class ModEngine(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP)
        self.resizable(False, False)
        self.state_data = load_state()
        self.features = load_features()
        self.game = None
        self.vars = {}
        self.look_vars = {}
        self._build()
        self._pick_initial_game()
        self.after(1500, self._poll_log)

    # -------------------------------------------------------------- layout
    def _build(self):
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Title.TLabel", font=("Segoe UI", 15, "bold"))
        style.configure("Small.TLabel", font=("Segoe UI", 9), foreground="#555")

        outer = ttk.Frame(self, padding=(22, 16, 22, 12))
        outer.grid(sticky="nsew")
        ttk.Label(outer, text="Mod Engine", style="Title.TLabel").grid(sticky="w")

        game_row = ttk.Frame(outer)
        game_row.grid(sticky="ew", pady=(6, 4))
        self.game_label = ttk.Label(game_row, text="Game: not found", style="Small.TLabel", width=58)
        self.game_label.grid(row=0, column=0, sticky="w")
        ttk.Button(game_row, text="Browse...", command=self._browse).grid(row=0, column=1, padx=(6, 0))

        install_row = ttk.Frame(outer)
        install_row.grid(sticky="ew", pady=(0, 8))
        self.install_label = ttk.Label(install_row, text="", width=40)
        self.install_label.grid(row=0, column=0, sticky="w")
        self.install_button = ttk.Button(install_row, text="Install", command=self._toggle_install)
        self.install_button.grid(row=0, column=1, padx=(6, 0))

        tabs = ttk.Notebook(outer)
        tabs.grid(sticky="nsew")
        tabs.add(self._mods_tab(tabs), text="Mods")
        tabs.add(self._perf_tab(tabs), text="Performance")
        tabs.add(self._looks_tab(tabs), text="Looks")

        ttk.Separator(outer).grid(sticky="ew", pady=(12, 8))
        self.status = ttk.Label(outer, text="Ready")
        self.status.grid(sticky="w")

    def _mods_tab(self, parent):
        tab = ttk.Frame(parent, padding=(10, 10))
        ttk.Label(tab, text="God mode changes now. Other switches apply on the next game launch.",
                  wraplength=460).grid(sticky="w", pady=(0, 10))
        for fid, label in MODS:
            var = tk.BooleanVar(value=False)
            self.vars[fid] = var
            ready = fid in self.features
            text = label if ready else f"{label}  (not in this build yet)"
            cb = ttk.Checkbutton(tab, text=text, variable=var, command=self._write_ini)
            if not ready:
                cb.state(["disabled"])
            cb.grid(sticky="w", pady=3)
        return tab

    def _perf_tab(self, parent):
        tab = ttk.Frame(parent, padding=(10, 10))
        ttk.Label(tab, text="Replaces VEGA Patch. Nothing here touches your NVIDIA driver database.",
                  wraplength=460).grid(sticky="w", pady=(0, 10))
        for key, _section, label, help_text in PERF:
            var = tk.BooleanVar(value=True)
            self.vars[key] = var
            ttk.Checkbutton(tab, text=label, variable=var, command=self._write_ini).grid(sticky="w", pady=(4, 0))
            ttk.Label(tab, text=help_text, style="Small.TLabel", wraplength=440).grid(sticky="w", padx=(24, 0))
        ttk.Separator(tab).grid(sticky="ew", pady=10)
        ttk.Label(tab, text="NVIDIA driver profile (optional, asks for admin):").grid(sticky="w")
        row = ttk.Frame(tab)
        row.grid(sticky="w", pady=(4, 0))
        ttk.Button(row, text="Apply VR profile", command=lambda: self._nvidia("apply")).grid(row=0, column=0)
        ttk.Button(row, text="Restore driver defaults", command=lambda: self._nvidia("restore")).grid(
            row=0, column=1, padx=(6, 0))
        ttk.Label(tab, text="Sets 'Power management: Prefer maximum performance' on the Lone Echo II profile only, "
                            "so the GPU doesn't downclock between frames. VEGA's texture-filter/LOD-bias settings "
                            "are left out on purpose: the driver ignores them for DX12 games like this one.",
                  style="Small.TLabel", wraplength=440).grid(sticky="w", pady=(4, 0))
        return tab

    def _looks_tab(self, parent):
        tab = ttk.Frame(parent, padding=(10, 10))
        ttk.Label(tab, text="Edits the game's settings.json (close the game first). "
                            "Your original is backed up the first time.", wraplength=460).grid(
            columnspan=2, sticky="w", pady=(0, 10))
        for i, (path, label, choices) in enumerate(LOOKS, start=1):
            ttk.Label(tab, text=label).grid(row=i, column=0, sticky="w", pady=2)
            var = tk.StringVar()
            box = ttk.Combobox(tab, textvariable=var, values=[t for _, t in choices], state="readonly", width=26)
            box.grid(row=i, column=1, sticky="w", padx=(10, 0), pady=2)
            self.look_vars[path] = (var, choices)
        row = ttk.Frame(tab)
        row.grid(columnspan=2, sticky="w", pady=(10, 0))
        ttk.Button(row, text="Save to game", command=self._save_looks).grid(row=0, column=0)
        ttk.Button(row, text="Restore my original", command=self._restore_looks).grid(row=0, column=1, padx=(6, 0))
        self._load_looks()
        return tab

    # -------------------------------------------------------------- game + install
    def _pick_initial_game(self):
        saved = self.state_data.get("game")
        if saved and (pathlib.Path(saved) / "bin" / "win10" / "loneecho2.exe").is_file():
            self._set_game(pathlib.Path(saved))
            return
        found = find_game_candidates()
        if found:
            self._set_game(found[0])
        else:
            self._refresh_install()

    def _browse(self):
        folder = filedialog.askdirectory(title="Lone Echo II folder (ready-at-dawn-lone-echo-2)")
        if not folder:
            return
        path = pathlib.Path(folder)
        for candidate in (path, path.parent, path.parent.parent):
            if (candidate / "bin" / "win10" / "loneecho2.exe").is_file():
                self._set_game(candidate)
                return
        messagebox.showerror(APP, "That folder doesn't contain bin\\win10\\loneecho2.exe.")

    def _set_game(self, path):
        self.game = path
        self.state_data["game"] = str(path)
        save_state(self.state_data)
        self.game_label.config(text=f"Game: {path}")
        self._read_ini()
        self._refresh_install()

    @property
    def bin_dir(self):
        return self.game / "bin" / "win10" if self.game else None

    def _installed(self):
        return self.bin_dir is not None and (self.bin_dir / "winmm.dll").is_file()

    def _refresh_install(self):
        if self.game is None:
            self.install_label.config(text="Pick the game folder to install.")
            self.install_button.state(["disabled"])
            return
        self.install_button.state(["!disabled"])
        if self._installed():
            self.install_label.config(text="Loader installed.")
            self.install_button.config(text="Uninstall")
        else:
            self.install_label.config(text="Loader not installed: mods are off.")
            self.install_button.config(text="Install")

    def _toggle_install(self):
        target = self.bin_dir / "winmm.dll"
        try:
            if self._installed():
                target.unlink()
                self._say("Loader removed. The game runs unmodded.")
            else:
                if not LOADER.is_file():
                    messagebox.showerror(APP, f"winmm.dll is missing next to the Mod Engine:\n{LOADER}")
                    return
                shutil.copy2(LOADER, target)
                self._write_ini()
                self._say("Loader installed. Start the game as usual.")
        except PermissionError:
            messagebox.showerror(APP, "Windows refused the change. Close the game, or run the Mod Engine as "
                                      "administrator if the game is under Program Files.")
        self._refresh_install()

    # -------------------------------------------------------------- le2mods.ini
    def _ini_path(self):
        return self.bin_dir / "le2mods.ini" if self.bin_dir else None

    def _read_ini(self):
        path = self._ini_path()
        cfg = configparser.ConfigParser()
        if path and path.is_file():
            cfg.read(path, encoding="utf-8")
        for fid, _ in MODS:
            self.vars[fid].set(cfg.getboolean("mods", fid, fallback=False))
        for key, section, _, _ in PERF:
            self.vars[key].set(cfg.getboolean(section, key, fallback=True))

    def _write_ini(self):
        path = self._ini_path()
        if path is None:
            self._say("Pick the game folder first.")
            return
        cfg = configparser.ConfigParser()
        cfg["mods"] = {fid: "1" if self.vars[fid].get() else "0" for fid, _ in MODS}
        cfg["mods"]["perf_default_caps"] = "1" if self.vars["perf_default_caps"].get() else "0"
        cfg["perf"] = {"no_power_throttling": "1" if self.vars["no_power_throttling"].get() else "0"}
        tmp = path.with_suffix(".tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            f.write("; Written by the Lone Echo II Mod Engine. 1 = on.\n")
            cfg.write(f)
        os.replace(tmp, path)
        self._say("Saved. God mode applies now; the rest on the next launch."
                  if self._installed() else "Saved. Install the loader for it to take effect.")

    # -------------------------------------------------------------- looks
    def _load_looks(self):
        try:
            data = read_settings()
        except (OSError, ValueError):
            self._say("No settings.json yet: start the game once first.")
            return
        for path, (var, choices) in self.look_vars.items():
            current = get_path(data, path)
            text = next((t for v, t in choices if same_value(v, current)), None)
            var.set(text if text is not None else f"{current} (current)")

    def _save_looks(self):
        try:
            data = read_settings()
        except (OSError, ValueError) as e:
            messagebox.showerror(APP, f"Can't read {SETTINGS}:\n{e}")
            return
        if not SETTINGS_BACKUP.exists():
            shutil.copy2(SETTINGS, SETTINGS_BACKUP)
        for path, (var, choices) in self.look_vars.items():
            value = next((v for v, t in choices if t == var.get()), None)
            if value is not None:
                set_path(data, path, value)
        SETTINGS.write_text(json.dumps(data, indent=4) + "\n", encoding="utf-8")
        self._say("Graphics settings saved. They apply the next time the game starts.")

    def _restore_looks(self):
        if not SETTINGS_BACKUP.exists():
            self._say("No backup yet: nothing has been changed.")
            return
        shutil.copy2(SETTINGS_BACKUP, SETTINGS)
        self._load_looks()
        self._say("Your original settings.json is back.")

    # -------------------------------------------------------------- NVIDIA
    def _nvidia(self, action):
        script = HERE / "nvidia_profile.py"
        exe = sys.executable.replace("pythonw.exe", "python.exe")
        import ctypes
        rc = ctypes.windll.shell32.ShellExecuteW(None, "runas", exe, f'-I "{script}" {action} --pause', str(HERE), 1)
        self._say("NVIDIA profile: follow the console window." if rc > 32 else "NVIDIA profile: cancelled.")

    # -------------------------------------------------------------- status
    def _say(self, text):
        self.status.config(text=text)

    def _poll_log(self):
        log = self.bin_dir / "le2mods.log" if self.bin_dir else None
        if log and log.is_file():
            try:
                lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
                if lines and not self.status.cget("text").startswith(("Saved", "Loader", "Graphics", "NVIDIA")):
                    self._say("Game log: " + lines[-1][15:][:90])
            except OSError:
                pass
        self.after(1500, self._poll_log)


if __name__ == "__main__":
    ModEngine().mainloop()
