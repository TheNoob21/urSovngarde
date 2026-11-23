#!/usr/bin/env python3
"""Generate urSovngardeCoopMenu.esp (Skyrim SE/VR, form version 44, plain ESP, master Skyrim.esm only).

Stdlib only; deterministic (no timestamps, no random data). Run with `python -I`.

    python -I Tools/Esp/make_coop_menu_esp.py [--out PATH]

Default output: <repo root>/GameFiles/Skyrim/urSovngardeCoopMenu.esp

Every byte layout below was copied from real plugins (all form version 44): SkyUI_SE.esp, MCMHelper.esp,
AmazingFollowerTweaks.esp ("AFT") and Skyrim.esm.

File-level facts, as real plugins have them:
  * HEDR recordCount == number of records + number of GRUPs (nested too), excluding the TES4 record itself.
  * Record header (24 bytes) = sig, dataSize, flags, formId, timestamp/vcinfo u32, formVersion u16, vcinfo2 u16.
    All real plugin records have timestamp 0 (e.g. SkyUI_SE.esp) and version 44 (0x2c).
  * Group header (24 bytes) = 'GRUP', groupSize (INCLUDES the 24 byte header), label, type, stamp u16, vc u16, u32.
  * TES4 subrecord order in SkyUI_SE.esp / AFT / MCMHelper.esp: HEDR, CNAM, SNAM(optional), MAST+DATA(8 zero bytes) per
    master, INTV (u32 = 1; present in all three).  TES4 flags 0 for a plain ESP (MCMHelper.esp has 0x200 = ESL).
  * Strings are inline null-terminated cp1252 because the TES4 "Localized" flag (0x80) is NOT set.

Spell and effect types, as the real files have them: SPIT type 3 = Lesser Power (Skyrim.esm PowerKhajiitNightEye 000AA01D,
  AFT TweakCommandSpell 02025A4B) and MGEF DATA archetype 1 = Script (AFT TweakCommandFx 02025A4A).  0x0A in SPIT is
  "Addiction" and 11 is "Voice".
"""
import argparse
import os
import struct
import sys

PLUGIN_NAME = "urSovngardeCoopMenu.esp"
FORM_VERSION = 44
MY = 0x01000000  # top byte 01: plugin index after the single master Skyrim.esm (index 00)

ID_MGEF = MY | 0x800
ID_SPEL = MY | 0x801
ID_MESG = MY | 0x802
ID_QUST_SETUP = MY | 0x803
ID_RETIRED_MENU_QUEST = MY | 0x804
# The word to another player's follower (urSovngardeFollowerEffect): the power's MGEF and SPEL layouts, and a 4-button MESG.
ID_FOLLOWER_MGEF = MY | 0x805
ID_FOLLOWER_SPEL = MY | 0x806
ID_FOLLOWER_MESG = MY | 0x807
# A dragon's soul for every player of the party (urSovngardeDragonSoulEffect): the power's MGEF and SPEL layouts.
ID_DRAGON_SOUL_MGEF = MY | 0x808
ID_DRAGON_SOUL_SPEL = MY | 0x809
ID_SERVER_PICKER = MY | 0x80A
NEXT_OBJECT_ID = 0x80B


# ------------------------------------------------------------------ low level writers

def zstr(s):
    return s.encode("cp1252") + b"\x00"


def wstring(s):
    b = s.encode("cp1252")
    return struct.pack("<H", len(b)) + b  # Papyrus/VMAD wstring: u16 length, NO terminator


def sub(sig, data=b""):
    assert len(sig) == 4 and len(data) <= 0xFFFF
    return sig.encode("ascii") + struct.pack("<H", len(data)) + data


def u32(v):
    return struct.pack("<I", v)


def record(sig, formid, subs, flags=0):
    data = b"".join(subs)
    # sig, dataSize, flags, formId, timestamp+vcinfo (0), formVersion, vcinfo2 (0)
    return sig.encode("ascii") + struct.pack("<IIIIHH", len(data), flags, formid, 0, FORM_VERSION, 0) + data


def group(label, children):
    body = b"".join(children)
    # 'GRUP', size incl. 24 byte header, label (signature), type 0 = top level, stamp, vcinfo, unknown
    return b"GRUP" + struct.pack("<I", len(body) + 24) + label.encode("ascii") + struct.pack("<IHHI", 0, 0, 0, 0) + body


