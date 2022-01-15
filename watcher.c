/*
 * watcher.c: monitor server's .update file
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include <vdr/tools.h>
#include <vdr/recording.h>
#include <vdr/videodir.h>
#include "setup.h"
#include "watcher.h"

#define WATCHER_INTERVALL_MS 10000

cUpdateWatcher* cUpdateWatcher::updateWatcher = NULL;

cUpdateWatcher* cUpdateWatcher::GetInstance()
{
	if (!updateWatcher)
		updateWatcher = new cUpdateWatcher();
	return updateWatcher;
}

void cUpdateWatcher::DeleteInstance()
{
	DELETENULL(updateWatcher);
}

cUpdateWatcher::cUpdateWatcher(): cThread("remotetimers update file watcher")
{
	serverUpdateFile = NULL;
}

cUpdateWatcher::~cUpdateWatcher()
{
	condWait.Signal();
	Cancel(1);
	free((void *) serverUpdateFile);
}

void cUpdateWatcher::Reconfigure()
{
	if (Running())
	{
		condWait.Signal();
		Cancel(1);
	}
	free((void *) serverUpdateFile);
	serverUpdateFile = NULL;
	if (RemoteTimersSetup.watchUpdate && RemoteTimersSetup.serverDir[0])
	{
		char *tmpDir = strdup(RemoteTimersSetup.serverDir);
		tmpDir = ExchangeChars(tmpDir, true);
		serverUpdateFile = strdup(AddDirectory(VideoDirectory, AddDirectory(tmpDir, ".update")));
		free(tmpDir);
		dsyslog("remotetimers update file watcher now monitoring '%s'", serverUpdateFile);
		Start();
	}
}

void cUpdateWatcher::Action()
{
	SetPriority(19);
	time_t tLast = LastModifiedTime(serverUpdateFile);

	while (!condWait.Wait(WATCHER_INTERVALL_MS) && Running())
	{
		time_t t = LastModifiedTime(serverUpdateFile);
		if (t != tLast)
		{
			Recordings.Update();
			tLast = t;
		}
	}
}
