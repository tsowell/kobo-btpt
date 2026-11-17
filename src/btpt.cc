#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <cstddef>
#include <cstdlib>
#include <sys/stat.h>

#include <linux/uinput.h>

#include <NickelHook.h>

#include <QApplication>
#include <QDir>
#include <QFileSystemWatcher>
#include <QFile>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QWidget>
#include <QTextStream>
#include <QThread>
#include <algorithm>

#include "btpt.h"
#include "eventcodes.h"

/* Directory in which to look for device configuration */
#define BTPT_DIR "/mnt/onboard/.btpt/"

/* Stop Bluetooth heartbeat after this many seconds of inactivity */
#define BLUETOOTH_TIMEOUT (10*60)

static void *(*BluetoothHeartbeat)(void *, long long);
static void (*BluetoothHeartbeat_beat)(void *);

static QObject *(*MainWindowController_sharedInstance)();
static QWidget *(*MainWindowController_currentView)(void *);
static void (*MainWindowController_popView)(void *, QWidget *);

static QObject *(*PowerManager_sharedInstance)();
static int (*PowerManager_filter)(QObject *, QObject *, QEvent *);
static QEvent::Type (*TimeEvent_eventType)();

static TimeLastUsedUpdater *g_uiHelper = NULL;

static bool invokeHome(QObject *mwc, const char *method)
{
	if (strcmp(method, "popToFirstView") == 0) {
		return QMetaObject::invokeMethod(
			mwc, "popToFirstView", Qt::QueuedConnection);
	}
	if (QMetaObject::invokeMethod(mwc, "clear", Qt::QueuedConnection)) {
		return true;
	}
	return QMetaObject::invokeMethod(
		mwc, "popToFirstView", Qt::QueuedConnection);
}

static void invokeMainWindowController(const char *method)
{
	QString name = QString();
	QObject *mwc = MainWindowController_sharedInstance();
	if (!mwc) {
		nh_log("invalid MainWindowController");
		return;
	}

	if (strcmp(method, "home") == 0 ||
	    strcmp(method, "mainMenu") == 0 ||
	    strcmp(method, "clear") == 0 ||
	    strcmp(method, "popToFirstView") == 0) {
		nh_log("%s", method);
		if (!invokeHome(mwc, method)) {
			nh_log("MainWindowController has no invokable home method for %s", method);
		}
		return;
	}

	bool isBack = (strcmp(method, "back") == 0 ||
	               strcmp(method, "goBack") == 0 ||
	               strcmp(method, "closeBook") == 0 ||
	               strcmp(method, "backRequested") == 0);
	bool isUiAction = (isBack ||
	                   strcmp(method, "navUp") == 0 ||
	                   strcmp(method, "navDown") == 0 ||
	                   strcmp(method, "navLeft") == 0 ||
	                   strcmp(method, "navRight") == 0 ||
	                   strcmp(method, "nextPage") == 0 ||
	                   strcmp(method, "prevPage") == 0 ||
	                   strcmp(method, "ok") == 0 ||
	                   strcmp(method, "menu") == 0 ||
	                   strcmp(method, "toggleMenu") == 0 ||
	                   strcmp(method, "readingMenu") == 0 ||
	                   strcmp(method, "backOrMenu") == 0);

	QWidget *cv = MainWindowController_currentView(mwc);
	if (!cv) {
		nh_log("invalid View");
		return;
	}
	name = cv->objectName();

	if (isUiAction && g_uiHelper &&
	    QMetaObject::invokeMethod(
		    g_uiHelper, "handleUiAction",
		    Qt::QueuedConnection,
		    Q_ARG(QString, QString::fromLatin1(method)),
		    Q_ARG(QObject *, cv),
		    Q_ARG(QString, name))) {
		nh_log("%s (%s) -> handleUiAction", method, name.toUtf8().constData());
		return;
	}

	if (name == "ReadingView") {
		const char *target = method;
		const char *fallback = NULL;

		if (isBack) {
			target = "backRequested";
		} else if (strcmp(method, "menu") == 0 ||
		           strcmp(method, "openMenu") == 0 ||
		           strcmp(method, "readingMenu") == 0) {
			target = "onHeaderTapped";
		} else if (strcmp(method, "closeMenu") == 0) {
			target = "closeFooterMenu";
		} else if (strcmp(method, "nextChapter") == 0) {
			fallback = "goToNextChapter";
		} else if (strcmp(method, "prevChapter") == 0) {
			fallback = "goToPrevChapter";
		}

		nh_log("%s -> %s", method, target);
		if (!QMetaObject::invokeMethod(cv, target, Qt::QueuedConnection)) {
			if (fallback &&
			    QMetaObject::invokeMethod(cv, fallback, Qt::QueuedConnection)) {
				return;
			}
			if (isBack && invokeHome(mwc, "clear")) {
				return;
			}
			nh_log("ReadingView has no invokable method %s", method);
		}
	}
	else if (isBack) {
		nh_log("%s (outside ReadingView -> home)", method);
		if (!invokeHome(mwc, "clear")) {
			nh_log("MainWindowController has no invokable home method for %s", method);
		}
	}
	else if (QMetaObject::invokeMethod(mwc, method, Qt::QueuedConnection)) {
		nh_log("MainWindowController::%s", method);
	}
	else {
		nh_log("not reading view");
	}
}

