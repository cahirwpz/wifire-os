/*
 * Heavily based on FreeBSD's code in `sys/dev/evdev/`.
 */

#include <dev/evdev.h>

#define NONE KEY_RESERVED

/*
 * USB HID scancode list is available at:
 * https://www.win.tue.nl/~aeb/linux/kbd/scancodes-14.html
 */

/* clang-format off */
static uint16_t evdev_usb_scancodes[256] = {
	/* 0x00 - 0x27 */
	NONE,	NONE,	NONE,	NONE,	KEY_A,	KEY_B,	KEY_C,	KEY_D,
	KEY_E,	KEY_F,	KEY_G,	KEY_H,	KEY_I,	KEY_J,	KEY_K,	KEY_L,
	KEY_M,	KEY_N,	KEY_O,	KEY_P,	KEY_Q,	KEY_R,	KEY_S,	KEY_T,
	KEY_U,	KEY_V,	KEY_W,	KEY_X,	KEY_Y,	KEY_Z,	KEY_1,	KEY_2,
	KEY_3,	KEY_4,	KEY_5,	KEY_6,	KEY_7,	KEY_8,	KEY_9,	KEY_0,
	/* 0x28 - 0x3f */
	KEY_ENTER,	KEY_ESC,	KEY_BACKSPACE,	KEY_TAB,
	KEY_SPACE,	KEY_MINUS,	KEY_EQUAL,	KEY_LEFTBRACE,
	KEY_RIGHTBRACE,	KEY_BACKSLASH,	KEY_BACKSLASH,	KEY_SEMICOLON,
	KEY_APOSTROPHE,	KEY_GRAVE,	KEY_COMMA,	KEY_DOT,
	KEY_SLASH,	KEY_CAPSLOCK,	KEY_F1,		KEY_F2,
	KEY_F3,		KEY_F4,		KEY_F5,		KEY_F6,
	/* 0x40 - 0x5f */
	KEY_F7,		KEY_F8,		KEY_F9,		KEY_F10,
	KEY_F11,	KEY_F12,	KEY_SYSRQ,	KEY_SCROLLLOCK,
	KEY_PAUSE,	KEY_INSERT,	KEY_HOME,	KEY_PAGEUP,
	KEY_DELETE,	KEY_END,	KEY_PAGEDOWN,	KEY_RIGHT,
	KEY_LEFT,	KEY_DOWN,	KEY_UP,		KEY_NUMLOCK,
	KEY_KPSLASH,	KEY_KPASTERISK,	KEY_KPMINUS,	KEY_KPPLUS,
	KEY_KPENTER,	KEY_KP1,	KEY_KP2,	KEY_KP3,
	KEY_KP4,	KEY_KP5,	KEY_KP6,	KEY_KP7,
	/* 0x60 - 0x7f */
	KEY_KP8,	KEY_KP9,	KEY_KP0,	KEY_KPDOT,
	KEY_102ND,	KEY_COMPOSE,	KEY_POWER,	KEY_KPEQUAL,
	KEY_F13,	KEY_F14,	KEY_F15,	KEY_F16,
	KEY_F17,	KEY_F18,	KEY_F19,	KEY_F20,
	KEY_F21,	KEY_F22,	KEY_F23,	KEY_F24,
	KEY_OPEN,	KEY_HELP,	KEY_PROPS,	KEY_FRONT,
	KEY_STOP,	KEY_AGAIN,	KEY_UNDO,	KEY_CUT,
	KEY_COPY,	KEY_PASTE,	KEY_FIND,	KEY_MUTE,
	/* 0x80 - 0x9f */
	KEY_VOLUMEUP,	KEY_VOLUMEDOWN,	NONE,		NONE,
	NONE,		KEY_KPCOMMA,	NONE,		KEY_RO,
	KEY_KATAKANAHIRAGANA,	KEY_YEN,KEY_HENKAN,	KEY_MUHENKAN,
	KEY_KPJPCOMMA,	NONE,		NONE,		NONE,
	KEY_HANGEUL,	KEY_HANJA,	KEY_KATAKANA,	KEY_HIRAGANA,
	KEY_ZENKAKUHANKAKU,	NONE,	NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	/* 0xa0 - 0xbf */
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	/* 0xc0 - 0xdf */
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	/* 0xe0 - 0xff */
	KEY_LEFTCTRL,	KEY_LEFTSHIFT,	KEY_LEFTALT,	KEY_LEFTMETA,
	KEY_RIGHTCTRL,	KEY_RIGHTSHIFT,	KEY_RIGHTALT,	KEY_RIGHTMETA,
	KEY_PLAYPAUSE,	KEY_STOPCD,	KEY_PREVIOUSSONG,KEY_NEXTSONG,
	KEY_EJECTCD,	KEY_VOLUMEUP,	KEY_VOLUMEDOWN,	KEY_MUTE,
	KEY_WWW,	KEY_BACK,	KEY_FORWARD,	KEY_STOP,
	KEY_FIND,	KEY_SCROLLUP,	KEY_SCROLLDOWN,	KEY_EDIT,
	KEY_SLEEP,	KEY_COFFEE,	KEY_REFRESH,	KEY_CALC,
	NONE,		NONE,		NONE,		NONE,
};
/* clang-format on */

