import os
import re

import test_engine

#------ albBegin ----------------------------------
def _parse_command_enum():
    """Ordered Event::command_t names, read straight from ../event.hpp.

    The number handed to request_cmd() is the enum ordinal, and the
    `commands` tuple below encodes that positionally -- but it only covers
    the first 24 values.  Anything past those (CANCEL_RECORD_MUTE, the solo
    and exclusive variants, ...) cannot get its number from a tuple index,
    so it is read from the header instead.  Parsing keeps this correct by
    itself if the enum ever changes.
    """
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "event.hpp")
    with open(path) as f:
        src = f.read()
    body = re.search(r"enum\s+command_t\s*\{(.*?)\}", src, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)            # drop comments
    names = []
    for entry in body.split(","):
        name = entry.split("=")[0].strip()
        if name and name not in ("UNKNOWN", "LAST_COMMAND"):
            names.append(name)
    return names

# name -> ordinal, for every command in event.hpp
command_ordinals = dict((n, i) for i, n in enumerate(_parse_command_enum()))
#------ albEnd ----------------------------------

class TwoWayDict(dict):
    def __len__(self):
        return dict.__len__(self) // 2          # was: / 2

    def __setitem__(self, key, value):
        dict.__setitem__(self, key, value)
        dict.__setitem__(self, value, key)

commands = (
        "UNDO"
        ,"REDO"
        ,"REPLACE"
        ,"REVERSE"
        ,"SCRATCH"
        ,"RECORD"
        ,"OVERDUB"
        ,"MULTIPLY"
        ,"INSERT"
        ,"MUTE"
        ,"DELAY"
        ,"REDO_TOG"
        ,"QUANT_TOG"
        ,"ROUND_TOG"
        ,"ONESHOT"
        ,"TRIGGER"
        ,"SUBSTITUTE"
        ,"UNDO_ALL"
        ,"REDO_ALL"
        ,"MUTE_ON"
        ,"MUTE_OFF"
        ,"PAUSE"
        ,"PAUSE_ON"
        ,"PAUSE_OFF"
        )

states = TwoWayDict()
for member in test_engine.__dict__.items():  # was: iteritems()
    if "LooperState" in member[0]:
        states[member[1]] = member[0][11:]


auto_states = [  states["Unknown"]
        , states["WaitStart"]
        , states["WaitStop"]
        , states["TriggerPlay"]
        , states["OneShot"]
        , states["Undo"]
        , states["UndoAll"]
        , states["Redo"]
        , states["RedoAll"]
        ]

typical =   [
        (commands.index("REPLACE"   ), states["Replacing"])
        ,(commands.index("REVERSE"   ), states["Playing"])
        ,(commands.index("SCRATCH"   ), states["Scratching"])
        ,(commands.index("RECORD"    ), states["Recording"])
        ,(commands.index("OVERDUB"   ), states["Overdubbing"])
        ,(commands.index("MULTIPLY"  ), states["Multiplying"])
        ,(commands.index("INSERT"    ), states["Inserting"])
        ,(commands.index("MUTE"      ), states["Muted"])
        ,(commands.index("DELAY"     ), states["Delay"])
        ,(commands.index("ONESHOT"   ), states["Playing"])#XXX or Off
        ,(commands.index("TRIGGER"   ), states["Playing"])
        ,(commands.index("SUBSTITUTE"), states["Substitute"])
        ,(commands.index("UNDO_ALL"  ), states["Off"])
        ,(commands.index("REDO_ALL"  ), states["Playing"])
        ,(commands.index("MUTE_ON"   ), states["Muted"])
        ,(commands.index("MUTE_OFF"  ), states["Playing"])
        ,(commands.index("PAUSE"     ), states["Paused"])
        ,(commands.index("PAUSE_ON"  ), states["Paused"])
        ,(commands.index("PAUSE_OFF" ), states["Playing"])
        ]

class Engine (test_engine.TestEngine):

    def request(self, command):
          #------ albBegin ----------------------------------
          # Positional lookup for the original 24 (unchanged behaviour for
          # every existing test); header ordinal for anything beyond them.
          if command in commands:
              command_no = commands.index(command)
          else:
              command_no = command_ordinals[command]
          #------ albEnd ----------------------------------
          self.looper.request_cmd(command_no)
          while (self.looper.request_pending):
              pass

    def getState(self):
        return int(self.looper.get_control_value(test_engine.State))