BluetoothPageTurner::BluetoothPageTurner()
{
	/* FileSystemWatcher will increment deviceChanges and wake us up when
	 * /dev/input changes */
	QObject::connect(
		&watcher, &QFileSystemWatcher::directoryChanged,
		this, &BluetoothPageTurner::directoryChanged);
	watcher.addPath("/dev/input");
}

bool BluetoothPageTurner::addDevice(
	const QString &name, const QString &uniq, const QString &handler)
{
	int fd;

	if (devices.contains(handler)) {
		return false;
	}

	/* Fall back to device name */
	QFile cfg(BTPT_DIR + name);

	/* Look for case-insensitive Bluetooth address in BTPT_DIR */
	if (!uniq.isEmpty()) {
		QStringList list = QDir(BTPT_DIR).entryList(QDir::Files);
		for (int i = 0; i < list.size(); ++i) {
			QString filename = list.at(i);
			if (filename.toLower() == uniq.toLower()) {
				cfg.setFileName(BTPT_DIR + filename);
			}
		}
	}

	if (!cfg.open(QIODevice::ReadOnly | QIODevice::Text)) {
		nh_log("unable to open %s",
		       cfg.fileName().toStdString().c_str());
		return false;
	}

	QByteArray text = cfg.readAll();
	cfg.close();
	QTextStream in(&text);

	QList<QPair<struct input_event, QString>> parsedCfg;

	while (!in.atEnd()) {
		/* Configuration file format is, one per line:
		 *
		 * METHOD TYPE CODE VALUE
		 *
		 * Invoke METHOD on MainWindowController when input event
		 * matches TYPE, CODE, and VALUE.
		 *
		 * TYPE, CODE, and VALUE can be #defines from
		 * <linux/input-event-codes.h>
		 */

		QString line = in.readLine();

		/* Support comments starting with '#' and blank lines */
		line = line.section('#', 0, 0);
		line.replace('\t', ' ');
		line = line.trimmed();
		if (line.isEmpty()) {
			continue;
		}

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
		QList<QString> parts = line.split(" ", Qt::SkipEmptyParts);
#else
		QList<QString> parts = line.split(" ", QString::SkipEmptyParts);
#endif
		if (parts.size() != 4) {
			nh_log("invalid config line: %s",
			       line.toStdString().c_str());
			return false;
		}

		struct input_event event = { 0 };
		bool ok;

		event.type = parseEventCode(&ok, parts[1]);
		if (!ok) {
			nh_log("invalid type: %s",
			       parts[1].toStdString().c_str());
			return false;
		}

		event.code = parseEventCode(&ok, parts[2]);
		if (!ok) {
			nh_log("invalid code: %s",
			       parts[2].toStdString().c_str());
			return false;
		}

		event.value = parseEventCode(&ok, parts[3]);
		if (!ok) {
			nh_log("invalid value: %s",
			       parts[3].toStdString().c_str());
			return false;
		}

		QPair<struct input_event, QString> map(event, parts[0]);
		parsedCfg.append(map);
	}

	if (parsedCfg.isEmpty()) {
		nh_log("empty config file: %s",
		       cfg.fileName().toStdString().c_str());
		return false;
	}

	QString path = "/dev/input/" + handler;
	fd = open(path.toStdString().c_str(), O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		nh_log("error opening %s",
		       path.toStdString().c_str());
		return false;
	}

	if (fd >= FD_SETSIZE) {
		nh_log("fd %d exceeds FD_SETSIZE for %s",
		       fd, path.toStdString().c_str());
		close(fd);
		return false;
	}

	/* Found a config file and a /dev/input handler, so configure device */
	Device &device = devices[handler];
	device.fd = fd;
	device.cfg = parsedCfg;

	nh_log("acquired device %s (%s): %s",
	       handler.toStdString().c_str(),
	       name.toStdString().c_str(),
	       uniq.toStdString().c_str());

	return true;
}

