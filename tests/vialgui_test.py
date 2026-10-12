#!/usr/bin/env python3
"""vialgui_test.py CORE_LIB.so VIAL_GUI_SRC: vial-gui's own protocol code (protocol/keyboard_comm.py, the class the
Vial app uses for every keyboard) against firmware/kbcore.c, compiled for the PC (tests/core_lib.c).

vial-gui's util.py, unlocker.py and macro/macro_action_ui.py need Qt and hidapi: they are replaced by small
stand-ins (MSG_LEN, hid_send's padding, chunks; an Unlocker that holds OCT- + OCT+ on the simulated panel while it
polls, as a person would; macro_action_ui's tag_to_action table, from the Qt-free macro_action.py). Needs
simpleeval (vial-gui's keycodes): .venv.
Checked: the definition decompresses and parses (KLE), the matrix and encoders match the firmware's control map,
the custom keycodes match FK_NAMES, the keymap and encoders load as the firmware's defaults, set_key /
set_encoder / set_macro reach the firmware, the unlock works. Run by `make test`."""
import ctypes
import sys
import types

lib = ctypes.CDLL(sys.argv[1])
lib.lib_fk_name.restype = ctypes.c_char_p
lib.lib_macros.restype = ctypes.POINTER(ctypes.c_uint8)
sys.path.insert(0, sys.argv[2])
DEVICE = sys.argv[3] if len(sys.argv) > 3 else None    # a /dev/hidrawN: the real FM-1 instead of the library
fails = 0


def check(ok, what):
    global fails
    if not ok:
        fails += 1
        print("FAIL", what)


def send(dev, msg, retries=1):                  # util.hid_send: pad to 32, one reply of 32
    msg = msg + b"\x00" * (32 - len(msg))
    if DEVICE:
        import os
        import select
        os.write(dev, b"\x00" + msg)
        if not select.select([dev], [], [], 1.0)[0]:
            raise RuntimeError("no reply from the FM-1")
        return os.read(dev, 64)
    buf = (ctypes.c_uint8 * 32)(*msg)
    lib.lib_cmd(buf)
    return bytes(buf)


class Unlocker:                                 # unlocker.py: the user holds the unlock keys until it is done
    @staticmethod
    def unlock(kb):
        if kb.get_unlock_status() == 1:
            return
        kb.unlock_start()
        for _ in range(100):
            lib.lib_panel(3, 0)                 # OCT- + OCT+
            lib.lib_advance(150)
            if kb.unlock_poll()[0]:
                break
        lib.lib_panel(0, 0)


util = types.ModuleType("util")
util.MSG_LEN = 32
util.hid_send = send
util.chunks = lambda data, sz: (data[i:i + sz] for i in range(0, len(data), sz))
sys.modules["util"] = util
unlocker = types.ModuleType("unlocker")
unlocker.Unlocker = Unlocker
sys.modules["unlocker"] = unlocker
from macro import macro_action  # noqa: E402   (Qt-free; macro_action_ui is its Qt editor, needed for one table)
action_ui = types.ModuleType("macro.macro_action_ui")
action_ui.tag_to_action = {"down": macro_action.ActionDown, "up": macro_action.ActionUp, "tap": macro_action.ActionTap,
                           "text": macro_action.ActionText, "delay": macro_action.ActionDelay}
sys.modules["macro.macro_action_ui"] = action_ui


class QmkSettings:                              # editor/qmk_settings.py's classmethods (its Qt editor left out),
    qsid_fields = {}                            # on vial-gui's own resources/base/qmk_settings.json

    @classmethod
    def load(cls, path):
        import json
        for tab in json.load(open(path))["tabs"]:
            for field in tab["fields"]:
                cls.qsid_fields.setdefault(field["qsid"], []).append(field)

    @classmethod
    def is_qsid_supported(cls, qsid):
        return qsid in cls.qsid_fields

    @classmethod
    def qsid_serialize(cls, qsid, data):
        f = cls.qsid_fields[qsid][0]
        return data.to_bytes(f.get("width", 1), byteorder="little")

    @classmethod
    def qsid_deserialize(cls, qsid, data):
        f = cls.qsid_fields[qsid][0]
        return int.from_bytes(data[0:f.get("width", 1)], byteorder="little")


