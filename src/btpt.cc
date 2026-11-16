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
#include <QWidget>
#include <QTextStream>
#include <QThread>

#include "btpt.h"
#include "eventcodes.h"

/* Directory in which to look for device configuration */
#define BTPT_DIR "/mnt/onboard/.btpt/"

/* Stop Bluetooth heartbeat after this many seconds of inactivity */
#define BLUETOOTH_TIMEOUT (10*60)

static void *(*BluetoothHeartbeat)(void *, long long);
static void (*BluetoothHeartbeat_beat)(void *);

static void *(*MainWindowController_sharedInstance)();
static QWidget *(*MainWindowController_currentView)(void *);

static QObject *(*PowerManager_sharedInstance)();
static int (*PowerManager_filter)(QObject *, QObject *, QEvent *);
static QEvent::Type (*TimeEvent_eventType)();

static void invokeMainWindowController(const char *method)
{
	QString name = QString();
	void *mwc = MainWindowController_sharedInstance();
	if (!mwc) {
		nh_log("invalid MainWindowController");
		return;
	}
	QWidget *cv = MainWindowController_currentView(mwc);
	if (!cv) {
		nh_log("invalid View");
		return;
	}
	name = cv->objectName();
	if (name == "ReadingView") {
		nh_log("%s", method);
		if (!QMetaObject::invokeMethod(cv, method, Qt::QueuedConnection)) {
			const char *fallback = NULL;
			if (strcmp(method, "nextChapter") == 0) {
				fallback = "goToNextChapter";
			} else if (strcmp(method, "prevChapter") == 0) {
				fallback = "goToPrevChapter";
			}
			if (!fallback ||
			    !QMetaObject::invokeMethod(cv, fallback, Qt::QueuedConnection)) {
				nh_log("ReadingView has no invokable method %s", method);
			}
		}
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

			for (int i = 0; i < device.cfg.size(); i++) {
				auto &pair = device.cfg[i];
				struct input_event test = pair.first;
				if (e.type == test.type &&
				    e.code == test.code &&
				    e.value == test.value) {
					/* Update Bluetooth heartbeat time */
					clock_gettime(CLOCK_MONOTONIC,
					              &last_event);

					/* Update PowerManager::timeLastUsed */
					emit notify();

					/* Invoke the configured method */
					QByteArray method = pair.second.toLatin1();
					invokeMainWindowController(
						method.constData());
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

static int btpt_init()
{
	mkdir(BTPT_DIR, 0755);
	TimeLastUsedUpdater *timeLastUsedUpdater = new TimeLastUsedUpdater();
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