void BluetoothPageTurner::directoryChanged(const QString &path)
{
	(void)path;
	mutex.lock();
	deviceChanges++;
	newDevice.wakeAll();
	mutex.unlock();
}

bool BluetoothPageTurner::scanDevices()
{
	bool devicesAdded = false;

	QFile file("/proc/bus/input/devices");
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		nh_log("error opening /proc/bus/input/devices");
		return devicesAdded;
	}

	nh_log("checking /proc/bus/input/devices for new devices");

	QByteArray text = file.readAll();

	file.close();

	QTextStream in(&text);
	QString i;
	QString u;
	QString n;
	while (!in.atEnd()) {
		QString line = in.readLine();
		QString type = line.section(": ", 0, 0);
		if (type == "I") {
			i = line.section(": ", 1);
			u.clear();
			n.clear();
		}
		else if (type == "U") {
			/* Bluetooth address without the ':'s */
			u = line.section(": Uniq=", 1);
			u.remove(':');
		}
		else if (type == "N") {
			/* Bluetooth name without surrounding double-quotes */
			n = line.section(": Name=", 1);
			if (n.startsWith('"') && n.endsWith('"') && n.size() >= 2) {
				n.chop(1);
				n.remove(0, 1);
			}
		}
		else if (type == "H") {
			/* Skip if not Bluetooth device */
			if (!i.startsWith("Bus=0005 ")) {
				nh_log("skipping %s", i.toStdString().c_str());
				continue;
			}

			/* Configure the device */
			nh_log("found %s", i.toStdString().c_str());
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
			QList<QString> handlers =
				line.section("Handlers=", 1).split(" ", Qt::SkipEmptyParts);
#else
			QList<QString> handlers =
				line.section("Handlers=", 1).split(" ", QString::SkipEmptyParts);
#endif
			foreach(const QString &handler, handlers) {
				if (handler.startsWith("event")) {
					devicesAdded |=
						addDevice(n, u, handler);
				}
			}
		}
	}

	nh_log("devices scanned");

	return devicesAdded;
}

