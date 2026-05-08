#include <Windows.h>


/****************************************************************************
 *
 *      DirectInput keyboard scan codes
 *
 ****************************************************************************/
#define DIK_ESCAPE 0x01
#define DIK_1 0x02
#define DIK_2 0x03
#define DIK_3 0x04
#define DIK_4 0x05
#define DIK_5 0x06
#define DIK_6 0x07
#define DIK_7 0x08
#define DIK_8 0x09
#define DIK_9 0x0A
#define DIK_0 0x0B
#define DIK_MINUS 0x0C /* - on main keyboard */
#define DIK_EQUALS 0x0D
#define DIK_BACK 0x0E /* backspace */
#define DIK_TAB 0x0F
#define DIK_Q 0x10
#define DIK_W 0x11
#define DIK_E 0x12
#define DIK_R 0x13
#define DIK_T 0x14
#define DIK_Y 0x15
#define DIK_U 0x16
#define DIK_I 0x17
#define DIK_O 0x18
#define DIK_P 0x19
#define DIK_LBRACKET 0x1A
#define DIK_RBRACKET 0x1B
#define DIK_RETURN 0x1C /* Enter on main keyboard */
#define DIK_LCONTROL 0x1D
#define DIK_A 0x1E
#define DIK_S 0x1F
#define DIK_D 0x20
#define DIK_F 0x21
#define DIK_G 0x22
#define DIK_H 0x23
#define DIK_J 0x24
#define DIK_K 0x25
#define DIK_L 0x26
#define DIK_SEMICOLON 0x27
#define DIK_APOSTROPHE 0x28
#define DIK_GRAVE 0x29 /* accent grave */
#define DIK_LSHIFT 0x2A
#define DIK_BACKSLASH 0x2B
#define DIK_Z 0x2C
#define DIK_X 0x2D
#define DIK_C 0x2E
#define DIK_V 0x2F
#define DIK_B 0x30
#define DIK_N 0x31
#define DIK_M 0x32
#define DIK_COMMA 0x33
#define DIK_PERIOD 0x34 /* . on main keyboard */
#define DIK_SLASH 0x35  /* / on main keyboard */
#define DIK_RSHIFT 0x36
#define DIK_MULTIPLY 0x37 /* * on numeric keypad */
#define DIK_LMENU 0x38    /* left Alt */
#define DIK_SPACE 0x39
#define DIK_CAPITAL 0x3A
#define DIK_F1 0x3B
#define DIK_F2 0x3C
#define DIK_F3 0x3D
#define DIK_F4 0x3E
#define DIK_F5 0x3F
#define DIK_F6 0x40
#define DIK_F7 0x41
#define DIK_F8 0x42
#define DIK_F9 0x43
#define DIK_F10 0x44
#define DIK_NUMLOCK 0x45
#define DIK_SCROLL 0x46 /* Scroll Lock */
#define DIK_NUMPAD7 0x47
#define DIK_NUMPAD8 0x48
#define DIK_NUMPAD9 0x49
#define DIK_SUBTRACT 0x4A /* - on numeric keypad */
#define DIK_NUMPAD4 0x4B
#define DIK_NUMPAD5 0x4C
#define DIK_NUMPAD6 0x4D
#define DIK_ADD 0x4E /* + on numeric keypad */
#define DIK_NUMPAD1 0x4F
#define DIK_NUMPAD2 0x50
#define DIK_NUMPAD3 0x51
#define DIK_NUMPAD0 0x52
#define DIK_DECIMAL 0x53 /* . on numeric keypad */
#define DIK_OEM_102 0x56 /* <> or \| on RT 102-key keyboard (Non-U.S.) */
#define DIK_F11 0x57
#define DIK_F12 0x58
#define DIK_F13 0x64          /*                     (NEC PC98) */
#define DIK_F14 0x65          /*                     (NEC PC98) */
#define DIK_F15 0x66          /*                     (NEC PC98) */
#define DIK_KANA 0x70         /* (Japanese keyboard)            */
#define DIK_ABNT_C1 0x73      /* /? on Brazilian keyboard */
#define DIK_CONVERT 0x79      /* (Japanese keyboard)            */
#define DIK_NOCONVERT 0x7B    /* (Japanese keyboard)            */
#define DIK_YEN 0x7D          /* (Japanese keyboard)            */
#define DIK_ABNT_C2 0x7E      /* Numpad . on Brazilian keyboard */
#define DIK_NUMPADEQUALS 0x8D /* = on numeric keypad (NEC PC98) */
#define DIK_PREVTRACK 0x90    /* Previous Track (DIK_CIRCUMFLEX on Japanese keyboard) */
#define DIK_AT 0x91           /*                     (NEC PC98) */
#define DIK_COLON 0x92        /*                     (NEC PC98) */
#define DIK_UNDERLINE 0x93    /*                     (NEC PC98) */
#define DIK_KANJI 0x94        /* (Japanese keyboard)            */
#define DIK_STOP 0x95         /*                     (NEC PC98) */
#define DIK_AX 0x96           /*                     (Japan AX) */
#define DIK_UNLABELED 0x97    /*                        (J3100) */
#define DIK_NEXTTRACK 0x99    /* Next Track */
#define DIK_NUMPADENTER 0x9C  /* Enter on numeric keypad */
#define DIK_RCONTROL 0x9D
#define DIK_MUTE 0xA0        /* Mute */
#define DIK_CALCULATOR 0xA1  /* Calculator */
#define DIK_PLAYPAUSE 0xA2   /* Play / Pause */
#define DIK_MEDIASTOP 0xA4   /* Media Stop */
#define DIK_VOLUMEDOWN 0xAE  /* Volume - */
#define DIK_VOLUMEUP 0xB0    /* Volume + */
#define DIK_WEBHOME 0xB2     /* Web home */
#define DIK_NUMPADCOMMA 0xB3 /* , on numeric keypad (NEC PC98) */
#define DIK_DIVIDE 0xB5      /* / on numeric keypad */
#define DIK_SYSRQ 0xB7
#define DIK_RMENU 0xB8        /* right Alt */
#define DIK_PAUSE 0xC5        /* Pause */
#define DIK_HOME 0xC7         /* Home on arrow keypad */
#define DIK_UP 0xC8           /* UpArrow on arrow keypad */
#define DIK_PRIOR 0xC9        /* PgUp on arrow keypad */
#define DIK_LEFT 0xCB         /* LeftArrow on arrow keypad */
#define DIK_RIGHT 0xCD        /* RightArrow on arrow keypad */
#define DIK_END 0xCF          /* End on arrow keypad */
#define DIK_DOWN 0xD0         /* DownArrow on arrow keypad */
#define DIK_NEXT 0xD1         /* PgDn on arrow keypad */
#define DIK_INSERT 0xD2       /* Insert on arrow keypad */
#define DIK_DELETE 0xD3       /* Delete on arrow keypad */
#define DIK_LWIN 0xDB         /* Left Windows key */
#define DIK_RWIN 0xDC         /* Right Windows key */
#define DIK_APPS 0xDD         /* AppMenu key */
#define DIK_POWER 0xDE        /* System Power */
#define DIK_SLEEP 0xDF        /* System Sleep */
#define DIK_WAKE 0xE3         /* System Wake */
#define DIK_WEBSEARCH 0xE5    /* Web Search */
#define DIK_WEBFAVORITES 0xE6 /* Web Favorites */
#define DIK_WEBREFRESH 0xE7   /* Web Refresh */
#define DIK_WEBSTOP 0xE8      /* Web Stop */
#define DIK_WEBFORWARD 0xE9   /* Web Forward */
#define DIK_WEBBACK 0xEA      /* Web Back */
#define DIK_MYCOMPUTER 0xEB   /* My Computer */
#define DIK_MAIL 0xEC         /* Mail */
#define DIK_MEDIASELECT 0xED  /* Media Select */


