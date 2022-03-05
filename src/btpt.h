#ifndef BTPT_H
#define BTPT_H

#include <linux/input.h>
#include <linux/uinput.h>

#include <QFileSystemWatcher>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QPair>
#include <QThread>
#include <QWaitCondition>

class Device
{
public:
	int fd;
	QList<QPair<struct input_event, QString>> cfg;
};

class BluetoothPageTurner : public QThread
{
	Q_OBJECT

	void run() override;

private:
	bool addDevice(
		const QString &name,
		const QString &uniq,
		const QString &handler);
	bool scanDevices();
	void learn(Device &device);
	QFileSystemWatcher watcher;
	QMap<QString, Device> devices;
	QMutex mutex;
	QWaitCondition newDevice;
	int deviceChanges = 0;

public slots:
	void directoryChanged(const QString &path);

signals:
	void notify();
};

class TimeLastUsedUpdater : public QObject
{
	Q_OBJECT

public slots:
	void notify();
};

#endif /* BTPT_H */