void BluetoothPageTurner::run()
{
	void *bluetoothHeartbeat = NULL;
	fd_set rfds;
	struct timeval tv;
	struct timespec last_event = { 0 };
	struct timespec now = { 0 };

	nh_log("starting");

	while (1) {
		struct input_event e;
		int ret;

		/* Scan input devices for new devices */
		mutex.lock();
		if (deviceChanges == 0 && devices.empty()) {
			nh_log("waiting for input devices");
			newDevice.wait(&mutex);
			deviceChanges = 0;
			mutex.unlock();
			if (!scanDevices()) {
				continue;
			}
		}
		else if (deviceChanges > 0) {
			deviceChanges = 0;
			mutex.unlock();
			if (!scanDevices() && devices.empty()) {
				continue;
			}
		}
		else {
			mutex.unlock();
		}

		/* Bluetooth heartbeat to prevent Bluetooth from turning off */
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (BluetoothHeartbeat && BluetoothHeartbeat_beat &&
		    last_event.tv_sec > 0 &&
		    now.tv_sec - last_event.tv_sec < BLUETOOTH_TIMEOUT) {
			if (bluetoothHeartbeat == NULL) {
				bluetoothHeartbeat = calloc(1, 1024);
				if (bluetoothHeartbeat != NULL) {
					new(bluetoothHeartbeat) QObject();
					BluetoothHeartbeat(bluetoothHeartbeat, 0);
				}
			}
			if (bluetoothHeartbeat != NULL) {
				BluetoothHeartbeat_beat(bluetoothHeartbeat);
			}
		}

		/* Poll devices for input events */
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);
		int maxfd = -1;

		for (auto it = devices.constBegin(); it != devices.constEnd(); ++it) {
			int fd = it.value().fd;
			if (fd >= 0 && fd < FD_SETSIZE) {
				FD_SET(fd, &rfds);
				if (fd > maxfd) {
					maxfd = fd;
				}
			}
		}

		if (maxfd < 0) {
			continue;
		}

		ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
		if (ret <= 0) {
			continue;
		}

		/* Process events for each device */
		for (auto it = devices.begin(); it != devices.end();) {
			Device &device = it.value();

			if (!FD_ISSET(device.fd, &rfds)) {
				it++;
				continue;
			}

			ret = read(device.fd, &e, sizeof(e));
			if (ret < (int)sizeof(e)) {
				if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
					it++;
					continue;
				}
				nh_log("lost device %s", it.key().toStdString().c_str());
				close(device.fd);
				it = devices.erase(it);
				continue;
			}

			if (e.type == 0x00 &&
			    e.code == 0x00 &&
			    e.value == 0x0001) {
				nh_log("lost device %s", it.key().toStdString().c_str());
				close(device.fd);
				it = devices.erase(it);
				continue;
			}

			if (e.type == EV_ABS) {
				if (e.code == ABS_X) {
					device.last_abs_x = e.value;
					device.has_abs_x = true;
					device.saw_abs_in_packet = true;
				} else if (e.code == ABS_Y) {
					device.last_abs_y = e.value;
					device.has_abs_y = true;
					device.saw_abs_in_packet = true;
				}
			} else if (e.type == EV_KEY && e.code == BTN_TOUCH) {
				if (e.value == 1) {
					device.touch_down = true;
					device.saw_abs_in_packet = false;
				} else {
					device.touch_down = false;
				}
			}

			auto dispatchEvent = [&](const struct input_event &ev) -> bool {
				for (int i = 0; i < device.cfg.size(); i++) {
					auto &pair = device.cfg[i];
					struct input_event test = pair.first;
					if (ev.type == test.type &&
					    ev.code == test.code &&
					    ev.value == test.value) {
						struct timespec ev_now = { 0 };
						clock_gettime(CLOCK_MONOTONIC, &ev_now);
						long long elapsed_ms =
							(ev_now.tv_sec - last_event.tv_sec) * 1000LL +
							(ev_now.tv_nsec - last_event.tv_nsec) / 1000000LL;
						if (last_event.tv_sec > 0 && elapsed_ms >= 0 && elapsed_ms < 200) {
							return true;
						}

						/* Update Bluetooth heartbeat time */
						last_event = ev_now;

						/* Update PowerManager::timeLastUsed */
						emit notify();

						/* Invoke the configured method */
						QByteArray method = pair.second.toLatin1();
						invokeMainWindowController(
							method.constData());
						return true;
					}
				}
				return false;
			};

			bool matched = dispatchEvent(e);

			if (!matched && e.type == EV_SYN && e.code == SYN_REPORT &&
			    device.touch_down && !device.saw_abs_in_packet) {
				if (device.has_abs_x) {
					struct input_event synth = { 0 };
					synth.type = EV_ABS;
					synth.code = ABS_X;
					synth.value = device.last_abs_x;
					matched = dispatchEvent(synth);
				}
				if (!matched && device.has_abs_y) {
					struct input_event synth = { 0 };
					synth.type = EV_ABS;
					synth.code = ABS_Y;
					synth.value = device.last_abs_y;
					matched = dispatchEvent(synth);
				}
			}

			if (!matched && e.type != EV_SYN &&
			    access(BTPT_DIR "debug", F_OK) == 0) {
				FILE *dbg = fopen(BTPT_DIR "events.log", "a");
				if (dbg) {
					fprintf(dbg, "%s: type=%u code=%u value=%d\n",
					        it.key().toStdString().c_str(),
					        e.type, e.code, e.value);
					fclose(dbg);
				}
			}

			it++;
		}
	}
}