QmkSettings.load(sys.argv[2] + "/../resources/base/qmk_settings.json")
qs_mod = types.ModuleType("editor.qmk_settings")
qs_mod.QmkSettings = QmkSettings
sys.modules["editor.qmk_settings"] = qs_mod

from keycodes.keycodes import Keycode  # noqa: E402
from protocol.keyboard_comm import Keyboard  # noqa: E402

lib.lib_init()
if DEVICE:
    import os
    kb = Keyboard(os.open(DEVICE, os.O_RDWR), usb_send=send)
else:
    kb = Keyboard(None, usb_send=send)
kb.reload()

check(kb.via_protocol == 9 and kb.vial_protocol == 6, f"protocols {kb.via_protocol} / {kb.vial_protocol}")
check((kb.rows, kb.cols, kb.layers) == (3, 16, 5), f"matrix {kb.rows} x {kb.cols}, {kb.layers} layers")

# the KLE layout's matrix positions = the firmware's controls, one each
fw = {(lib.lib_pos_rowcol(p) >> 8, lib.lib_pos_rowcol(p) & 0xFF) for p in range(lib.lib_npos())}
kle = {(k.row, k.col) for k in kb.keys}
check(len(kb.keys) == lib.lib_npos() and kle == fw, f"layout keys {len(kb.keys)}: missing {fw - kle}, extra {kle - fw}")
enc = sorted((k.encoder_idx, k.encoder_dir) for k in kb.encoders)
check(enc == [(i, d) for i in range(4) for d in (0, 1)], f"encoders (KNOB1..4 only) {enc}")

names = [c["shortName"] for c in kb.custom_keycodes]
fk = []
i = 0
while lib.lib_fk_name(i) is not None:
    fk.append(lib.lib_fk_name(i).decode())
    i += 1
check(names == fk, f"customKeycodes {names} != FK_NAMES {fk}")

# the keymap and encoders as Vial loaded them = the firmware's defaults
bad = 0
for p in range(lib.lib_npos()):
    r, c = lib.lib_pos_rowcol(p) >> 8, lib.lib_pos_rowcol(p) & 0xFF
    for layer in range(5):
        if kb.layout[(layer, r, c)] != Keycode.serialize(lib.lib_default_key(layer, p)):
            bad += 1
            if bad < 4:
                print("  layout", layer, r, c, kb.layout[(layer, r, c)], hex(lib.lib_default_key(layer, p)))
if DEVICE:                                      # a device keeps its saved configuration: not a failure
    print(f"  (device: {bad} keys differ from the defaults: its saved configuration)")
else:
    check(bad == 0, f"keymap: {bad} keys differ from the defaults")
bad = sum(kb.encoder_layout[(layer, e, d)] != Keycode.serialize(lib.lib_default_enc(layer, e, d))
          for layer in range(5) for e in range(4) for d in (0, 1))
if DEVICE:
    print(f"  (device: {bad} encoder actions differ from the defaults)")
else:
    check(bad == 0, f"encoders: {bad} differ from the defaults")
if DEVICE:
    print(f"  device settings {kb.settings}, {kb.tap_dance_count} tap dances, {kb.combo_count} combos")
print("loaded:", kb.layout[(0, 2, 0)], kb.layout[(0, 0, 0)], kb.layout[(0, 0, 2)], kb.encoder_layout[(0, 2, 1)],
      "| custom:", names)

if DEVICE:                                      # the real FM-1: loading only, nothing is changed on it
    print(f"vialgui_test (device {DEVICE}): {'FAILED' if fails else 'ok'} ({fails} failures)")
    sys.exit(1 if fails else 0)

