# Patch file format

Every switch is one file in `patches/`, named after the switch id used in `le2mods.ini` (`patches/god_mode.json` ↔
`god_mode=1`). `tools/gen_patches.py` compiles them into the loader; run `build.ps1` after any change.

Target: `loneecho2.exe` PE timestamp `0x61578052`, image base `0x140000000`. Script DLLs are
`bin\win10\scripts\<16-hex CSymbol64 hash>.dll`; names for most of them are in
[echo-symbols](https://github.com/Dualgame/echo-symbols) (`data/names.tsv`).

```json
{
  "feature": "web_receivers_disabled",
  "live": true,
  "patches": [
    {
      "module": "exe",
      "rva": "0xa7d979",
      "expect": "72 15",
      "replace": "eb 15",
      "why": "one line: what this instruction is and what the change does"
    }
  ],
  "evidence": "how it was found (strings, xrefs, call chain, struct offsets) and what is not verified",
  "risk": "what else shares the code, what ends up in saves"
}
```

| Field | Meaning |
|---|---|
| `live` | `true` only if the patches can be applied and reverted while the game runs. Live switches are re-read from `le2mods.ini` four times a second; the others apply at the next launch. |
| `module` | `"exe"` for `loneecho2.exe`, or a script file name such as `"1b9174546ee05f8a.dll"`. Script patches are applied the moment the engine asks that script for `setup_bindings`. |
| `rva` | Address relative to the module's image base. |
| `expect` | The exact bytes there now. The loader refuses to write if they differ, so a wrong build can never be corrupted. |
| `replace` | Same length as `expect`. |

Rules for new patches

- Patch the decision point (a conditional jump, a compare constant, a timer subtraction), not whole functions.
- Read `expect` from the real file, never by hand.
- If the code is shared by many callers, patch the caller-specific site and say so in `risk`.
- Anything that changes saved progress must say so in `risk`.