void TimeLastUsedUpdater::notify()
{
	if (!TimeEvent_eventType || !PowerManager_sharedInstance || !PowerManager_filter) {
		return;
	}

	/* Update PowerManager::timeLastUsed to prevent sleep */
	QEvent timeEvent(TimeEvent_eventType());

	QObject *pm = PowerManager_sharedInstance();
	if (pm == NULL) {
		nh_log("invalid PowerManager");
		return;
	}

	PowerManager_filter(pm, this, &timeEvent);
}

static bool isClickableMenuItem(QWidget *w, QWidget *overlayRoot, QWidget *highlight)
{
	if (!w || w == highlight || w == overlayRoot ||
	    !w->isVisible() || !w->isEnabled()) {
		return false;
	}
	if (w->width() < 20 || w->height() < 20 || w->visibleRegion().isEmpty()) {
		return false;
	}
	if (overlayRoot &&
	    w->width() > overlayRoot->width() * 9 / 10 &&
	    w->height() > overlayRoot->height() * 9 / 10) {
		return false;
	}
	const char *cls = w->metaObject()->className();
	if (strcmp(cls, "ReadingMenuView") == 0 ||
	    strcmp(cls, "ReadingView") == 0 ||
	    strcmp(cls, "MainNavView") == 0) {
		return false;
	}
	if (w->inherits("QAbstractButton") || w->inherits("TouchLabel")) {
		return true;
	}
	const QMetaObject *mo = w->metaObject();
	return mo->indexOfSignal("tapped(bool)") >= 0 ||
	       mo->indexOfSignal("tapped()") >= 0 ||
	       mo->indexOfSignal("clicked()") >= 0;
}

static void sortWidgetsReadingOrder(QWidget *overlayRoot, QList<QWidget *> &out)
{
	std::sort(out.begin(), out.end(), [overlayRoot](QWidget *a, QWidget *b) {
		QPoint pa = a->mapTo(overlayRoot, QPoint(0, 0));
		QPoint pb = b->mapTo(overlayRoot, QPoint(0, 0));
		int rowA = pa.y() / 40;
		int rowB = pb.y() / 40;
		if (rowA != rowB) {
			return rowA < rowB;
		}
		return pa.x() < pb.x();
	});
}

static void collectReadingMenuWidgets(
	QWidget *overlayRoot, QWidget *highlight,
	QList<QWidget *> &out, bool &menuOpen)
{
	menuOpen = false;
	QList<QWidget *> menus;
	const QList<QWidget *> all = overlayRoot->findChildren<QWidget *>();
	for (int i = 0; i < all.size(); ++i) {
		QWidget *w = all.at(i);
		if (w->isVisible() && w->width() > 50 && w->height() > 50 &&
		    !w->visibleRegion().isEmpty() &&
		    (w->inherits("ReadingMenuView") ||
		     strcmp(w->metaObject()->className(), "ReadingMenuView") == 0)) {
			menuOpen = true;
			menus.append(w);
		}
	}
	if (!menuOpen) {
		return;
	}

	for (int m = 0; m < menus.size(); ++m) {
		QWidget *menu = menus.at(m);
		const QList<QWidget *> children = menu->findChildren<QWidget *>();
		for (int i = 0; i < children.size(); ++i) {
			QWidget *w = children.at(i);
			if (isClickableMenuItem(w, overlayRoot, highlight) &&
			    !out.contains(w)) {
				out.append(w);
			}
		}
	}

	sortWidgetsReadingOrder(overlayRoot, out);
}