std::unordered_map<int, int> PopulateKeyMapping() {
    std::unordered_map<int, int> keyMap;

    // Map Skyrim key codes to Windows virtual key codes
    keyMap[1] = VK_ESCAPE;     // esc
    keyMap[2] = '1';           // 1
    keyMap[3] = '2';           // 2
    keyMap[4] = '3';           // 3
    keyMap[5] = '4';           // 4
    keyMap[6] = '5';           // 5
    keyMap[7] = '6';           // 6
    keyMap[8] = '7';           // 7
    keyMap[9] = '8';           // 8
    keyMap[10] = '9';          // 9
    keyMap[11] = '0';          // 0
    keyMap[12] = VK_OEM_MINUS; // -
    keyMap[13] = VK_OEM_PLUS;  // =
    keyMap[14] = VK_BACK;      // backspace
    keyMap[15] = VK_TAB;       // tab
    keyMap[16] = 'Q';          // q
    keyMap[17] = 'W';          // w
    keyMap[18] = 'E';          // e
    keyMap[19] = 'R';          // r
    keyMap[20] = 'T';          // t
    keyMap[21] = 'Y';          // y
    keyMap[22] = 'U';          // u
    keyMap[23] = 'I';          // i
    keyMap[24] = 'O';          // o
    keyMap[25] = 'P';          // p
    keyMap[26] = VK_OEM_4;     // [
    keyMap[27] = VK_OEM_6;     // ]
    keyMap[28] = VK_RETURN;    // enter
    keyMap[29] = VK_CONTROL;   // ctrl/control
    keyMap[30] = 'A';          // a
    keyMap[31] = 'S';          // s
    keyMap[32] = 'D';          // d
    keyMap[33] = 'F';          // f
    keyMap[34] = 'G';          // g
    keyMap[35] = 'H';          // h
    keyMap[36] = 'J';          // j
    keyMap[37] = 'K';          // k
    keyMap[38] = 'L';          // l
    keyMap[39] = VK_OEM_1;     // ;
    keyMap[40] = VK_OEM_7;     // '
    keyMap[41] = VK_OEM_3;     // `
    keyMap[42] = VK_SHIFT;     // shift
    keyMap[43] = VK_OEM_5;     // 
    keyMap[44] = 'Z';          // z
    keyMap[45] = 'X';          // x
    keyMap[46] = 'C';          // c
    keyMap[47] = 'V';          // v
    keyMap[48] = 'B';          // b
    keyMap[49] = 'N';          // n
    keyMap[50] = 'M';          // m
    keyMap[51] = VK_OEM_COMMA; // ,
    keyMap[52] = VK_OEM_PERIOD;// .
    keyMap[53] = VK_OEM_2;     // /
    keyMap[54] = VK_RSHIFT;    // rshift/rightshift
    keyMap[55] = VK_MULTIPLY;  // num*
    keyMap[56] = VK_MENU;      // alt
    keyMap[57] = VK_SPACE;     // space
    keyMap[58] = VK_CAPITAL;   // capslock
    keyMap[59] = VK_F1;        // f1
    keyMap[60] = VK_F2;        // f2
    keyMap[61] = VK_F3;        // f3
    keyMap[62] = VK_F4;        // f4
    keyMap[63] = VK_F5;        // f5
    keyMap[64] = VK_F6;        // f6
    keyMap[65] = VK_F7;        // f7
    keyMap[66] = VK_F8;        // f8
    keyMap[67] = VK_F9;        // f9
    keyMap[68] = VK_F10;       // f10
    keyMap[69] = VK_NUMLOCK;   // numlock
    keyMap[70] = VK_SCROLL;    // scrolllock
    keyMap[71] = VK_NUMPAD7;   // num7
    keyMap[72] = VK_NUMPAD8;   // num8
    keyMap[73] = VK_NUMPAD9;   // num9
    keyMap[74] = VK_SUBTRACT;  // num-
    keyMap[75] = VK_NUMPAD4;   // num4
    keyMap[76] = VK_NUMPAD5;   // num5
    keyMap[77] = VK_NUMPAD6;   // num6
    keyMap[78] = VK_ADD;       // num+/numplus
    keyMap[79] = VK_NUMPAD1;   // num1
    keyMap[80] = VK_NUMPAD2;   // num2
    keyMap[81] = VK_NUMPAD3;   // num3
    keyMap[82] = VK_NUMPAD0;   // num0
    keyMap[83] = VK_DECIMAL;   // numdel
    keyMap[84] = VK_SNAPSHOT;  // sysreq
    keyMap[87] = VK_F11;       // f11
    keyMap[88] = VK_F12;       // f12
    keyMap[124] = VK_F13;      // f13
    keyMap[125] = VK_F14;      // f14
    keyMap[126] = VK_F15;      // f15
    keyMap[127] = VK_F16;      // f16
    keyMap[128] = VK_F17;      // f17
    keyMap[129] = VK_F18;      // f18
    keyMap[130] = VK_F19;      // f19
    keyMap[131] = VK_F20;      // f20
    keyMap[132] = VK_F21;      // f21
    keyMap[133] = VK_F22;      // f22
    keyMap[134] = VK_F23;      // f23
    keyMap[135] = VK_F24;      // f24
    keyMap[156] = VK_RETURN;   // numenter
    keyMap[157] = VK_RCONTROL; // rctrl/rightctrl/rightcontrol/rcontrol
    keyMap[181] = VK_DIVIDE;   // num/
    keyMap[183] = VK_SNAPSHOT; // printscreen/printscrn
    keyMap[184] = VK_RMENU;    // ralt/rightalt
    keyMap[199] = VK_HOME;     // home
    keyMap[200] = VK_UP;       // up
    keyMap[201] = VK_PRIOR;    // pageup
    keyMap[203] = VK_LEFT;     // left
    keyMap[205] = VK_RIGHT;    // right
    keyMap[207] = VK_END;      // end
    keyMap[208] = VK_DOWN;     // down
    keyMap[209] = VK_NEXT;     // pagedown
    keyMap[210] = VK_INSERT;   // insert/ins
    keyMap[211] = VK_DELETE;   // del/delete

    // Mouse buttons and gamepad controls
    keyMap[256] = VK_LBUTTON;  // leftmousebutton/lmb
    keyMap[257] = VK_RBUTTON;  // rightmousebutton/rmb
    keyMap[258] = VK_MBUTTON;  // middlemousebutton/mmb
    keyMap[259] = VK_XBUTTON1; // mouse3
    keyMap[260] = VK_XBUTTON2; // mouse4
    keyMap[264] = VK_PRIOR;    // mousewheelup/scrollwheelup
    keyMap[265] = VK_NEXT;     // mousewheeldown/scrollwheeldown

    // Note: Gamepad buttons don't have direct Windows VK equivalents
    // We'll keep the original codes for these
    keyMap[266] = 266;  // dpadup
    keyMap[267] = 267;  // dpaddown
    keyMap[268] = 268;  // dpadleft
    keyMap[269] = 269;  // dpadright
    keyMap[270] = 270;  // start
    keyMap[271] = 271;  // back
    keyMap[272] = 272;  // lthumb
    keyMap[273] = 273;  // rthumb
    keyMap[274] = 274;  // lshoulder/lbumper
    keyMap[275] = 275;  // rshoulder/rbumper
    keyMap[276] = 276;  // gamepada
    keyMap[277] = 277;  // gamepadb
    keyMap[278] = 278;  // gamepadx
    keyMap[279] = 279;  // gamepady
    keyMap[280] = 280;  // ltrigger
    keyMap[281] = 281;  // rtrigger

    return keyMap;
}