uint16_t evdev_mod2key(uint8_t modnum) {
  return evdev_usb_scancodes[modnum + 0xe0];
}

uint16_t evdev_hid2key(int scancode) {
  return evdev_usb_scancodes[scancode];
}

void evdev_hidkbd_support_all_known_keys(evdev_dev_t *evdev) {
  size_t nitems = sizeof(evdev_usb_scancodes) / sizeof(uint16_t);
  for (size_t i = KEY_RESERVED; i < nitems; i++)
    if (evdev_usb_scancodes[i] != NONE)
      evdev_support_key(evdev, evdev_usb_scancodes[i]);
}

/*
 * PC AT scancode list is available at:
 * https://www.win.tue.nl/~aeb/linux/kbd/scancodes-1.html
 */

/* clang-format off */
static uint16_t evdev_at_set1_scancodes[] = {
	/* 0x00 - 0x1f */
	NONE,		KEY_ESC,	KEY_1,		KEY_2,
	KEY_3,		KEY_4,		KEY_5,		KEY_6,
	KEY_7,		KEY_8,		KEY_9,		KEY_0,
	KEY_MINUS,	KEY_EQUAL,	KEY_BACKSPACE,	KEY_TAB,
	KEY_Q,		KEY_W,		KEY_E,		KEY_R,
	KEY_T,		KEY_Y,		KEY_U,		KEY_I,
	KEY_O,		KEY_P,		KEY_LEFTBRACE,	KEY_RIGHTBRACE,
	KEY_ENTER,	KEY_LEFTCTRL,	KEY_A,		KEY_S,
	/* 0x20 - 0x3f */
	KEY_D,		KEY_F,		KEY_G,		KEY_H,
	KEY_J,		KEY_K,		KEY_L,		KEY_SEMICOLON,
	KEY_APOSTROPHE,	KEY_GRAVE,	KEY_LEFTSHIFT,	KEY_BACKSLASH,
	KEY_Z,		KEY_X,		KEY_C,		KEY_V,
	KEY_B,		KEY_N,		KEY_M,		KEY_COMMA,
	KEY_DOT,	KEY_SLASH,	KEY_RIGHTSHIFT,	KEY_KPASTERISK,
	KEY_LEFTALT,	KEY_SPACE,	KEY_CAPSLOCK,	KEY_F1,
	KEY_F2,		KEY_F3,		KEY_F4,		KEY_F5,
	/* 0x40 - 0x5f */
	KEY_F6,		KEY_F7,		KEY_F8,		KEY_F9,
	KEY_F10,	KEY_NUMLOCK,	KEY_SCROLLLOCK,	KEY_KP7,
	KEY_KP8,	KEY_KP9,	KEY_KPMINUS,	KEY_KP4,
	KEY_KP5,	KEY_KP6,	KEY_KPPLUS,	KEY_KP1,
	KEY_KP2,	KEY_KP3,	KEY_KP0,	KEY_KPDOT,
	NONE,		NONE,		KEY_102ND,	KEY_F11,
	KEY_F12,	NONE,		NONE,		NONE,
	NONE,		KEY_F13,	NONE,		NONE,
	/* 0x60 - 0x7f */
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	KEY_KATAKANAHIRAGANA,	KEY_HANGEUL,	KEY_HANJA,	KEY_RO,
	NONE,		NONE,	KEY_ZENKAKUHANKAKU,	KEY_HIRAGANA,
	KEY_KATAKANA,	KEY_HENKAN,	NONE,		KEY_MUHENKAN,
	NONE,		KEY_YEN,	KEY_KPCOMMA,	NONE,
	/* 0x00 - 0x1f. 0xE0 prefixed */
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	KEY_PREVIOUSSONG,	NONE,	NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		KEY_NEXTSONG,	NONE,		NONE,
	KEY_KPENTER,	KEY_RIGHTCTRL,	NONE,		NONE,
	/* 0x20 - 0x3f. 0xE0 prefixed */
	KEY_MUTE,	KEY_CALC,	KEY_PLAYPAUSE,	NONE,
	KEY_STOPCD,	NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		KEY_VOLUMEDOWN,	NONE,
	KEY_VOLUMEUP,	NONE,		KEY_HOMEPAGE,	NONE,
	NONE,		KEY_KPSLASH,	NONE,		KEY_SYSRQ,
	KEY_RIGHTALT,	NONE,		NONE,		KEY_F13,
	KEY_F14,	KEY_F15,	KEY_F16,	KEY_F17,
	/* 0x40 - 0x5f. 0xE0 prefixed */
	KEY_F18,	KEY_F19,	KEY_F20,	KEY_F21,
	KEY_F22,	NONE,		KEY_PAUSE,	KEY_HOME,
	KEY_UP,		KEY_PAGEUP,	NONE,		KEY_LEFT,
	NONE,		KEY_RIGHT,	NONE,		KEY_END,
	KEY_DOWN,	KEY_PAGEDOWN,	KEY_INSERT,	KEY_DELETE,
	NONE,		NONE,		NONE,		KEY_F23,
	KEY_F24,	NONE,		NONE,		KEY_LEFTMETA,
	KEY_RIGHTMETA,	KEY_MENU,	KEY_POWER,	KEY_SLEEP,
	/* 0x60 - 0x7f. 0xE0 prefixed */
	NONE,		NONE,		NONE,		KEY_WAKEUP,
	NONE,		KEY_SEARCH,	KEY_BOOKMARKS,	KEY_REFRESH,
	KEY_STOP,	KEY_FORWARD,	KEY_BACK,	KEY_COMPUTER,
	KEY_MAIL,	KEY_MEDIA,	NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
	NONE,		NONE,		NONE,		NONE,
};
/* clang-format on */