static void collectInteractiveWidgets(
	QWidget *searchRoot, QWidget *overlayRoot, QWidget *highlight,
	QList<QWidget *> &out)
{
	if (!searchRoot) {
		return;
	}
	const QList<QWidget *> children = searchRoot->findChildren<QWidget *>();
	for (int i = 0; i < children.size(); ++i) {
		QWidget *w = children.at(i);
		if (w != searchRoot &&
		    isClickableMenuItem(w, overlayRoot, highlight) &&
		    !out.contains(w)) {
			out.append(w);
		}
	}
	sortWidgetsReadingOrder(overlayRoot, out);
}

static QWidget *findSpatialNext(
	const QList<QWidget *> &items, QWidget *current,
	QWidget *overlayRoot, const QString &dir)
{
	if (items.isEmpty()) {
		return nullptr;
	}
	int idx = current ? items.indexOf(current) : -1;
	if (idx < 0) {
		return (dir == "navUp" || dir == "navLeft" || dir == "prevPage")
			? items.last()
			: items.first();
	}
	if (dir == "nextPage" || dir == "prevPage") {
		int step = (dir == "nextPage") ? 1 : -1;
		return items.at((idx + step + items.size()) % items.size());
	}

	QPoint cur = current->mapTo(
		overlayRoot, QPoint(current->width() / 2, current->height() / 2));
	QWidget *best = nullptr;
	int bestScore = 0x7fffffff;

	for (int i = 0; i < items.size(); ++i) {
		QWidget *w = items.at(i);
		if (w == current) {
			continue;
		}
		QPoint cand = w->mapTo(
			overlayRoot, QPoint(w->width() / 2, w->height() / 2));
		int dx = cand.x() - cur.x();
		int dy = cand.y() - cur.y();
		int score = -1;

		if (dir == "navRight" && dx > 5) {
			score = dx + 2 * std::abs(dy);
		} else if (dir == "navLeft" && dx < -5) {
			score = (-dx) + 2 * std::abs(dy);
		} else if (dir == "navDown" && dy > 5) {
			score = dy + 2 * std::abs(dx);
		} else if (dir == "navUp" && dy < -5) {
			score = (-dy) + 2 * std::abs(dx);
		}

		if (score >= 0 && score < bestScore) {
			bestScore = score;
			best = w;
		}
	}

	if (best) {
		return best;
	}
	int fallbackStep = (dir == "navLeft" || dir == "navUp") ? -1 : 1;
	return items.at((idx + fallbackStep + items.size()) % items.size());
}

