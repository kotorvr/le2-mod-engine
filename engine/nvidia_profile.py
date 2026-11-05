"""Set or restore NVIDIA driver settings on the "Lone Echo II" profile, through NVAPI.

    python nvidia_profile.py apply     # Power management: Prefer maximum performance
    python nvidia_profile.py restore   # back to the driver's own values for this profile
    python nvidia_profile.py show      # print the profile's settings

Unlike VEGA Patch, this never replaces the driver database: it changes one setting on one profile, through the
driver's own API, and `restore` removes exactly what `apply` set. Saving needs administrator rights.
"""
import ctypes as C
import sys

PROFILE_EXE = "loneecho2.exe"
POWER_MANAGEMENT = 0x1057EB71          # PREFERRED_PSTATE_ID
PREFER_MAX_PERFORMANCE = 1
OURS = {POWER_MANAGEMENT: PREFER_MAX_PERFORMANCE}

US = C.c_wchar * 2048


class BIN(C.Structure):
    _fields_ = [("len", C.c_uint32), ("data", C.c_ubyte * 4096)]


class VAL(C.Union):
    _fields_ = [("u32", C.c_uint32), ("bin", BIN), ("ws", US)]


class SETTING(C.Structure):
    _fields_ = [("version", C.c_uint32), ("name", US), ("id", C.c_uint32), ("type", C.c_uint32),
                ("location", C.c_uint32), ("isCurPredef", C.c_uint32), ("isPredefValid", C.c_uint32),
                ("predef", VAL), ("cur", VAL)]


class PROFILE(C.Structure):
    _fields_ = [("version", C.c_uint32), ("name", US), ("gpu", C.c_uint32), ("isPredef", C.c_uint32),
                ("numApps", C.c_uint32), ("numSettings", C.c_uint32)]


class APP(C.Structure):
    _fields_ = [("version", C.c_uint32), ("isPredef", C.c_uint32), ("appName", US), ("friendly", US),
                ("launcher", US)]


def ver(struct, v=1):
    return C.sizeof(struct) | (v << 16)


class Nvapi:
    def __init__(self):
        try:
            nv = C.WinDLL("nvapi64.dll")
        except OSError:
            raise SystemExit("No NVIDIA driver found (nvapi64.dll). Nothing to do.")
        self._qi = nv.nvapi_QueryInterface
        self._qi.restype = C.c_void_p
        self._qi.argtypes = [C.c_uint32]
        H = C.c_void_p
        self.init = self._fn(0x0150E828)
        self.create = self._fn(0x0694D52E, C.POINTER(H))
        self.destroy = self._fn(0xDAD9CFF8, H)
        self.load = self._fn(0x375DBD6B, H)
        self.save = self._fn(0xFCBC7E14, H)
        self.find_app = self._fn(0xEEE566B2, H, C.c_wchar_p, C.POINTER(H), C.POINTER(APP))
        self.get_profile = self._fn(0x61CD6FD6, H, H, C.POINTER(PROFILE))
        self.get_setting = self._fn(0x73BF8338, H, H, C.c_uint32, C.POINTER(SETTING))
        self.set_setting = self._fn(0x577DD202, H, H, C.POINTER(SETTING))
        self.delete_setting = self._fn(0xE4A26362, H, H, C.c_uint32)
        self.name_of = self._fn(0xD61CBE6E, C.c_uint32, C.POINTER(US))

    def _fn(self, iid, *args):
        p = self._qi(iid)
        if not p:
            raise SystemExit(f"This NVIDIA driver doesn't provide NVAPI function {iid:#x}.")
        return C.CFUNCTYPE(C.c_int, *args)(p)


def check(rc, what):
    if rc != 0:
        raise SystemExit(f"NVAPI {what} failed (error {rc}).")


def open_profile(api):
    check(api.init(), "initialise")
    session = C.c_void_p()
    check(api.create(C.byref(session)), "create session")
    check(api.load(session), "load settings")
    profile = C.c_void_p()
    app = APP()
    app.version = ver(APP)
    rc = api.find_app(session, PROFILE_EXE, C.byref(profile), C.byref(app))
    if rc != 0:
        raise SystemExit(f"The driver has no profile for {PROFILE_EXE} (error {rc}).")
    return session, profile


def current(api, session, profile, setting_id):
    s = SETTING()
    s.version = ver(SETTING)
    if api.get_setting(session, profile, setting_id, C.byref(s)) != 0:
        return None
    return s.cur.u32, s.location


def main(action):
    api = Nvapi()
    session, profile = open_profile(api)
    info = PROFILE()
    info.version = ver(PROFILE)
    check(api.get_profile(session, profile, C.byref(info)), "read profile")
    print(f'Profile: "{info.name}"')
    try:
        if action == "show":
            for sid in OURS:
                name = US()
                api.name_of(sid, C.byref(name))
                print(f"  {name.value}: {current(api, session, profile, sid)}")
            return
        for sid, value in OURS.items():
            name = US()
            api.name_of(sid, C.byref(name))
            if action == "apply":
                s = SETTING()
                s.version = ver(SETTING)
                s.id = sid
                s.type = 0  # DWORD
                s.cur.u32 = value
                check(api.set_setting(session, profile, C.byref(s)), f"set {name.value}")
                print(f"  {name.value} -> {value}")
            else:
                rc = api.delete_setting(session, profile, sid)
                print(f"  {name.value} -> driver default" if rc == 0 else f"  {name.value}: already default")
        check(api.save(session), "save (run as administrator)")
        print("Saved.")
    finally:
        api.destroy(session)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--pause"]
    action = args[0] if args else "show"
    if action not in ("apply", "restore", "show"):
        raise SystemExit(__doc__)
    try:
        main(action)
    finally:
        if "--pause" in sys.argv:
            input("\nPress Enter to close.")
