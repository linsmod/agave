/*
** ScanCodeMap.cpp: ScanCodeMap implements.
**
** Copyright(C) 2009 Zhang Ji Peng (onecoolx@gmail.com)
**
*/


#ifndef _SCANCODE_MAP_H_
#define _SCANCODE_MAP_H_

namespace WebCore {

static const unsigned CtrlKey = 1 << 0;
static const unsigned AltKey = 1 << 1;
static const unsigned ShiftKey = 1 << 2;


struct KeyEntry {
    unsigned virtualKey;
    unsigned modifiers;
    const char* name;
};

static const KeyEntry keyEntries[] = {
    { KEY_LEFT,   0,                  "MoveLeft"                                    },
    { KEY_LEFT,   ShiftKey,           "MoveLeftAndModifySelection"                  },
    { KEY_LEFT,   CtrlKey,            "MoveWordLeft"                                },
    { KEY_LEFT,   CtrlKey | ShiftKey, "MoveWordLeftAndModifySelection"              },
    { KEY_RIGHT,  0,                  "MoveRight"                                   },
    { KEY_RIGHT,  ShiftKey,           "MoveRightAndModifySelection"                 },
    { KEY_RIGHT,  CtrlKey,            "MoveWordRight"                               },
    { KEY_RIGHT,  CtrlKey | ShiftKey, "MoveWordRightAndModifySelection"             },
    { KEY_UP,     0,                  "MoveUp"                                      },
    { KEY_UP,     ShiftKey,           "MoveUpAndModifySelection"                    },
    { KEY_PAGEUP,  ShiftKey,           "MoveUpAndModifySelection"                    },
    { KEY_DOWN,   0,                  "MoveDown"                                    },
    { KEY_DOWN,   ShiftKey,           "MoveDownAndModifySelection"                  },
    { KEY_PAGEDOWN,   ShiftKey,           "MoveDownAndModifySelection"                  },
    { KEY_PAGEUP,  0,                  "MoveUpByPageAndModifyCaret"                  },
    { KEY_PAGEDOWN,   0,                  "MoveDownByPageAndModifyCaret"                },
    { KEY_HOME,   0,                  "MoveToBeginningOfLine"                       },
    { KEY_HOME,   ShiftKey,           "MoveToBeginningOfLineAndModifySelection"     },
    { KEY_HOME,   CtrlKey,            "MoveToBeginningOfDocument"                   },
    { KEY_HOME,   CtrlKey | ShiftKey, "MoveToBeginningOfDocumentAndModifySelection" },

    { KEY_END,    0,                  "MoveToEndOfLine"                             },
    { KEY_END,    ShiftKey,           "MoveToEndOfLineAndModifySelection"           },
    { KEY_END,    CtrlKey,            "MoveToEndOfDocument"                         },
    { KEY_END,    CtrlKey | ShiftKey, "MoveToEndOfDocumentAndModifySelection"       },

    { KEY_BACK,   0,                  "BackwardDelete"                              },
    { KEY_BACK,   ShiftKey,           "BackwardDelete"                              },
    { KEY_DELETE, 0,                  "ForwardDelete"                               },
    { KEY_DELETE, ShiftKey,           "ForwardDelete"                               },
    { KEY_BACK,   CtrlKey,            "DeleteWordBackward"                          },
    { KEY_DELETE, CtrlKey,            "DeleteWordForward"                           },
    
    { 'B',       CtrlKey,            "ToggleBold"                                  },
    { 'I',       CtrlKey,            "ToggleItalic"                                },

    { KEY_ESCAPE, 0,                  "Cancel"                                      },
    { KEY_PERIOD, CtrlKey,        	  "Cancel"                                      },
    { KEY_TAB,    0,                  "InsertTab"                                   },
    { KEY_TAB,    ShiftKey,           "InsertBacktab"                               },
    /* Enter is NOT a keydown editing command: Win32 delivers its WM_CHAR, and
     * the browser order is keydown -> keypress -> default insertion. Inserting
     * here skipped the keypress (pages that search on keypress never ran) and
     * then inserted again on the character, so one Enter produced two "\n". */

    { 'C',       CtrlKey,            "Copy"                                        },
    { 'V',       CtrlKey,            "Paste"                                       },
    { 'X',       CtrlKey,            "Cut"                                         },
    { 'A',       CtrlKey,            "SelectAll"                                   },
    { 'Z',       CtrlKey,            "Undo"                                        },
    { 'Z',       CtrlKey | ShiftKey, "Redo"                                        },
};

}
#endif /* _SCANCODE_MAP_H_ */
