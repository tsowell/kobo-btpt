#include <QMap>
#include <QString>

#include <map>

#include <linux/input-event-codes.h>

#define VALUE(code) code
#define MAP(code) map[#code] = VALUE(code)

static QMap<QString, int> init()
{
	QMap<QString, int> map;
#include "eventcodes_init.h"
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
