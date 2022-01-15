/*
 * remotetimers.c: A plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 * $Id$
 */

#include <vdr/plugin.h>
#include <vdr/videodir.h>
#include <vdr/interface.h>
#include "remotetimers.h"
#include "setup.h"
#include "menu.h"
#include "watcher.h"
#include "i18n.h"

static const char *VERSION        = "0.1.4-alpha3";
static const char *DESCRIPTION    = trNOOP("Edit timers on remote VDR");
static const char *MAINMENUENTRY  = trNOOP("Remote Timers");

class cPluginRemotetimers : public cPlugin {
public:
  cPluginRemotetimers(void);
  virtual ~cPluginRemotetimers();
  virtual const char *Version(void) { return VERSION; }
  virtual const char *Description(void) { return trREMOTETIMERS(DESCRIPTION); }
  virtual const char *CommandLineHelp(void);
  virtual bool ProcessArgs(int argc, char *argv[]);
  virtual bool Initialize(void);
  virtual bool Start(void);
  virtual void Stop(void);
  virtual void Housekeeping(void);
  virtual void MainThreadHook(void);
  virtual cString Active(void);
  virtual const char *MainMenuEntry(void) { return RemoteTimersSetup.hideMainMenuEntry ? NULL : trREMOTETIMERS(MAINMENUENTRY); }
  virtual cOsdObject *MainMenuAction(void);
  virtual cMenuSetupPage *SetupMenu(void);
  virtual bool SetupParse(const char *Name, const char *Value);
  virtual bool Service(const char *Id, void *Data = NULL);
  virtual const char **SVDRPHelpPages(void);
  virtual cString SVDRPCommand(const char *Command, const char *Option, int &ReplyCode);
  };

cPluginRemotetimers::cPluginRemotetimers(void)
{
  // Initialize any member variables here.
  // DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
  // VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
}

cPluginRemotetimers::~cPluginRemotetimers()
{
  // Clean up after yourself!
}

const char *cPluginRemotetimers::CommandLineHelp(void)
{
  // Return a string that describes all known command line options.
  return NULL;
}

bool cPluginRemotetimers::ProcessArgs(int argc, char *argv[])
{
  // Implement command line argument processing here if applicable.
  return true;
}

bool cPluginRemotetimers::Initialize(void)
{
#if APIVERSNUM < 10712
  PluginRemoteTimers::Folders.Load(AddDirectory(AddDirectory(ConfigDirectory(), ".."), "folders.conf"));
#endif
  return true;
}

bool cPluginRemotetimers::Start(void)
{
  // Start any background activities the plugin shall perform.
#if VDRVERSNUM < 10507
  RegisterI18n(Phrases);
#endif
  cUpdateWatcher::GetInstance()->Reconfigure();
  return true;
}

void cPluginRemotetimers::Stop(void)
{
  // Stop any background activities the plugin shall perform.
  cSvdrp::DeleteInstance();
  cUpdateWatcher::DeleteInstance();
}

void cPluginRemotetimers::Housekeeping(void)
{
  // Perform any cleanup or other regular tasks.
}

void cPluginRemotetimers::MainThreadHook(void)
{
  // Perform actions in the context of the main program thread.
  // WARNING: Use with great care - see PLUGINS.html!
}

cString cPluginRemotetimers::Active(void)
{
  // Return a message string if shutdown should be postponed
  return NULL;
}

cOsdObject *cPluginRemotetimers::MainMenuAction(void)
{
  // Perform the action when selected from the main VDR menu.
  // Return to our recordings menu after replaying a recording from there
  return new ::PluginRemoteTimers::cMenuMain(tr(MAINMENUENTRY), ::PluginRemoteTimers::cMenuRecordings::ReplayEnded() ? osRecordings : osUnknown);
}

cMenuSetupPage *cPluginRemotetimers::SetupMenu(void)
{
  // Return a setup menu in case the plugin supports one.
  return new cRemoteTimersMenuSetup();
}

bool cPluginRemotetimers::SetupParse(const char *Name, const char *Value)
{
  // Parse your own setup parameters and store their values.
  return RemoteTimersSetup.Parse(Name, Value);
}