# ------------------------------------------------------------------ VMAD

def vmad_script_entry(name):
    """One script, status 0, zero properties.  Layout copied byte for byte from
    FISS.esp QUST 020012C4 'FISSScript' (VMAD[21]):  05 00 | 02 00 | 01 00 | wstring | 00 | 00 00
    and from AFT/Apocalypse script MGEFs (e.g. Apocalypse 0300C0A9 VMAD[len 11+namelen])."""
    return wstring(name) + b"\x00" + struct.pack("<H", 0)


def vmad_header(count):
    # version 5, object format 2, script count
    return struct.pack("<hhH", 5, 2, count)


def vmad_plain(script):
    return vmad_header(1) + vmad_script_entry(script)


# ------------------------------------------------------------------ records

def make_mgef(formid=ID_MGEF, edid="urSovngardeCoopMGEF", script="urSovngardeCoopEffect", name="Call to Sovngarde",
              description="Open the urSovngarde co-op menu."):
    """MGEF urSovngardeCoopMGEF.  Template: AFT MGEF 02025A4A 'TweakCommandFx' (script MGEF of a Self / Fire and Forget
    Lesser Power).  Subrecords in the real order: EDID, VMAD, FULL, MDOB, DATA, SNDD, DNAM.  (No KSIZ/KWDA, no CTDA,
    no ESCE/counter effects in that example.)  Record flags 0 (example flags 0).
      EDID  zstring
      VMAD  version 5 / objfmt 2 / 1 script / status 0 / 0 properties  (example has many properties; layout otherwise equal)
      FULL  inline zstring 'Call to Sovngarde'
      MDOB  u32 0x000435A5  (Skyrim.esm STAT, the vanilla 'Menu Display Object' every example uses; verified in Skyrim.esm)
      DATA  152 bytes copied verbatim from the example (formid fields inside are all 0/0xFFFFFFFF):
              flags 0x00000A02 = Recover | No Duration | No Area ; baseCost 0.0 ; assoc item 0 ;
              magic skill -1 ; resist -1 ; ... ; archetype @64 = 1 (Script) ; actor value @68 = -1 ;
              castType @80 = 1 (Fire and Forget) ; delivery @84 = 0 (Self) ; second AV @88 = -1 ; ...
      SNDD  32 bytes verbatim from the example: 4 x (u32 type, u32 SNDR formid in Skyrim.esm): 0:0007BBAB 1:0007C71F
            2:0007C720 3:0007BBAE (all four verified as SNDR records in Skyrim.esm)
      DNAM  inline zstring effect description
    """
    data = bytes.fromhex(
        "020a00000000000000000000ffffffffffffffff000000000000000000000000"
        "000000000000000000000000000000000000003f000000000000000000000000"
        "01000000ffffffff00000000000000000100000000000000ffffffff00000000"
        "000000000000000000000000000000000000803f000000000000000000000000"
        "000000000000000000000000000000000000000000000000"
    )
    assert len(data) == 152, len(data)
    assert struct.unpack_from("<I", data, 64)[0] == 1      # archetype Script
    assert struct.unpack_from("<I", data, 80)[0] == 1      # cast type Fire and Forget
    assert struct.unpack_from("<I", data, 84)[0] == 0      # delivery Self
    sndd = bytes.fromhex("00000000abbb0700010000001fc707000200000020c7070003000000aebb0700")
    return record("MGEF", formid, [
        sub("EDID", zstr(edid)),
        sub("VMAD", vmad_plain(script)),
        sub("FULL", zstr(name)),
        sub("MDOB", u32(0x000435A5)),
        sub("DATA", data),
        sub("SNDD", sndd),
        sub("DNAM", zstr(description)),
    ])