static void activateMenuItem(QWidget *w)
{
	if (!w) {
		return;
	}
	if (w->inherits("QAbstractButton")) {
		if (QMetaObject::invokeMethod(w, "click", Qt::QueuedConnection)) {
			return;
		}
	}
	if (QMetaObject::invokeMethod(w, "tapped", Qt::QueuedConnection, Q_ARG(bool, false))) {
		return;
	}
	if (QMetaObject::invokeMethod(w, "tapped", Qt::QueuedConnection)) {
		return;
	}
	QPoint center(w->width() / 2, w->height() / 2);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QPointF localPos(center);
	QPointF globalPos(w->mapToGlobal(center));
	QMouseEvent press(QEvent::MouseButtonPress, localPos, globalPos,
	                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent release(QEvent::MouseButtonRelease, localPos, globalPos,
	                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
#else
	QMouseEvent press(QEvent::MouseButtonPress, center, w->mapToGlobal(center),
	                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent release(QEvent::MouseButtonRelease, center, w->mapToGlobal(center),
	                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
#endif
	QApplication::sendEvent(w, &press);
	QApplication::sendEvent(w, &release);
}

void TimeLastUsedUpdater::handleUiAction(
	const QString &method, QObject *cvObj, const QString &viewName)
{
	QWidget *cv = qobject_cast<QWidget *>(cvObj);
	if (!cv) {
		return;
	}

	QWidget *win = cv->window() ? cv->window() : cv;
	QWidget *modal = QApplication::activePopupWidget();
	if (!modal) {
		modal = QApplication::activeModalWidget();
	}
	bool modalOpen = (modal && modal != win && modal->isVisible() &&
	                  modal->width() > 50 && modal->height() > 50);
	QWidget *overlayRoot = modalOpen ? modal : win;

	auto hideHighlight = [this]() {
		if (m_highlight) {
			m_highlight->hide();
		}
		m_selectedWidget = nullptr;
		m_menuIndex = -1;
	};

	auto showHighlight = [this, overlayRoot](QWidget *target) {
		if (!target) {
			return;
		}
		if (!m_highlight || m_highlight->parentWidget() != overlayRoot) {
			if (m_highlight) {
				m_highlight->deleteLater();
			}
			m_highlight = new QWidget(overlayRoot);
			m_highlight->setAttribute(Qt::WA_TransparentForMouseEvents, true);
			m_highlight->setStyleSheet(
				"background: transparent; border: 3px solid black;");
		}
		QPoint pos = target->mapTo(overlayRoot, QPoint(0, 0));
		m_highlight->setGeometry(pos.x(), pos.y(),
		                         target->width(), target->height());
		m_highlight->raise();
		m_highlight->show();
		m_selectedWidget = target;
	};

	bool isReading = (viewName == "ReadingView");
	bool menuOpen = false;
	QList<QWidget *> items;

	if (isReading && !modalOpen) {
		collectReadingMenuWidgets(
			overlayRoot, m_highlight.data(), items, menuOpen);

		if (method == "menu" || method == "toggleMenu" ||
		    method == "readingMenu" || method == "backOrMenu") {
			hideHighlight();
			const char *slot = menuOpen ? "closeFooterMenu" : "onHeaderTapped";
			nh_log("handleUiAction(ReadingView): menuOpen=%d -> %s",
			       menuOpen ? 1 : 0, slot);
			QMetaObject::invokeMethod(cv, slot, Qt::QueuedConnection);
			return;
		}

		if (!menuOpen) {
			hideHighlight();
			if (method == "ok" || method == "closeBook" ||
			    method == "back" || method == "goBack" ||
			    method == "backRequested") {
				if (!QMetaObject::invokeMethod(
					    cv, "backRequested", Qt::QueuedConnection)) {
					QObject *mwc = MainWindowController_sharedInstance();
					if (mwc) {
						invokeHome(mwc, "clear");
					}
				}
			} else if (method == "navLeft" || method == "navUp" ||
			           method == "nextPage") {
				QMetaObject::invokeMethod(cv, "nextPage", Qt::QueuedConnection);
			} else if (method == "navRight" || method == "navDown" ||
			           method == "prevPage") {
				QMetaObject::invokeMethod(cv, "prevPage", Qt::QueuedConnection);
			}
			return;
		}
	} else {
		if (method == "menu" || method == "toggleMenu" ||
		    method == "readingMenu" || method == "backOrMenu") {
			hideHighlight();
			if (modalOpen) {
				modal->close();
				return;
			}
			QStackedWidget *stack =
				qobject_cast<QStackedWidget *>(cv->parentWidget());
			QObject *mwc = MainWindowController_sharedInstance();
			if (mwc && MainWindowController_popView && stack &&
			    stack->count() > 1 && viewName != "HomePageView") {
				nh_log("handleUiAction: popView(%s)",
				       viewName.toUtf8().constData());
				MainWindowController_popView(mwc, cv);
				return;
			}
			if (mwc && viewName != "HomePageView") {
				invokeHome(mwc, "clear");
			}
			return;
		}

		collectInteractiveWidgets(
			modalOpen ? modal : overlayRoot,
			overlayRoot, m_highlight.data(), items);
	}

	if (items.isEmpty()) {
		if (isReading && menuOpen) {
			if (method == "nextPage" || method == "prevPage") {
				QMetaObject::invokeMethod(
					cv, method.toLatin1().constData(),
					Qt::QueuedConnection);
			} else if (method == "ok" || method == "closeBook" ||
			           method == "back" || method == "goBack" ||
			           method == "backRequested") {
				hideHighlight();
				QMetaObject::invokeMethod(
					cv, "closeFooterMenu", Qt::QueuedConnection);
			}
		}
		return;
	}

	if (method == "navUp" || method == "navDown" ||
	    method == "navLeft" || method == "navRight" ||
	    method == "nextPage" || method == "prevPage") {
		QWidget *next = findSpatialNext(
			items, m_selectedWidget.data(), overlayRoot, method);
		m_menuIndex = items.indexOf(next);
		showHighlight(next);
		return;
	}

	if (method == "ok") {
		QWidget *target = m_selectedWidget.data();
		if (!target || !items.contains(target)) {
			m_menuIndex = 0;
			target = items.at(0);
		}
		nh_log("OK on UI item: %s", target->metaObject()->className());
		hideHighlight();
		activateMenuItem(target);
		return;
	}

	if (method == "closeBook" || method == "back" ||
	    method == "goBack" || method == "backRequested") {
		hideHighlight();
		if (modalOpen) {
			modal->close();
		} else if (isReading && menuOpen) {
			QMetaObject::invokeMethod(
				cv, "closeFooterMenu", Qt::QueuedConnection);
		} else {
			QObject *mwc = MainWindowController_sharedInstance();
			if (mwc) {
				invokeHome(mwc, "clear");
			}
		}
	}
}

static int btpt_init()
{
	mkdir(BTPT_DIR, 0755);
	TimeLastUsedUpdater *timeLastUsedUpdater = new TimeLastUsedUpdater();
	g_uiHelper = timeLastUsedUpdater;
	BluetoothPageTurner *btpt = new BluetoothPageTurner();
	QObject::connect(
		btpt, &BluetoothPageTurner::notify,
		timeLastUsedUpdater, &TimeLastUsedUpdater::notify,
		Qt::QueuedConnection);
	QObject::connect(
		btpt, &QThread::finished,
		btpt, &QObject::deleteLater);
	QObject::connect(
		btpt, &QThread::finished,
		timeLastUsedUpdater, &QObject::deleteLater);
	btpt->start();

	return 0;
}

static struct nh_info btpt_info = {
	.name           = "BluetoothPageTurner",
	.desc           = "Turn pages with Bluetooth device",
	.uninstall_flag = BTPT_DIR "uninstall",
};

static struct nh_hook btpt_hook[] = {
	{0},
};

static struct nh_dlsym btpt_dlsym[] = {
	{
		.name = "_ZN18BluetoothHeartbeatC1Ex",
		.out = nh_symoutptr(BluetoothHeartbeat),
		.desc = NULL,
		.optional = true
	},
	{
		.name = "_ZN18BluetoothHeartbeat4beatEv",
		.out = nh_symoutptr(BluetoothHeartbeat_beat),
		.desc = NULL,
		.optional = true
	},
	{
		.name = "_ZN20MainWindowController14sharedInstanceEv",
		.out = nh_symoutptr(MainWindowController_sharedInstance)
	},
	{
		.name = "_ZNK20MainWindowController11currentViewEv",
		.out = nh_symoutptr(MainWindowController_currentView)
	},
	{
		.name = "_ZN20MainWindowController7popViewEP7QWidget",
		.out = nh_symoutptr(MainWindowController_popView),
		.desc = NULL,
		.optional = true
	},
	{
		.name = "_ZN9TimeEvent9eventTypeEv",
		.out = nh_symoutptr(TimeEvent_eventType),
		.desc = NULL,
		.optional = true
	},
	{
		.name = "_ZN12PowerManager14sharedInstanceEv",
		.out = nh_symoutptr(PowerManager_sharedInstance),
		.desc = NULL,
		.optional = true
	},
	{
		.name = "_ZN12PowerManager6filterEP7QObjectP6QEvent",
		.out = nh_symoutptr(PowerManager_filter),
		.desc = NULL,
		.optional = true
	},
	{0},
};

NickelHook(
	.init  = btpt_init,
	.info  = &btpt_info,
	.hook  = btpt_hook,
	.dlsym = btpt_dlsym,
)