#define INSTANT_REC_EPG_LOOKAHEAD 60 // seconds to look into the EPG data for an instant recording

bool cPluginRemotetimers::Service(const char *Id, void *Data)
{
  // Handle custom service requests from other plugins

  cSvdrp *svdrp = cSvdrp::GetInstance();

  if (strcmp(Id, "RemoteTimers::InstantRecording-v1.0") == 0) {
     if (Data) {
        RemoteTimers_InstantRecording_v1_0 *ir = (RemoteTimers_InstantRecording_v1_0 *) Data;
         ir->name = ir->fileName = NULL;
         if (ir->pause ? RemoteTimersSetup.remotePause : RemoteTimersSetup.remoteInstant) {
            bool timerAdded = false;
            if (svdrp->Connect()) {
               cRemoteTimer *rt = new cRemoteTimer();
               *(cTimer*) rt = *ir->timer;

               // set file name
               cRecording recording(rt, ir->event);

               PluginRemoteTimers::cMenuTimerItem::UpdateUser(*rt, MASK_FROM_SETUP(RemoteTimersSetup.defaultUser));
               if (RemoteTimers.New(rt) <= rtsRefresh) {
                   timerAdded = true;
                   if (*RemoteTimersSetup.serverDir) {
                      ir->name = cString::sprintf("%s~%s", RemoteTimersSetup.serverDir, recording.Name());

                      int len = strlen(VideoDirectory);
                      ir->fileName = recording.FileName();
                      if (strncmp(ir->fileName, VideoDirectory, strlen(VideoDirectory)) == 0 && ir->fileName[len] == '/') {
                         char *serverDir = ExchangeChars(strdup(RemoteTimersSetup.serverDir), true);
                         ir->fileName = cString::sprintf("%s/%s%s", VideoDirectory, serverDir, ir->fileName + len);
                         free(serverDir);
                      }
                      else {
                         esyslog("RemoteTimers::InstantRecording: Unexpected filename '%s'", *ir->fileName);
                      }
                  }
                  else {
                      ir->name = recording.Name();
                      ir->fileName = recording.FileName();
                  }
               }
               else {
                  esyslog("RemoteTimers::InstantRecording: Failed to add new timer");
                  delete rt;
               }
            }
            svdrp->Disconnect();
            if (!timerAdded)
               return !Interface->Confirm(trREMOTETIMERS("Remote recording failed. Start local recording?"));
         }
         else
            return false;
     }
     return true;
  }

  if (strcmp(Id, "RemoteTimers::RefreshTimers-v1.0") == 0) {
     if (Data) {
        cString *errorMsg = (cString *) Data;
        if (svdrp->Connect() && RemoteTimers.Refresh() == rtsOk) {
           cSchedulesLock SchedulesLock;
           const cSchedules *Schedules = cSchedules::Schedules(SchedulesLock);
           if (Schedules) {
              for (cRemoteTimer *timer = RemoteTimers.First(); timer; timer = RemoteTimers.Next(timer)) {
                 timer->SetEventFromSchedule(Schedules); // make sure the event is current
              }
           }
        }
        else {
           *errorMsg = tr(MSG_UNAVAILABLE);
        }
        svdrp->Disconnect();
        return **errorMsg == NULL;
     }
     return true;
  }

  if (strcmp(Id, "RemoteTimers::ForEach-v1.0") == 0) {
     if (Data) {
        cRemoteTimer *t = *(cRemoteTimer **) Data;
	t = t ? RemoteTimers.First() : RemoteTimers.Next(t);
     }
     return true;
  }

  if (strcmp(Id, "RemoteTimers::GetMatch-v1.0") == 0) {
     if (Data) {
        RemoteTimers_GetMatch_v1_0 *match = (RemoteTimers_GetMatch_v1_0 *) Data;
        match->timer = PluginRemoteTimers::GetBestMatch(match->event, MASK_FROM_SETUP(RemoteTimersSetup.userFilterSchedule), &match->timerMatch, &match->timerType, &match->isRemote);
     }
     return true;
  }

  if (strcmp(Id, "RemoteTimers::GetTimer-v1.0") == 0) {
     if (Data) {
        cTimer *t = *(cTimer **) Data;
        cTimer **result = (cTimer **) Data;
        *result = RemoteTimers.GetTimer(t);
        if (!*result)
           *result = Timers.GetTimer(t);
     }
     return true;
  }

  if (strcmp(Id, "RemoteTimers::GetTimerByEvent-v1.0") == 0) {
     if (Data) {
        RemoteTimers_Event_v1_0 *data = (RemoteTimers_Event_v1_0 *) Data;
        for (cRemoteTimer *t = RemoteTimers.First(); t; t = RemoteTimers.Next(t)) {
           if (t->Event() == data->event) {
              data->timer = t;
              return true;
           }
        }
        for (cTimer *t = Timers.First(); t; t = Timers.Next(t)) {
           if (t->Event() == data->event) {
              data->timer = t;
              return true;
           }
        }
	data->timer = NULL;
        return false;
     }
     return true;
  }

  if (strcmp(Id, "RemoteTimers::NewTimerByEvent-v1.0") == 0) {
     if (Data) {
        RemoteTimers_Event_v1_0 *data = (RemoteTimers_Event_v1_0 *) Data;
        cTimer *t = NULL;
        int tm = tmNone;
        bool isRemote = false;

        // check for timer with user filter
        data->timer = PluginRemoteTimers::GetBestMatch(data->event, MASK_FROM_SETUP(RemoteTimersSetup.userFilterSchedule), &tm, NULL, &isRemote);

        // if timer already exists, we're done
        if (data->timer && tm == tmFull)
           return true;

        // check if timer exists without user filter
        data->timer = PluginRemoteTimers::GetBestMatch(data->event, 0, &tm, NULL, &isRemote);

        // we need a timer object for this event now
        t = new cTimer(data->event);

        // if there's no match by event, maybe there's a match by time?
        if (!data->timer || tm != tmFull) {
           isRemote = true;
           data->timer = RemoteTimers.GetTimer(t);
        }
        if (!data->timer) {
           isRemote = false;
           data->timer = Timers.GetTimer(t);
        }

        if (data->timer) {
           // timer exists
           delete t;
           if (tm != tmFull) 
              return true;
           else {
              // matching timer, but it was filtered
              int user = PluginRemoteTimers::cMenuTimerItem::ParseUser(data->timer);
              PluginRemoteTimers::cMenuTimerItem::UpdateUser(*data->timer, user | MASK_FROM_SETUP(RemoteTimersSetup.defaultUser));
              if (isRemote) {
                 if (svdrp->Connect()) {
                    eRemoteTimersState state = RemoteTimers.Modify((cRemoteTimer*)t);
                    if (state > rtsRefresh)
                       data->errorMsg = tr(RemoteTimers.GetErrorMessage(state));
                 }
                 else {
                    data->errorMsg = tr(MSG_UNAVAILABLE);
                 }
                 svdrp->Disconnect();
              }
              else {
                 Timers.SetModified();
              }
           }
        }
        else {
           RemoteTimers_Timer_v1_0 rt;
           rt.timer = t;
           if (!Service("RemoteTimers::NewTimer-v1.0", &rt))
              data->errorMsg = *rt.errorMsg;
           data->timer = rt.timer;
        }
        return *data->errorMsg == NULL;
     }
     return true;
  }

  if (strcmp(Id, "RemoteTimers::NewTimer-v1.0") == 0) {
     if (Data) {
        RemoteTimers_Timer_v1_0 *data = (RemoteTimers_Timer_v1_0 *) Data;

        PluginRemoteTimers::cMenuTimerItem::UpdateUser(*data->timer, MASK_FROM_SETUP(RemoteTimersSetup.defaultUser));
        if (RemoteTimersSetup.addToRemote) {
           if (svdrp->Connect()) {
              cRemoteTimer *rt = new cRemoteTimer();
              *(cTimer*) rt = *data->timer;
              delete(data->timer);
              data->timer = rt;
              eRemoteTimersState state = RemoteTimers.New(rt);
              if (state > rtsRefresh) {
                 DELETENULL(data->timer);
                 data->errorMsg = tr(RemoteTimers.GetErrorMessage(state));
              }
           }
           else {
              DELETENULL(data->timer);
              data->errorMsg = tr(MSG_UNAVAILABLE);
           }
           svdrp->Disconnect();
        }
        else {
           Timers.Add(data->timer);
           Timers.SetModified();
        }
        if (data->timer)
           isyslog("timer %s added (active)", *data->timer->ToDescr());
        return *data->errorMsg == NULL;
     }
     return true;
  }

  if (strcmp(Id, "RemoteTimers::ModTimer-v1.0") == 0) {
     if (Data) {
        RemoteTimers_Timer_v1_0 *data = (RemoteTimers_Timer_v1_0 *) Data;

        for (cRemoteTimer *t = RemoteTimers.First(); t; t = RemoteTimers.Next(t)) {
           if (t == data->timer) {
              if (svdrp->Connect()) {
                 eRemoteTimersState state = RemoteTimers.Modify(t);
                 if (state > rtsRefresh)
                    data->errorMsg = tr(RemoteTimers.GetErrorMessage(state));
              }
              else {
                 data->errorMsg = tr(MSG_UNAVAILABLE);
              }
              svdrp->Disconnect();
              return *data->errorMsg == NULL;
           }
        }
        for (cTimer *t = Timers.First(); t; t = Timers.Next(t)) {
           if (t == data->timer) {
              Timers.SetModified();
              return true;
           }
        }
        // should not happen
        esyslog("RemoteTimers::ModTimer service: timer not found");
     }
     return true;
  }

  if (strcmp(Id, "RemoteTimers::DelTimer-v1.0") == 0) {
     if (Data) {
        RemoteTimers_Timer_v1_0 *data = (RemoteTimers_Timer_v1_0 *) Data;

        for (cRemoteTimer *t = RemoteTimers.First(); t; t = RemoteTimers.Next(t)) {
           if (t == data->timer) {
              if (svdrp->Connect()) {
                 eRemoteTimersState state = RemoteTimers.Delete(t);
                 if (state > rtsRefresh)
                    data->errorMsg = tr(RemoteTimers.GetErrorMessage(state));
              }
              else {
                 data->errorMsg = tr(MSG_UNAVAILABLE);
              }
              svdrp->Disconnect();
              return *data->errorMsg == NULL;
           }
        }
        for (cTimer *t = Timers.First(); t; t = Timers.Next(t)) {
           if (t == data->timer) {
              Timers.Del(t);
              return true;
           }
        }
        // should not happen
        esyslog("RemoteTimers::DelTimer service: timer not found");
     }
     return true;
  }

  /*
   * MainMenuHooks
   */
  cOsdMenu **menu = (cOsdMenu**) Data;
  if (RemoteTimersSetup.replaceSchedule && strcmp(Id, "MainMenuHooksPatch-v1.0::osSchedule") == 0) {
     if (menu) 
        *menu = new ::PluginRemoteTimers::cMenuSchedule();
     return true;
  }
  if (RemoteTimersSetup.replaceTimers && strcmp(Id, "MainMenuHooksPatch-v1.0::osTimers") == 0) {
     if (menu) 
        *menu = new ::PluginRemoteTimers::cMenuTimers();
     return true;
  }
  if (RemoteTimersSetup.replaceRecordings && strcmp(Id, "MainMenuHooksPatch-v1.0::osRecordings") == 0) {
     if (menu) 
        *menu = new ::PluginRemoteTimers::cMenuRecordings();
     return true;
  }
  return false;
}

const char **cPluginRemotetimers::SVDRPHelpPages(void)
{
  // Return help text for SVDRP commands this plugin implements
  return NULL;
}

cString cPluginRemotetimers::SVDRPCommand(const char *Command, const char *Option, int &ReplyCode)
{
  // Process SVDRP commands this plugin implements
  return NULL;
}

VDRPLUGINCREATOR(cPluginRemotetimers); // Don't touch this!