uint16_t evdev_scancode2key(int *statep, int scancode) {
  uint16_t keycode;

  /* translate the scan code into a keycode */
  keycode = evdev_at_set1_scancodes[scancode & 0x7f];
  switch (*statep) {
    case 0x00: /* normal scancode */
      switch (scancode) {
        case 0xE0:
        case 0xE1:
          *statep = scancode;
          return NONE;
      }
      break;
    case 0xE0: /* 0xE0 prefix */
      *statep = 0;
      keycode = evdev_at_set1_scancodes[0x80 + (scancode & 0x7f)];
      break;
    case 0xE1: /* 0xE1 prefix */
      /*
       * The pause/break key on the 101 keyboard produces:
       * E1-1D-45 E1-9D-C5
       * Ctrl-pause/break produces:
       * E0-46 E0-C6 (See above.)
       */
      *statep = 0;
      if ((scancode & 0x7f) == 0x1D)
        *statep = scancode;
      return NONE;
      /* NOT REACHED */
    case 0x1D: /* pause / break */
    case 0x9D:
      if ((*statep ^ scancode) & 0x80)
        return NONE;
      *statep = 0;
      if ((scancode & 0x7f) != 0x45)
        return NONE;
      keycode = KEY_PAUSE;
      break;
  }

  return keycode;
}

void evdev_atkbd_support_all_known_keys(evdev_dev_t *evdev) {
  size_t nitems = sizeof(evdev_at_set1_scancodes) / sizeof(uint16_t);
  for (size_t i = KEY_RESERVED; i < nitems; i++)
    if (evdev_at_set1_scancodes[i] != NONE)
      evdev_support_key(evdev, evdev_at_set1_scancodes[i]);
}
