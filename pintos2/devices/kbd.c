#include "devices/kbd.h"
#include <ctype.h>
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "devices/input.h"
#include "threads/interrupt.h"
#include "threads/io.h"

/* Keyboard data register port. */
#define DATA_REG 0x60

/* Current state of shift keys.
   True if depressed, false otherwise. */
static bool left_shift, right_shift; /* Left and right Shift keys. */
static bool left_alt, right_alt;	 /* Left and right Alt keys. */
static bool left_ctrl, right_ctrl;	 /* Left and right Ctl keys. */

/* Status of Caps Lock.
   True when on, false when off. */
static bool caps_lock;

/* Number of keys pressed. */
static int64_t key_cnt;

static intr_handler_func keyboard_interrupt;

/* Initializes the keyboard. */
void kbd_init(void)
{
	intr_register_ext(0x21, keyboard_interrupt, "8042 Keyboard");
}

/* Prints keyboard statistics. */
void kbd_print_stats(void)
{
	printf("Keyboard: %lld keys pressed\n", key_cnt);
}

/* Maps a set of contiguous scancodes into characters. */
struct keymap
{
	uint8_t first_scancode; /* First scancode. */
	const char *chars;		/* chars[0] has scancode first_scancode,
							   chars[1] has scancode first_scancode + 1,
							   and so on to the end of the string. */
};

/* Keys that produce the same characters regardless of whether
   the Shift keys are down.  Case of letters is an exception
   that we handle elsewhere.  */
static const struct keymap invariant_keymap[] = {
	{0x01, "\033"},
	{0x0e, "\b"},
	{0x0f, "\tQWERTYUIOP"},
	{0x1c, "\r"},
	{0x1e, "ASDFGHJKL"},
	{0x2c, "ZXCVBNM"},
	{0x37, "*"},
	{0x39, " "},
	{0, NULL},
};

/* Characters for keys pressed without Shift, for those keys
   where it matters. */
static const struct keymap unshifted_keymap[] = {
	{0x02, "1234567890-="},
	{0x1a, "[]"},
	{0x27, ";'`"},
	{0x2b, "\\"},
	{0x33, ",./"},
	{0, NULL},
};

/* Characters for keys pressed with Shift, for those keys where
   it matters. */
static const struct keymap shifted_keymap[] = {
	{0x02, "!@#$%^&*()_+"},
	{0x1a, "{}"},
	{0x27, ":\"~"},
	{0x2b, "|"},
	{0x33, "<>?"},
	{0, NULL},
};

static bool map_key(const struct keymap[], unsigned scancode, uint8_t *);

/**
 * 1. 사용자가 QEMU 창에서 키를 누르거나 뗀다.
 * 2. 키보드 컨트롤러가 스캔코드를 만든다.
 * 3. IRQ 1 이 발생하고, PIC remap 때문에 벡터 0x21로 들어감
 * 4. keyboard_interrupt() 가 실행
 * 5. 이 함수가 inb(0x60)으로 스캔코드를 읽고, Shift/Ctrl/Cap Lock
 * 상태를 반영해서 문자로 해석
 * 6. 최종 문자를 input_putc()로 input_buffer에 넣는다.
 */
static void
keyboard_interrupt(struct intr_frame *args UNUSED)
{
	/* Status of shift keys. */
	bool shift = left_shift || right_shift;
	bool alt = left_alt || right_alt;
	bool ctrl = left_ctrl || right_ctrl;

	/* Keyboard scancode. */
	unsigned code;

	/* False if key pressed, true if key released. */
	bool release;

	/* Character that corresponds to `code'. */
	uint8_t c;

	/* Read scancode, including second byte if prefix code. */
	code = inb(DATA_REG);
	if (code == 0xe0)
		code = (code << 8) | inb(DATA_REG);

	/* Bit 0x80 distinguishes key press from key release
	   (even if there's a prefix). */
	release = (code & 0x80) != 0;
	code &= ~0x80u;

	/* Interpret key. */
	if (code == 0x3a)
	{
		/* Caps Lock. */
		if (!release)
			caps_lock = !caps_lock;
	}
	else if (map_key(invariant_keymap, code, &c) || (!shift && map_key(unshifted_keymap, code, &c)) || (shift && map_key(shifted_keymap, code, &c)))
	{
		/* Ordinary character. */
		if (!release)
		{
			/* Handle Ctrl, Shift.
			   Note that Ctrl overrides Shift. */
			if (ctrl && c >= 0x40 && c < 0x60)
			{
				/* A is 0x41, Ctrl+A is 0x01, etc. */
				c -= 0x40;
			}
			else if (shift == caps_lock)
				c = tolower(c);

			/* Handle Alt by setting the high bit.
			   This 0x80 is unrelated to the one used to
			   distinguish key press from key release. */
			if (alt)
				c += 0x80;

			/* Append to keyboard buffer. */
			if (!input_full())
			{
				key_cnt++;
				input_putc(c);
			}
		}
	}
	else
	{
		/* Maps a keycode into a shift state variable. */
		struct shift_key
		{
			unsigned scancode;
			bool *state_var;
		};

		/* Table of shift keys. */
		static const struct shift_key shift_keys[] = {
			{0x2a, &left_shift},
			{0x36, &right_shift},
			{0x38, &left_alt},
			{0xe038, &right_alt},
			{0x1d, &left_ctrl},
			{0xe01d, &right_ctrl},
			{0, NULL},
		};

		const struct shift_key *key;

		/* Scan the table. */
		for (key = shift_keys; key->scancode != 0; key++)
			if (key->scancode == code)
			{
				*key->state_var = !release;
				break;
			}
	}
}

/* Scans the array of keymaps K for SCANCODE.
   If found, sets *C to the corresponding character and returns
   true.
   If not found, returns false and C is ignored. */
static bool
map_key(const struct keymap k[], unsigned scancode, uint8_t *c)
{
	for (; k->first_scancode != 0; k++)
		if (scancode >= k->first_scancode && scancode < k->first_scancode + strlen(k->chars))
		{
			*c = k->chars[scancode - k->first_scancode];
			return true;
		}

	return false;
}
