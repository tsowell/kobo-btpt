#include <QMap>
#include <QString>

#include <linux/input.h>

#ifndef BTN_SOUTH
#define BTN_SOUTH 0x130
#endif
#ifndef BTN_EAST
#define BTN_EAST 0x131
#endif
#ifndef BTN_NORTH
#define BTN_NORTH 0x133
#endif
#ifndef BTN_WEST
#define BTN_WEST 0x134
#endif
#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP 0x220
#endif
#ifndef BTN_DPAD_DOWN
#define BTN_DPAD_DOWN 0x221
#endif
#ifndef BTN_DPAD_LEFT
#define BTN_DPAD_LEFT 0x222
#endif
#ifndef BTN_DPAD_RIGHT
#define BTN_DPAD_RIGHT 0x223
#endif

#define VALUE(code) code
#define MAP(code) map[#code] = VALUE(code)

static QMap<QString, int> init()
{
	QMap<QString, int> map;
#include "eventcodes_init.h"
	MAP(BTN_SOUTH);
	MAP(BTN_EAST);
	MAP(BTN_NORTH);
	MAP(BTN_WEST);
	MAP(BTN_DPAD_UP);
	MAP(BTN_DPAD_DOWN);
	MAP(BTN_DPAD_LEFT);
	MAP(BTN_DPAD_RIGHT);
	return map;
}

static QMap<QString, int> codes = init();

/* Parse C-style integer or #define from <linux/input-event-codes.h> */
int parseEventCode(bool *ok, const QString &s)
{
	int value = s.toInt(ok, 0);
	if (*ok) {
		return value;
	}
	else if (codes.contains(s)) {
		*ok = true;
		return codes[s];
	}

	*ok = false;
	return -1;
}
