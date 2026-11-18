#ifndef BTPT_H
#define BTPT_H

#include <linux/input.h>
#include <linux/uinput.h>

#include <QFileSystemWatcher>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QPair>
#include <QPointer>
#include <QThread>
#include <QWaitCondition>

class QWidget;

class Device
{
public:
	int fd;
	QList<QPair<struct input_event, QString>> cfg;
	int last_abs_x = 0;
	int last_abs_y = 0;
	bool has_abs_x = false;
	bool has_abs_y = false;
	bool touch_down = false;
	bool saw_abs_in_packet = false;
};

class BluetoothPageTurner : public QThread
{
	Q_OBJECT

public:
	BluetoothPageTurner();

protected:
	void run() override;

private:
	bool addDevice(
		const QString &name,
		const QString &uniq,
		const QString &handler);
	bool scanDevices();
	QFileSystemWatcher watcher;
	QMap<QString, Device> devices;
	QMutex mutex;
	QWaitCondition newDevice;
	int deviceChanges = 1;

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
	void handleUiAction(
		const QString &method, QObject *cvObj, const QString &viewName);

private:
	QPointer<QWidget> m_highlight;
	QPointer<QWidget> m_selectedWidget;
	int m_menuIndex = -1;
};

#endif /* BTPT_H */