def make_spel(formid=ID_SPEL, edid="urSovngardeCoopSPEL", effect=ID_MGEF, name="Call to Sovngarde",
              description="Open the urSovngarde co-op menu."):
    """SPEL urSovngardeCoopSPEL.  Template: AFT SPEL 02025A4B 'TweakCommandSpell' (Lesser Power, Self, Fire and Forget,
    effect = a script MGEF); same subrecord set/order as vanilla powers (Skyrim.esm 000AA01D PowerKhajiitNightEye):
    EDID, OBND, FULL, MDOB, ETYP, DESC, SPIT, EFID, EFIT.
      OBND  12 zero bytes
      FULL  inline zstring
      MDOB  0x000435A5 (as in example and vanilla)
      ETYP  0x00025BEE  EQUP 'EquipTypeVoice' (verified EQUP in Skyrim.esm; same value in vanilla powers and the example)
      DESC  inline zstring
      SPIT  36 bytes, example: 00000000 01000000 03000000 00000000 01000000 00000000 00000000 00000000 00000000
            = base cost 0 | flags 0x1 (Manual cost calc / no auto-calc) | type 3 (Lesser Power) | charge time 0.0 |
              cast type 1 (Fire and Forget) | target type 0 (Self) | cast duration 0.0 | range 0.0 | perk 0
      EFID  u32 effect formid
      EFIT  12 bytes: magnitude f32 0 | area u32 0 | duration u32 0
    """
    spit = struct.pack("<IIIfIIffI", 0, 1, 3, 0.0, 1, 0, 0.0, 0.0, 0)
    assert spit.hex() == "000000000100000003000000000000000100000000000000000000000000000000000000"
    return record("SPEL", formid, [
        sub("EDID", zstr(edid)),
        sub("OBND", b"\x00" * 12),
        sub("FULL", zstr(name)),
        sub("MDOB", u32(0x000435A5)),
        sub("ETYP", u32(0x00025BEE)),
        sub("DESC", zstr(description)),
        sub("SPIT", spit),
        sub("EFID", u32(effect)),
        sub("EFIT", struct.pack("<fII", 0.0, 0, 0)),
    ])


def make_mesg():
    """MESG urSovngardeCoopMenuMSG.  Template: AFT MESG 02025A48 'TweakCommandMisc' and
    3DNPC.esp MESG 051DA7CF (non-localized, title + body + buttons).  Real order: EDID, DESC (body), FULL (title),
    INAM, DNAM, ITXT x N.
      INAM  u32 0 (icon form; 0 in both examples)
      DNAM  u32 1 (flags: 1 = Message Box; 2 would be Auto-display; both examples use 1)
      no QNAM (quest owner), no TNAM (display time) -- absent in message boxes without them (e.g. 3DNPC 051DA7CF)
      ITXT  inline zstring per button, in display order
    Record flags 0 in both examples."""
    return record("MESG", ID_MESG, [
        sub("EDID", zstr("urSovngardeCoopMenuMSG")),
        sub("DESC", zstr("Choose a server or check your connection.")),
        sub("FULL", zstr("Call to Sovngarde")),
        sub("INAM", u32(0)),
        sub("DNAM", u32(1)),
        sub("ITXT", zstr("Saved servers")),
        sub("ITXT", zstr("Disconnect")),
        sub("ITXT", zstr("Show status")),
        sub("ITXT", zstr("Close")),
        sub("ITXT", zstr("Join last used")),
    ])


def make_server_picker():
    return record("MESG", ID_SERVER_PICKER, [
        sub("EDID", zstr("urSovngardeServerPickerMSG")),
        sub("DESC", zstr("Choose the number of the server you want to join.")),
        sub("FULL", zstr("Saved servers")),
        sub("INAM", u32(0)),
        sub("DNAM", u32(1)),
        *[sub("ITXT", zstr(button)) for button in ("1", "2", "3", "4", "Previous page", "Next page", "Close")],
    ])


def make_follower_mesg():
    """MESG urSovngardeFollowerMSG: the word to another player's follower, 3 buttons (layout as make_mesg).
    The script reads the buttons by index: 0 Wait here, 1 Follow her player, 2 Trade, 3 Cancel."""
    return record("MESG", ID_FOLLOWER_MESG, [
        sub("EDID", zstr("urSovngardeFollowerMSG")),
        sub("DESC", zstr("This follower is another player's, and her player's game runs her. You can ask her to wait here or to follow her player again, or trade items with her.")),
        sub("FULL", zstr("Another player's follower")),
        sub("INAM", u32(0)),
        sub("DNAM", u32(1)),
        sub("ITXT", zstr("Wait here")),
        sub("ITXT", zstr("Follow her player")),
        sub("ITXT", zstr("Trade")),
        sub("ITXT", zstr("Cancel")),
    ])


def make_follower_effect():
    return make_mgef(ID_FOLLOWER_MGEF, "urSovngardeFollowerMGEF", "urSovngardeFollowerEffect", "Word to a follower",
                     "Ask another player's follower to wait or to follow her player.")