# edits from the app reach the firmware
kb.set_key(0, 2, 0, "LCTL(KC_C)")
check(lib.lib_km_get(0, 2, 0) == 0x0106, f"set_key LCTL(KC_C): {lib.lib_km_get(0, 2, 0):#06x}")
kb.set_key(1, 0, 2, "MO(4)")
check(lib.lib_km_get(1, 0, 2) == 0x5224, f"set_key MO(4): {lib.lib_km_get(1, 0, 2):#06x}")
kb.set_encoder(0, 3, 1, "KC_VOLU")
check(lib.lib_enc_get(0, 3, 1) == 0x00A9, f"set_encoder KC_VOLU: {lib.lib_enc_get(0, 3, 1):#06x}")
kb.set_key(0, 0, 3, "USER02")
check(lib.lib_km_get(0, 0, 3) == 0x7E02, f"set_key USER02 (KB OFF): {lib.lib_km_get(0, 0, 3):#06x}")

# QMK settings, tap dances, combos (phase 4)
check(kb.settings == {2: 50, 6: 0, 7: 200, 18: 0, 20: 5, 22: 0, 23: 0, 24: 0, 25: 200}, f"settings {kb.settings}")
kb.qmk_settings_set(7, 180)
kb.qmk_settings_set(22, 1)
check(lib.lib_qs(7) == 180 and lib.lib_qs(22) == 1, f"settings set: term {lib.lib_qs(7)}, permissive {lib.lib_qs(22)}")
check(kb.tap_dance_count == 16 and kb.combo_count == 16, f"dynamic: {kb.tap_dance_count} TD, {kb.combo_count} combos")
kb.tap_dance_set(2, ("KC_A", "KC_LCTL", "KC_B", "KC_NO", 180))
check([lib.lib_td(2, f) for f in range(5)] == [0x04, 0xE0, 0x05, 0, 180],
      f"tap_dance_set: {[hex(lib.lib_td(2, f)) for f in range(5)]}")
kb.combo_set(1, ("KC_A", "KC_C", "KC_NO", "KC_NO", "KC_ESC"))
check([lib.lib_combo(1, f) for f in range(5)] == [0x04, 0x06, 0, 0, 0x29],
      f"combo_set: {[hex(lib.lib_combo(1, f)) for f in range(5)]}")

# macros: the GUI's serialisation, written after its unlock
check(kb.macro_count == 16 and kb.macro_memory == 2048, f"macros {kb.macro_count} / {kb.macro_memory}")
Unlocker.unlock(kb)
check(lib.lib_unlocked() == 1, "unlock through the GUI's unlock_start / unlock_poll")
data = b"hello\x00\x01\x02\x04world\x00" + b"\x00" * 14
kb.set_macro(data)
got = bytes(lib.lib_macros()[i] for i in range(len(data)))
check(got == data, f"set_macro: {got!r}")

# a second session sees the edits
kb2 = Keyboard(None, usb_send=send)
kb2.reload()
check(kb2.layout[(0, 2, 0)] == "LCTL(KC_C)" and kb2.encoder_layout[(0, 3, 1)] == "KC_VOLU",
      f"reload: {kb2.layout[(0, 2, 0)]} {kb2.encoder_layout[(0, 3, 1)]}")
check(kb2.macro.startswith(b"hello\x00"), f"reload macros {kb2.macro[:16]!r}")
check(kb2.settings[7] == 180 and kb2.settings[22] == 1, f"reload settings {kb2.settings}")
check(kb2.tap_dance_entries[2] == ("KC_A", "KC_LCTRL", "KC_B", "KC_NO", 180), f"reload TD {kb2.tap_dance_entries[2]}")
check(kb2.combo_entries[1] == ("KC_A", "KC_C", "KC_NO", "KC_NO", "KC_ESCAPE"), f"reload combo {kb2.combo_entries[1]}")

print(f"vialgui_test: {'FAILED' if fails else 'ok'} ({fails} failures)")
sys.exit(1 if fails else 0)