def make_follower_spel():
    # Never given to a player: the client casts it (Code/client/FollowerCommands.cpp).
    return make_spel(ID_FOLLOWER_SPEL, "urSovngardeFollowerSPEL", ID_FOLLOWER_MGEF, "Word to a follower",
                     "Ask another player's follower to wait or to follow her player.")


def make_dragon_soul_effect():
    return make_mgef(ID_DRAGON_SOUL_MGEF, "urSovngardeDragonSoulMGEF", "urSovngardeDragonSoulEffect", "Dragon soul",
                     "A dragon of the party died: its soul for this player.")


def make_dragon_soul_spel():
    # Never given to a player: the client casts it (Code/client/DragonSouls.cpp).
    return make_spel(ID_DRAGON_SOUL_SPEL, "urSovngardeDragonSoulSPEL", ID_DRAGON_SOUL_MGEF, "Dragon soul",
                     "A dragon of the party died: its soul for this player.")


def make_qust_setup():
    """QUST urSovngardeCoopSetupQuest.  Template: AFT QUST 0205020D 'TweakThreadWorker4' (script quest, no aliases,
    no stages): subrecords EDID, VMAD, DNAM, NEXT, ANAM; record flags 0.  (FISS.esp 020012C4 'FISSScript' has the same
    five subrecords and a 21-byte VMAD identical in layout to ours.)
      DNAM  12 bytes: u16 flags 0x0011 | u8 priority 0 | u8 0xFF | u32 0 | u32 quest type 0
            Example bytes: 11 00 00 ff 00000000 00000000.  Bit 0x01 Start Game Enabled, bit 0x10 present in every
            start-up script quest, bit 0x100 Run Once is CLEAR here (SkyUI/Valhalla use 0x0111 with Run Once;
            AFT/Conduit/QuickLootEE/SkyRem use 0x0011).  No FULL (AFT example has none).
      NEXT  empty subrecord (marker before alias section)
      ANAM  u32 next alias id = 0 (no aliases)
    """
    return record("QUST", ID_QUST_SETUP, [
        sub("EDID", zstr("urSovngardeCoopSetupQuest")),
        sub("VMAD", vmad_plain("urSovngardeCoopSetup")),
        sub("DNAM", struct.pack("<HBBII", 0x0011, 0, 0xFF, 0, 0)),
        sub("NEXT"),
        sub("ANAM", u32(0)),
    ])


def make_retired_menu_quest():
    # Existing saves may refer to form 804. Keep the form without a startup flag or scripts.
    return record("QUST", ID_RETIRED_MENU_QUEST, [
        sub("EDID", zstr("urSovngardeCoopMCMQuest")),
        sub("DNAM", struct.pack("<HBBII", 0, 0, 0xFF, 0, 0)),
        sub("NEXT"),
        sub("ANAM", u32(0)),
    ])


def build():
    groups = [
        ("MGEF", [make_mgef(), make_follower_effect(), make_dragon_soul_effect()]),
        ("SPEL", [make_spel(), make_follower_spel(), make_dragon_soul_spel()]),
        ("MESG", [make_mesg(), make_follower_mesg(), make_server_picker()]),
        ("QUST", [make_qust_setup(), make_retired_menu_quest()]),
    ]
    n_records = sum(len(r) for _, r in groups)
    n_groups = len(groups)
    # HEDR record count = records + groups, excluding TES4 (checked on 93 real plugins).
    hedr = struct.pack("<fII", 1.7, n_records + n_groups, NEXT_OBJECT_ID)
    tes4_subs = [
        sub("HEDR", hedr),
        sub("CNAM", zstr("urSovngarde")),
        sub("SNAM", zstr("urSovngarde co-op: Call to Sovngarde power, message menu, setup quest, the word to another player's follower, and the party's dragon souls.")),
        sub("MAST", zstr("Skyrim.esm")),
        sub("DATA", b"\x00" * 8),
        sub("INTV", u32(1)),
    ]
    tes4 = record("TES4", 0, tes4_subs, flags=0)
    return tes4 + b"".join(group(lbl, recs) for lbl, recs in groups)


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(root, "GameFiles", "Skyrim", PLUGIN_NAME))
    a = ap.parse_args()
    blob = build()
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(blob)
    print("wrote %s (%d bytes)" % (a.out, len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
