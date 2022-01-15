/*
 * Copyright (C) 2008-2011 Frank Schmirler <vdr@schmirler.de>
 *
 * Major parts copied from VDR's menu.c
 * $Id: menu.c 2.54 2012/05/12 13:08:23 kls Exp $
 * Copyright (C) 2000, 2003, 2006, 2008 Klaus Schmidinger
 *
 * This file is part of VDR Plugin remotetimers.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 * Or, point your browser to http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 */

#include "menu.h"
#include "setup.h"
#include "moverec.h"
#include "i18n.h"
#include <vdr/menu.h>
#include "menuitems.h"
//#include <ctype.h>
//#include <limits.h>
//#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>
#include <vdr/channels.h>
//#include "config.h"
#include <vdr/cutter.h>
//#include "eitscan.h"
//#include "i18n.h"
#include <vdr/interface.h>
//#include "plugin.h"
#include <vdr/recording.h>
//#include "remote.h"
//#include "shutdown.h"
//#include "sourceparams.h"
//#include "sources.h"
#include <vdr/status.h>
//#include "themes.h"
#include <vdr/timers.h>
//#include "transfer.h"
#include <vdr/videodir.h>

#include <ctype.h>

namespace PluginRemoteTimers {

//#define MAXWAIT4EPGINFO   3 // seconds
//#define MODETIMEOUT       3 // seconds
#ifdef REMOTETIMERS_DISKSPACE
#define DISKSPACECHEK     5 // seconds between disk space checks
#endif
#define NEWTIMERLIMIT   120 // seconds until the start time of a new timer created from the Schedule menu,
                            // within which it will go directly into the "Edit timer" menu to allow
                            // further parameter settings
//#define DEFERTIMER       60 // seconds by which a timer is deferred in case of problems

//#define MAXRECORDCONTROLS (MAXDEVICES * MAXRECEIVERS)
//#define MAXINSTANTRECTIME (24 * 60 - 1) // 23:59 hours
//#define MAXWAITFORCAMMENU  10 // seconds to wait for the CAM menu to open
//#define CAMMENURETYTIMEOUT  3 // seconds after which opening the CAM menu is retried
//#define CAMRESPONSETIMEOUT  5 // seconds to wait for a response from a CAM
//#define MINFREEDISK       300 // minimum free disk space (in MB) required to start recording
//#define NODISKSPACEDELTA  300 // seconds between "Not enough disk space to start recording!" messages
#define MAXCHNAMWIDTH      16

#define CHNUMWIDTH  (numdigits(Channels.MaxNumber()) + 1)
#if VDRVERSNUM < 10728
#define CHNAMWIDTH  7
#else
#define CHNAMWIDTH  (min(MAXCHNAMWIDTH, Channels.MaxShortChannelNameLength() + 1))
#endif

#define AUX_STARTTAG "<remotetimers>"
#define AUX_ENDTAG "</remotetimers>"

int ParseUser(const char *p) {
  int user = 0;
  char *buffer = NULL;
  if (p && (p = strstr(p, AUX_STARTTAG))) {
      if (sscanf(p, AUX_STARTTAG "%a[^<]" AUX_ENDTAG, &buffer) == 1) {
          user = cMenuEditUserItem::Parse(buffer);
	  free(buffer);
	  }
      }
  return user;
}

cString UpdateUser(const char *s, int User) {
  if (!s)
      s = "";

  const char *tail = s + strlen(s);
  while (tail > s && isspace(*(tail - 1)))
      tail--;

  const char *start = strstr(s, AUX_STARTTAG);
  const char *end = start ? strstr(start, AUX_ENDTAG) : NULL;

  if (start)
      end = end ? end + strlen(AUX_ENDTAG) : start + strlen(AUX_STARTTAG);
  else
      start = end = tail;

  int lenPrefix = start - s;
  int lenPostfix = tail - end;

  if (User == 0)
      return cString::sprintf("%.*s%.*s", lenPrefix, s, lenPostfix, end);
  else
      return cString::sprintf("%.*s" AUX_STARTTAG "%s" AUX_ENDTAG "%.*s", lenPrefix, s, *cMenuEditUserItem::ToString(User), lenPostfix, end);
}

cTimer* GetBestMatch(const cEvent *Event, int UserMask, int *Match, int *Type, bool *Remote)
{
	cTimer *localTimer = NULL, *remoteTimer = NULL;
	int localMatch = tmNone, remoteMatch = tmNone;
	int localUser = 0, remoteUser = 0;

	for (cTimer *t = Timers.First(); t; t = Timers.Next(t)) {
		int tm = t->Matches(Event);
		if (tm > localMatch) {
			int user = ParseUser(t->Aux());
			if (UserMask == 0 || user == 0 || (user & UserMask)) {
				localTimer = t;
				localMatch = tm;
				localUser = user;
				if (localMatch == tmFull)
					break;
			}
		}
	}
	for (cRemoteTimer *t = RemoteTimers.First(); t; t = RemoteTimers.Next(t)) {
		int tm = t->Matches(Event);
		if (tm > remoteMatch) {
			int user = ParseUser(t->Aux());
			if (UserMask == 0 || user == 0 || (user & UserMask)) {
				remoteTimer = t;
				remoteMatch = tm;
				remoteUser = user;
				if (remoteMatch == tmFull)
					break;
			}
		}
	}
	if (Match)
		*Match = remoteMatch > localMatch ? remoteMatch : localMatch;
	if (Type)
		*Type = (remoteMatch > localMatch ? remoteUser : localUser) == 0 ? 0 : 1;
	if (Remote)
		*Remote = remoteMatch > localMatch;
	return remoteMatch > localMatch ? remoteTimer : localTimer;
}


#ifdef REMOTETIMERS_DISKSPACE
// --- cFreeDiskSpace --------------------------------------------------------

#define MB_PER_MINUTE 25.75 // this is just an estimate!

class cFreeDiskSpace {
private:
  static time_t lastDiskSpaceCheck;
  static int lastFreeMB;
  static cString freeDiskSpaceString;
#if VDRVERSNUM >= 10515
  static cString lastPath;
  static int VideoDiskSpace(const char *Dir, int *FreeMB);
  static int DeletedFileSizeMB(const char *Dir);
#endif
public:
#if VDRVERSNUM >= 10515
  static bool HasChanged(const char *SubDir, bool ForceCheck = false);
#else
  static bool HasChanged(bool ForceCheck = false);
#endif
  static const char *FreeDiskSpaceString() { return freeDiskSpaceString; }
  };

time_t cFreeDiskSpace::lastDiskSpaceCheck = 0;
int cFreeDiskSpace::lastFreeMB = 0;
cString cFreeDiskSpace::freeDiskSpaceString;

cFreeDiskSpace FreeDiskSpace;

#if VDRVERSNUM >= 10515
cString cFreeDiskSpace::lastPath("/");

bool cFreeDiskSpace::HasChanged(const char *SubDir, bool ForceCheck)
{
  cString path(ExchangeChars(strdup(SubDir ? SubDir : ""), true), true);
  path = cString::sprintf("%s/%s", VideoDirectory, *path);
  if (ForceCheck || time(NULL) - lastDiskSpaceCheck > DISKSPACECHEK || !EntriesOnSameFileSystem(path, lastPath)) {
     int FreeMB;
     lastPath = path;
     int Percent = IsOnVideoDirectoryFileSystem(path) ? ::VideoDiskSpace(&FreeMB) : VideoDiskSpace(path, &FreeMB);

#else

bool cFreeDiskSpace::HasChanged(bool ForceCheck)
{
  if (ForceCheck || time(NULL) - lastDiskSpaceCheck > DISKSPACECHEK) {
     int FreeMB;
     int Percent = VideoDiskSpace(&FreeMB);
#endif
     lastDiskSpaceCheck = time(NULL);
     if (ForceCheck || FreeMB != lastFreeMB) {
        int Minutes = int(double(FreeMB) / MB_PER_MINUTE);
        int Hours = Minutes / 60;
        Minutes %= 60;
        freeDiskSpaceString = cString::sprintf("%s %d%% - %2d:%02d %s", tr("Disk"), Percent, Hours, Minutes, tr("free"));
        lastFreeMB = FreeMB;
        return true;
        }
     }
  return false;
}

#if VDRVERSNUM >= 10515
int cFreeDiskSpace::VideoDiskSpace(const char *Dir, int *FreeMB)
{
  int used = 0;
  int free = FreeDiskSpaceMB(Dir, &used);
  int deleted = DeletedFileSizeMB(Dir);
  if (deleted > used)
     deleted = used;
  free += deleted;
  used -= deleted;
  if (FreeMB)
     *FreeMB = free;
  return (free + used) ? used * 100 / (free + used) : 0;
}

int cFreeDiskSpace::DeletedFileSizeMB(const char *Dir)
{
  int size = 0;
  cThreadLock DeletedRecordingsLock(&DeletedRecordings);
  for (cRecording *recording = DeletedRecordings.First(); recording; recording = DeletedRecordings.Next(recording)) {
      int fileSizeMB = DirSizeMB(recording->FileName());
      if (fileSizeMB > 0 && EntriesOnSameFileSystem(Dir, recording->FileName()))
         size += fileSizeMB;
      }
  return size;
}
#endif
#endif //REMOTETIMERS_DISKSPACE

#if APIVERSNUM < 10712

// --- cNestedItem -----------------------------------------------------------

// copy of VDR's cNestedItem / cNestedItemList

cNestedItem::cNestedItem(const char *Text, bool WithSubItems)
{
  text = strdup(Text ? Text : "");
  subItems = WithSubItems ? new cList<cNestedItem> : NULL;
}

cNestedItem::~cNestedItem()
{
  delete subItems;
  free(text);
}

int cNestedItem::Compare(const cListObject &ListObject) const
{
  return strcasecmp(text, ((cNestedItem *)&ListObject)->text);
}

void cNestedItem::AddSubItem(cNestedItem *Item)
{
  if (!subItems)
     subItems = new cList<cNestedItem>;
  if (Item)
     subItems->Add(Item);
}

void cNestedItem::SetText(const char *Text)
{
  free(text);
  text = strdup(Text ? Text : "");
}

void cNestedItem::SetSubItems(bool On)
{
  if (On && !subItems)
     subItems = new cList<cNestedItem>;
  else if (!On && subItems) {
     delete subItems;
     subItems = NULL;
     }
}

// --- cNestedItemList -------------------------------------------------------

cNestedItemList::cNestedItemList(void)
{
  fileName = NULL;
}

cNestedItemList::~cNestedItemList()
{
  free(fileName);
}

bool cNestedItemList::Parse(FILE *f, cList<cNestedItem> *List, int &Line)
{
  char *s;
  cReadLine ReadLine;
  while ((s = ReadLine.Read(f)) != NULL) {
        Line++;
        char *p = strchr(s, '#');
        if (p)
           *p = 0;
        s = skipspace(stripspace(s));
        if (!isempty(s)) {
           p = s + strlen(s) - 1;
           if (*p == '{') {
              *p = 0;
              stripspace(s);
              cNestedItem *Item = new cNestedItem(s, true);
              List->Add(Item);
              if (!Parse(f, Item->SubItems(), Line))
                 return false;
              }
           else if (*s == '}')
              break;
           else
              List->Add(new cNestedItem(s));
           }
        }
  return true;
}

bool cNestedItemList::Write(FILE *f, cList<cNestedItem> *List, int Indent)
{
  for (cNestedItem *Item = List->First(); Item; Item = List->Next(Item)) {
      if (Item->SubItems()) {
         fprintf(f, "%*s%s {\n", Indent, "", Item->Text());
         Write(f, Item->SubItems(), Indent + 2);
         fprintf(f, "%*s}\n", Indent + 2, "");
         }
      else
         fprintf(f, "%*s%s\n", Indent, "", Item->Text());
      }
  return true;
}

void cNestedItemList::Clear(void)
{
  free(fileName);
  fileName = NULL;
  cList<cNestedItem>::Clear();
}

bool cNestedItemList::Load(const char *FileName)
{
  cList<cNestedItem>::Clear();
  if (FileName) {
     free(fileName);
     fileName = strdup(FileName);
     }
  bool result = false;
  if (fileName && access(fileName, F_OK) == 0) {
     isyslog("loading %s", fileName);
     FILE *f = fopen(fileName, "r");
     if (f) {
        int Line = 0;
        result = Parse(f, this, Line);
        fclose(f);
        }
     else {
        LOG_ERROR_STR(fileName);
        result = false;
        }
     }
  return result;
}

bool cNestedItemList::Save(void)
{
  bool result = true;
  cSafeFile f(fileName);
  if (f.Open()) {
     result = Write(f, this);
     if (!f.Close())
        result = false;
     }
  else
     result = false;
  return result;
}

cNestedItemList Folders;

// --- cMenuFolderItem -------------------------------------------------------

class cMenuFolderItem : public cOsdItem {
private:
  cNestedItem *folder;
public:
  cMenuFolderItem(cNestedItem *Folder);
  cNestedItem *Folder(void) { return folder; }
  };

cMenuFolderItem::cMenuFolderItem(cNestedItem *Folder)
:cOsdItem(Folder->Text())
{
  folder = Folder;
  if (folder->SubItems())
     SetText(cString::sprintf("%s...", folder->Text()));
}

// --- cMenuEditFolder -------------------------------------------------------

class cMenuEditFolder : public cOsdMenu {
private:
  cList<cNestedItem> *list;
  cNestedItem *folder;
  char name[PATH_MAX];
  int subFolder;
  eOSState Confirm(void);
public:
  cMenuEditFolder(const char *Dir, cList<cNestedItem> *List, cNestedItem *Folder = NULL);
  cString GetFolder(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuEditFolder::cMenuEditFolder(const char *Dir, cList<cNestedItem> *List, cNestedItem *Folder)
:cOsdMenu(Folder ? trREMOTETIMERS("Edit folder") : trREMOTETIMERS("New folder"), 12)
{
  list = List;
  folder = Folder;
  if (folder) {
     strn0cpy(name, folder->Text(), sizeof(name));
     subFolder = folder->SubItems() != NULL;
     }
  else {
     *name = 0;
     subFolder = 0;
     cRemote::Put(kRight, true); // go right into string editing mode
     }
  if (!isempty(Dir)) {
     cOsdItem *DirItem = new cOsdItem(Dir);
     DirItem->SetSelectable(false);
     Add(DirItem);
     }
  Add(new cMenuEditStrItem( tr("Name"), name, sizeof(name), tr(FileNameChars)));
  Add(new cMenuEditBoolItem(trREMOTETIMERS("Sub folder"), &subFolder));
}

cString cMenuEditFolder::GetFolder(void)
{
  return folder ? folder->Text() : "";
}

eOSState cMenuEditFolder::Confirm(void)
{
  if (!folder || strcmp(folder->Text(), name) != 0) {
     // each name may occur only once in a folder list
     for (cNestedItem *Folder = list->First(); Folder; Folder = list->Next(Folder)) {
         if (strcmp(Folder->Text(), name) == 0) {
            Skins.Message(mtError, trREMOTETIMERS("Folder name already exists!"));
            return osContinue;
            }
         }
     char *p = strpbrk(name, "\\{}#~"); // FOLDERDELIMCHAR
     if (p) {
        Skins.Message(mtError, cString::sprintf(trREMOTETIMERS("Folder name must not contain '%c'!"), *p));
        return osContinue;
        }
     }
  if (folder) {
     folder->SetText(name);
     folder->SetSubItems(subFolder);
     }
  else
     list->Add(folder = new cNestedItem(name, subFolder));
  return osEnd;
}

eOSState cMenuEditFolder::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kOk:     return Confirm();
       case kRed:
       case kGreen:
       case kYellow:
       case kBlue:   return osContinue;
       default: break;
       }
     }
  return state;
}

// --- cMenuFolder -----------------------------------------------------------

cMenuFolder::cMenuFolder(const char *Title, cNestedItemList *NestedItemList, const char *Path)
:cOsdMenu(Title)
{
  // cOsdMenu::Title() and cOsdMenu::SubMenu() missing in < 10712
  title = Title;
  subMenuFolder = NULL;
  subMenuEditFolder = NULL;

  list = nestedItemList = NestedItemList;
  firstFolder = NULL;
  editing = false;
  Set();
  SetHelpKeys();
  DescendPath(Path);
}

cMenuFolder::cMenuFolder(const char *Title, cList<cNestedItem> *List, cNestedItemList *NestedItemList, const char *Dir, const char *Path)
:cOsdMenu(Title)
{
  // cOsdMenu::Title() and cOsdMenu::SubMenu() missing in < 10712
  title = Title;
  subMenuFolder = NULL;
  subMenuEditFolder = NULL;

  list = List;
  nestedItemList = NestedItemList;
  dir = Dir;
  firstFolder = NULL;
  editing = false;
  Set();
  SetHelpKeys();
  DescendPath(Path);
}

void cMenuFolder::SetHelpKeys(void)
{
  SetHelp(firstFolder ? trREMOTETIMERS("Button$Select") : NULL, tr("Button$New"), firstFolder ? tr("Button$Delete") : NULL, firstFolder ? tr("Button$Edit") : NULL);
}

void cMenuFolder::Set(const char *CurrentFolder)
{
  firstFolder = NULL;
  Clear();
  if (!isempty(dir)) {
     cOsdItem *DirItem = new cOsdItem(dir);
     DirItem->SetSelectable(false);
     Add(DirItem);
     }
  list->Sort();
  for (cNestedItem *Folder = list->First(); Folder; Folder = list->Next(Folder)) {
      cOsdItem *FolderItem = new cMenuFolderItem(Folder);
      Add(FolderItem, CurrentFolder ? strcmp(Folder->Text(), CurrentFolder) == 0 : false);
      if (!firstFolder)
         firstFolder = FolderItem;
      }
}

void cMenuFolder::DescendPath(const char *Path)
{
  if (Path) {
     const char *p = strchr(Path, FOLDERDELIMCHAR);
     if (p) {
        for (cMenuFolderItem *Folder = (cMenuFolderItem *)firstFolder; Folder; Folder = (cMenuFolderItem *)Next(Folder)) {
            if (strncmp(Folder->Folder()->Text(), Path, p - Path) == 0) {
               SetCurrent(Folder);
               if (Folder->Folder()->SubItems())
                  AddSubMenu(subMenuFolder = new cMenuFolder(title, Folder->Folder()->SubItems(), nestedItemList, !isempty(dir) ? *cString::sprintf("%s%c%s", *dir, FOLDERDELIMCHAR, Folder->Folder()->Text()) : Folder->Folder()->Text(), p + 1));
               break;
               }
            }
        }
    }
}

eOSState cMenuFolder::Select(void)
{
  if (firstFolder) {
     cMenuFolderItem *Folder = (cMenuFolderItem *)Get(Current());
     if (Folder) {
        if (Folder->Folder()->SubItems())
           return AddSubMenu(subMenuFolder = new cMenuFolder(title, Folder->Folder()->SubItems(), nestedItemList, !isempty(dir) ? *cString::sprintf("%s%c%s", *dir, FOLDERDELIMCHAR, Folder->Folder()->Text()) : Folder->Folder()->Text()));
        else
           return osEnd;
        }
     }
  return osContinue;
}

eOSState cMenuFolder::New(void)
{
  editing = true;
  return AddSubMenu(subMenuEditFolder = new cMenuEditFolder(dir, list));
}

eOSState cMenuFolder::Delete(void)
{
  if (!HasSubMenu() && firstFolder) {
     cMenuFolderItem *Folder = (cMenuFolderItem *)Get(Current());
     if (Folder && Interface->Confirm(Folder->Folder()->SubItems() ? trREMOTETIMERS("Delete folder and all sub folders?") : trREMOTETIMERS("Delete folder?"))) {
        list->Del(Folder->Folder());
        Del(Folder->Index());
        firstFolder = Get(isempty(dir) ? 0 : 1);
        Display();
        SetHelpKeys();
        nestedItemList->Save();
        }
     }
  return osContinue;
}

eOSState cMenuFolder::Edit(void)
{
  if (!HasSubMenu() && firstFolder) {
     cMenuFolderItem *Folder = (cMenuFolderItem *)Get(Current());
     if (Folder) {
        editing = true;
        return AddSubMenu(subMenuEditFolder = new cMenuEditFolder(dir, list, Folder->Folder()));
        }
     }
  return osContinue;
}

eOSState cMenuFolder::SetFolder(void)
{
  cMenuEditFolder *mef = subMenuEditFolder;
  if (mef) {
     Set(mef->GetFolder());
     SetHelpKeys();
     Display();
     nestedItemList->Save();
     subMenuEditFolder = NULL;
     }
  return CloseSubMenu();
}

cString cMenuFolder::GetFolder(void)
{
  if (firstFolder) {
     cMenuFolderItem *Folder = (cMenuFolderItem *)Get(Current());
     if (Folder) {
        cMenuFolder *mf = subMenuFolder;
        if (mf)
           return cString::sprintf("%s%c%s", Folder->Folder()->Text(), FOLDERDELIMCHAR, *mf->GetFolder());
        return Folder->Folder()->Text();
        }
     }
  return "";
}

eOSState cMenuFolder::ProcessKey(eKeys Key)
{
  if (!HasSubMenu())
     editing = false;
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kOk:
       case kRed:    return Select();
       case kGreen:  return New();
       case kYellow: return Delete();
       case kBlue:   return Edit();
       default:      state = osContinue;
       }
     }
  else if (state == osEnd && HasSubMenu() && editing)
     state = SetFolder();
  return state;
}

#endif

// --- cMenuEditRemoteTimer --------------------------------------------------------
class cMenuEditRemoteTimer : public cMenuEditTimer {
private:
  cTimer *timer;
  cMenuTimerItem **timerItem;
/*
  cTimer data;
  int channel;
  bool addIfConfirmed;
  cMenuEditDateItem *firstday;
  void SetFirstDayItem(void);
*/
  int remote;
  int tmpRemote;
  int user;
  int tmpUser;
#if APIVERSNUM < 10712
  cMenuFolder *subMenuFolder;
  char name[MaxFileName];
  cMenuEditStrItem *file;
  eOSState SetFolder(void);
  void SetHelpKeys(void);
#endif
  eOSState CheckState(eRemoteTimersState State);
public:
/*
  cMenuEditTimer(cTimer *Timer, bool New = false);
  virtual ~cMenuEditTimer();
*/
  virtual eOSState ProcessKey(eKeys Key);
  cMenuEditRemoteTimer(cTimer *Timer, bool Remote, bool New = false, cMenuTimerItem **TimerItem = NULL);
  };

cMenuEditRemoteTimer::cMenuEditRemoteTimer(cTimer *Timer, bool Remote, bool New, cMenuTimerItem **TimerItem)
:cMenuEditTimer(Timer, New), timer(Timer), timerItem(TimerItem)
{
  remote = tmpRemote = Remote;
  user = tmpUser = New ? MASK_FROM_SETUP(RemoteTimersSetup.defaultUser) : cMenuTimerItem::ParseUser(timer);
#if APIVERSNUM < 10712
  subMenuFolder = NULL;
  file = NULL;
  strn0cpy(name, Timer->File(), sizeof(name));
  SetHelpKeys();
  // replace "File" edit by our own implementation
  Add(file = new cMenuEditStrItem( tr("File"), name, sizeof(name), tr(FileNameChars)), Get(8));
  Del(8);
#endif
  cOsdItem *item = new cMenuEditBoolItem(trREMOTETIMERS("Location"), &tmpRemote, trREMOTETIMERS("local"), trREMOTETIMERS("remote"));
  if (cSvdrp::GetInstance()->Offline())
      item->SetSelectable(false);
  Add(item, false, First());
  Add(new cMenuEditUserItem(trREMOTETIMERS("User ID"), &tmpUser), false, Get(8));
}

eOSState cMenuEditRemoteTimer::CheckState(eRemoteTimersState State)
{
  if (State > rtsRefresh) {
     Skins.Message(State == rtsLocked ? mtWarning : mtError, tr(RemoteTimers.GetErrorMessage(State)));
     return osContinue;
     }
  return osBack;
}

#if APIVERSNUM < 10712

void cMenuEditRemoteTimer::SetHelpKeys(void)
{
  SetHelp(trREMOTETIMERS("Button$Folder"));
}

eOSState cMenuEditRemoteTimer::SetFolder(void)
{
  cMenuFolder *mf = subMenuFolder;
  if (mf) {
     cString Folder = mf->GetFolder();
     char *p = strrchr(name, FOLDERDELIMCHAR);
     if (p)
        p++;
     else
        p = name;
     if (!isempty(*Folder))
        strn0cpy(name, cString::sprintf("%s%c%s", *Folder, FOLDERDELIMCHAR, p), sizeof(name));
     else if (p != name)
        memmove(name, p, strlen(p) + 1);
     SetCurrent(file);
     Display();
     subMenuFolder = NULL;
     }
  return CloseSubMenu();
}

#endif

eOSState cMenuEditRemoteTimer::ProcessKey(eKeys Key)
{
  int TimerNumber = Timers.Count();
  eOSState state = cMenuEditTimer::ProcessKey(Key);
  if (state == osBack && Key == kOk) {
     // changes have been confirmed
#if APIVERSNUM < 10712
     timer->SetFile(name);
#endif
     if (user != tmpUser)
        cMenuTimerItem::UpdateUser(*timer, tmpUser);
     if (TimerNumber == Timers.Count()) {
        // editing existing timer (remote timers are also added to Timers, first)
        if (remote == tmpRemote) {
           // timer was not moved
           if (tmpRemote)
              state = CheckState(RemoteTimers.Modify((cRemoteTimer*) timer));
           }
        else if (tmpRemote) {
           // move local to remote
           if (timer->Recording()) {
              Skins.Message(mtError, trREMOTETIMERS("Timer is recording - can't move it to server"));
              state = osContinue;
              }
           else {
              cRemoteTimer *rt = new cRemoteTimer();
              *(cTimer*) rt = *timer;
              if ((state = CheckState(RemoteTimers.New(rt))) == osBack) {
                 Timers.Del(timer);
                 timer = rt;
                 }
              else
                 delete rt;
              }
           }
        else {
           // move remote to local
           cTimer *lt = new cTimer();
#if VDRVERSNUM < 10403
           if (lt->Parse(timer->ToText())) {
#else
           *lt = *(cTimer*) timer;
#endif
           if ((state = CheckState(RemoteTimers.Delete((cRemoteTimer*) timer))) == osBack) {
              Timers.Add(lt);
              timer = lt;
              }
           else
              delete lt;
#if VDRVERSNUM < 10403
           }
           else
              delete lt;
#endif
           }
        if (timerItem && state == osBack)
           (*timerItem)->Update(timer, tmpUser, tmpRemote);
        }
     else {
        // local timer has been added
        if (tmpRemote) {
           // move to remote
           cRemoteTimer *rt = new cRemoteTimer();
           *(cTimer*) rt = *timer;
           if ((state = CheckState(RemoteTimers.New(rt))) == osBack) {
              Timers.Del(timer);
              timer = rt;
              }
           else
              delete rt;
           }
        if (timerItem && state == osBack)
           *timerItem = new cMenuTimerItem(timer, tmpUser, tmpRemote);
        }
     }
#if APIVERSNUM < 10712
  else if (state == osContinue && Key == kRed && !HasSubMenu())
    return AddSubMenu(subMenuFolder = new cMenuFolder(trREMOTETIMERS("Select folder"), &Folders, name));
  else if (state == osEnd && HasSubMenu())
     state = SetFolder();
#endif
  return state;
}

// --- cMenuTimerItem --------------------------------------------------------

/*
class cMenuTimerItem : public cOsdItem {
private:
  cTimer *timer;
public:
  cMenuTimerItem(cTimer *Timer);
  virtual int Compare(const cListObject &ListObject) const;
  virtual void Set(void);
  cTimer *Timer(void) { return timer; }
  };
*/

cMenuTimerItem::cMenuTimerItem(cTimer *Timer, int User, bool Remote)
{
  Update(Timer, User, Remote);
  Set();
}

void cMenuTimerItem::Update(cTimer *Timer, int User, bool Remote)
{
  timer = Timer;
  user = User;
  remote = Remote;
}

int cMenuTimerItem::Compare(const cListObject &ListObject) const
{
  return timer->Compare(*((cMenuTimerItem *)&ListObject)->timer);
}

void cMenuTimerItem::Set(void)
{
  cString day, name("");
  if (timer->WeekDays())
#if VDRVERSNUM < 10503
     day = timer->PrintDay(0, timer->WeekDays());
#else
     day = timer->PrintDay(0, timer->WeekDays(), false);
#endif
  else if (timer->Day() - time(NULL) < 28 * SECSINDAY) {
     day = itoa(timer->GetMDay(timer->Day()));
     name = WeekDayName(timer->Day());
     }
  else {
     struct tm tm_r;
     time_t Day = timer->Day();
     localtime_r(&Day, &tm_r);
     char buffer[16];
     strftime(buffer, sizeof(buffer), "%Y%m%d", &tm_r);
     day = buffer;
     }
#if VDRVERSNUM >= 10714
  const char *File = Setup.FoldersInTimerMenu ? NULL : strrchr(timer->File(), FOLDERDELIMCHAR);
  if (File && strcmp(File + 1, TIMERMACRO_TITLE) && strcmp(File + 1, TIMERMACRO_EPISODE))
     File++;
  else
     File = timer->File();
#endif
  // TRANSLATORS: Indicator for (R)emote or (L)ocal timer in timers list
  const char *RL = trREMOTETIMERS("RL");
  SetText(cString::sprintf("%c\t%c%d\t%s%s%s\t%02d:%02d\t%02d:%02d\t%s",
                    !(timer->HasFlags(tfActive)) ? ' ' : timer->FirstDay() ? '!' : timer->Recording() ? '#' : '>',
                    RL[remote ? 0 : 1],
                    timer->Channel()->Number(),
                    *name,
                    *name && **name ? " " : "",
                    *day,
                    timer->Start() / 100,
                    timer->Start() % 100,
                    timer->Stop() / 100,
                    timer->Stop() % 100,
#if VDRVERSNUM >= 10714
                    File));
#else
                    timer->File()));
#endif
}

int cMenuTimerItem::ParseUser(const cTimer *Timer) {
  return PluginRemoteTimers::ParseUser(Timer->Aux());
}

void cMenuTimerItem::UpdateUser(cTimer& Timer, int User) {
  cString origTimer = Timer.ToText();
  cString modTimer(PluginRemoteTimers::UpdateUser(*origTimer, User));
  Timer.Parse(*modTimer);
}

// --- cMenuTimers -----------------------------------------------------------

/*
class cMenuTimers : public cOsdMenu {
private:
  int helpKeys;
  eOSState Edit(void);
  eOSState New(void);
  eOSState Delete(void);
  eOSState OnOff(void);
  eOSState Info(void);
  cTimer *CurrentTimer(void);
  void SetHelpKeys(void);
public:
  cMenuTimers(void);
  virtual ~cMenuTimers();
  virtual eOSState ProcessKey(eKeys Key);
  };
*/

cMenuTimers::cMenuTimers(void)
:cOsdMenu(tr("Timers"), 2, CHNUMWIDTH + 1, 10, 6, 6)
{
#if VDRVERSNUM >= 10728
  SetMenuCategory(mcTimer);
#endif
  helpKeys = -1;
  userFilter = USER_FROM_SETUP(RemoteTimersSetup.userFilterTimers);
  currentItem = NULL;
  if (!cSvdrp::GetInstance()->Connect())
      Skins.Message(mtWarning, tr(MSG_UNAVAILABLE));
  Set();
  /*
  for (cTimer *timer = Timers.First(); timer; timer = Timers.Next(timer)) {
      timer->SetEventFromSchedule(); // make sure the event is current
      Add(new cMenuTimerItem(timer));
      }
  Sort();
  SetCurrent(First());
  SetHelpKeys();
  */
  Timers.IncBeingEdited();
}

cMenuTimers::~cMenuTimers()
{
  Timers.DecBeingEdited();
  cSvdrp::GetInstance()->Disconnect();
}

void cMenuTimers::Set(eRemoteTimersState Msg)
{
  cString currentTimerString;
  int currentIndex = Current();
  if (currentIndex >= 0)
     currentTimerString = ((cMenuTimerItem *) Get(currentIndex))->Timer()->ToText();
  Clear();
  eRemoteTimersState state = cSvdrp::GetInstance()->Offline() ? rtsOk : RemoteTimers.Refresh();
  if (state == rtsOk) {
      for (cRemoteTimer *timer = RemoteTimers.First(); timer; timer = RemoteTimers.Next(timer)) {
          int user = cMenuTimerItem::ParseUser(timer);
          if (userFilter == 0 || user == 0 || (user & USER_MASK(userFilter))) {
              timer->SetEventFromSchedule(); // make sure the event is current
              Add(new cMenuTimerItem(timer, user, true));
              }
          }
      }
  for (cTimer *timer = Timers.First(); timer; timer = Timers.Next(timer)) {
      int user = cMenuTimerItem::ParseUser(timer);
      if (userFilter == 0 || user == 0 || (user & USER_MASK(userFilter))) {
          timer->SetEventFromSchedule(); // make sure the event is current
          Add(new cMenuTimerItem(timer, user, false));
          }
      }
  Sort();

  cOsdItem *currentItem = NULL;
  // if we have been editing a timer make it current
  if (*currentTimerString) {
     for (cOsdItem *item = First(); item && !currentItem; item = Next(item)) {
        if (strcmp(*currentTimerString, ((cMenuTimerItem *)item)->Timer()->ToText()) == 0)
           currentItem = item;
        }
     }
  // make the previous position current
  if (!currentItem)
     currentItem = Get(currentIndex < Count() ? currentIndex : Count() - 1);
  // use first
  if (!currentItem)
     currentItem = First();

  SetCurrent(currentItem);
  SetHelpKeys();
  if (userFilter == 0)
     SetTitle(tr("Timers"));
  else
     SetTitle(cString::sprintf("%s - %s %d", tr("Timers"), trREMOTETIMERS("User"), userFilter));

  if (Msg > state)
     state = Msg;
  if (state != rtsOk)
     Skins.Message(state == rtsRefresh ? mtInfo : state == rtsLocked ? mtWarning : mtError, tr(RemoteTimers.GetErrorMessage(state)));
}

void cMenuTimers::CheckState(eRemoteTimersState State, bool RefreshMsg)
{
  if (State == rtsRefresh && !RefreshMsg)
     Set(rtsOk);
  else if (State != rtsOk)
     Set(State);
}

cMenuTimerItem *cMenuTimers::CurrentItem(void)
{
  return (cMenuTimerItem *)Get(Current());
}

cTimer *cMenuTimers::CurrentTimer(void)
{
  cMenuTimerItem *item = (cMenuTimerItem *)Get(Current());
  return item ? item->Timer() : NULL;
}

void cMenuTimers::SetHelpKeys(void)
{
  int NewHelpKeys = 0;
  cTimer *timer = CurrentTimer();
  if (timer) {
     if (timer->Event())
        NewHelpKeys = 2;
     else
        NewHelpKeys = 1;
     cMenuTimerItem *item = CurrentItem();
     if (userFilter != 0 && item->User() != 0 && item->User() != USER_MASK(userFilter))
        NewHelpKeys += 2;
     }
  if (NewHelpKeys != helpKeys) {
     helpKeys = NewHelpKeys;
     // TRANSLATORS: Button displayed instead of "Delete" when there are other users who subscribed to the timer
     SetHelp(helpKeys > 0 ? tr("Button$On/Off") : NULL, tr("Button$New"), helpKeys > 0 ? (helpKeys > 2 ? trREMOTETIMERS("Cancel") : tr("Button$Delete")) : NULL, helpKeys == 2 ? tr("Button$Info") : NULL);
     }
}

eOSState cMenuTimers::OnOff(void)
{
  if (HasSubMenu())
     return osContinue;
  cMenuTimerItem *item = CurrentItem();
  cTimer *timer = CurrentTimer();
  if (timer) {
     timer->OnOff();
     timer->SetEventFromSchedule();
     if (item->Remote())
        CheckState(RemoteTimers.Modify((cRemoteTimer*)timer));
     RefreshCurrent();
     DisplayCurrent(true);
     if (timer->FirstDay())
        isyslog("timer %s first day set to %s", *timer->ToDescr(), *timer->PrintFirstDay());
     else
        isyslog("timer %s %sactivated", *timer->ToDescr(), timer->HasFlags(tfActive) ? "" : "de");
     if (!item->Remote())
        Timers.SetModified();
     }
  return osContinue;
}

eOSState cMenuTimers::Edit(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  isyslog("editing timer %s", *CurrentTimer()->ToDescr());
  currentItem = CurrentItem();
  return AddSubMenu(new cMenuEditRemoteTimer(CurrentTimer(), CurrentItem()->Remote(), false, &currentItem));
}

eOSState cMenuTimers::New(void)
{
  if (HasSubMenu())
     return osContinue;
  currentItem = NULL;
  return AddSubMenu(new cMenuEditRemoteTimer(new cTimer, cSvdrp::GetInstance()->Offline() ? false : RemoteTimersSetup.addToRemote, true, &currentItem));
}

eOSState cMenuTimers::Delete(void)
{
  // Check if this timer is active:
  cMenuTimerItem *item = CurrentItem();
  cTimer *ti = CurrentTimer();
  if (ti) {
     if (userFilter != 0 && item->User() != 0 && item->User() != USER_MASK(userFilter)) {
	cMenuTimerItem::UpdateUser(*ti, item->User() ^ USER_MASK(userFilter));
        if (item->Remote())
           CheckState(RemoteTimers.Modify((cRemoteTimer*)ti), false);
        else
           Timers.SetModified();
        cOsdMenu::Del(Current());
        Display();
        }
     else if (Interface->Confirm(tr("Delete timer?"))) {
        if (ti->Recording() && !item->Remote()) {
           if (Interface->Confirm(tr("Timer still recording - really delete?"))) {
              ti->Skip();
              cRecordControls::Process(time(NULL));
              }
           else
              return osContinue;
           }
        isyslog("deleting timer %s", *ti->ToDescr());
        if (item->Remote())
           CheckState(RemoteTimers.Delete((cRemoteTimer*)ti), false);
        else {
           Timers.Del(ti);
           cOsdMenu::Del(Current());
           Timers.SetModified();
           }
        Display();
        }
     }
  return osContinue;
}

eOSState cMenuTimers::Info(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cTimer *ti = CurrentTimer();
  if (ti && ti->Event())
     return AddSubMenu(new cMenuEvent(ti->Event()));
  return osContinue;
}

eOSState cMenuTimers::ProcessKey(eKeys Key)
{
  //int TimerNumber = HasSubMenu() ? Count() : -1;
  // cMenuTimers::New() sets currentItem to NULL
  int TimerNumber = HasSubMenu() && !currentItem ? Count() : -1;
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kOk:     return Edit();
       case kRed:    state = OnOff(); break; // must go through SetHelpKeys()!
       case kGreen:  return New();
       case kYellow: state = Delete(); break;
       case kInfo:
       case kBlue:   return Info();
                     break;
       case k0 ... k9:
                     userFilter = Key - k0;
		     if (RemoteTimersSetup.userFilterTimers < 0 && ::Setup.ResumeID != userFilter) {
		        ::Setup.ResumeID = userFilter;
			Recordings.ResetResume();
			}
                     Set();
                     Display();
                     return osContinue;
       default: break;
       }
     }
  //if (TimerNumber >= 0 && !HasSubMenu() && Timers.Get(TimerNumber)) {
  if (TimerNumber >= 0 && !HasSubMenu() && currentItem) {
     // a newly created timer was confirmed with Ok
     Add(currentItem, true);
     //Add(new cMenuTimerItem(Timers.Get(TimerNumber)), true);
     Set();
     Display();
     }
  if (Key != kNone)
     SetHelpKeys();
  return state;
}

// --- cMenuEvent ------------------------------------------------------------

cMenuEvent::cMenuEvent(const cEvent *Event, bool CanSwitch, bool Buttons)
:cOsdMenu(tr("Event"))
{
#if VDRVERSNUM >= 10728
  SetMenuCategory(mcEvent);
#endif
  event = Event;
  if (event) {
     cChannel *channel = Channels.GetByChannelID(event->ChannelID(), true);
     if (channel) {
        SetTitle(channel->Name());
        int TimerMatch = tmNone;
        //Timers.GetMatch(event, &TimerMatch);
	GetBestMatch(event, MASK_FROM_SETUP(RemoteTimersSetup.userFilterSchedule), &TimerMatch, NULL, NULL);
        if (Buttons)
           SetHelp(TimerMatch == tmFull ? tr("Button$Timer") : tr("Button$Record"), NULL, NULL, CanSwitch ? tr("Button$Switch") : NULL);
        }
     }
}

void cMenuEvent::Display(void)
{
  cOsdMenu::Display();
  DisplayMenu()->SetEvent(event);
  if (event->Description())
     cStatus::MsgOsdTextItem(event->Description());
}

eOSState cMenuEvent::ProcessKey(eKeys Key)
{
  switch (int(Key)) {
    case kUp|k_Repeat:
    case kUp:
    case kDown|k_Repeat:
    case kDown:
    case kLeft|k_Repeat:
    case kLeft:
    case kRight|k_Repeat:
    case kRight:
                  DisplayMenu()->Scroll(NORMALKEY(Key) == kUp || NORMALKEY(Key) == kLeft, NORMALKEY(Key) == kLeft || NORMALKEY(Key) == kRight);
                  cStatus::MsgOsdTextItem(NULL, NORMALKEY(Key) == kUp || NORMALKEY(Key) == kLeft);
                  return osContinue;
    case kInfo:   return osBack;
    default: break;
    }

  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kGreen:
       case kYellow: return osContinue;
       case kOk:     return osBack;
       default: break;
       }
     }
  return state;
}

// --- cMenuScheduleItem -----------------------------------------------------

class cMenuScheduleItem : public cOsdItem {
public:
  enum eScheduleSortMode { ssmAllThis, ssmThisThis, ssmThisAll, ssmAllAll }; // "which event(s) on which channel(s)"
private:
  static eScheduleSortMode sortMode;
public:
  const cEvent *event;
  const cChannel *channel;
  bool withDate;
  bool withBar;
  int timerMatch;
  int timerType;
  cMenuScheduleItem(const cEvent *Event, cChannel *Channel = NULL, bool WithDate = false, bool WithBar = false);
  static void SetSortMode(eScheduleSortMode SortMode) { sortMode = SortMode; }
  static void IncSortMode(void) { sortMode = eScheduleSortMode((sortMode == ssmAllAll) ? ssmAllThis : sortMode + 1); }
  static eScheduleSortMode SortMode(void) { return sortMode; }
  virtual int Compare(const cListObject &ListObject) const;
  bool Update(bool Force = false);
  };

cMenuScheduleItem::eScheduleSortMode cMenuScheduleItem::sortMode = ssmAllThis;

cMenuScheduleItem::cMenuScheduleItem(const cEvent *Event, cChannel *Channel, bool WithDate, bool WithBar)
{
  event = Event;
  channel = Channel;
  withDate = WithDate;
  withBar = WithBar;
  timerMatch = tmNone;
  timerType = 0;
  Update(true);
}

int cMenuScheduleItem::Compare(const cListObject &ListObject) const
{
  cMenuScheduleItem *p = (cMenuScheduleItem *)&ListObject;
  int r = -1;
  if (sortMode != ssmAllThis)
     r = strcoll(event->Title(), p->event->Title());
  if (sortMode == ssmAllThis || r == 0)
     r = event->StartTime() - p->event->StartTime();
  return r;
}

static const char * const ProgressBar[] =
{
  "[      ]",
  "[|     ]",
  "[||    ]",
  "[|||   ]",
  "[||||  ]",
  "[||||| ]",
  "[||||||]"
};
#define MAXPROGRESS ((int) (sizeof(ProgressBar) / sizeof(char *)))

//static const char *TimerMatchChars = " tT";
// TRANSLATORS: Schedule timer match characters (lower case for partial match): no match (blank), normal (tT)imer, (pP)rivate timer
static const char *TimerMatchChars = trREMOTETIMERS(" tTpP");

bool cMenuScheduleItem::Update(bool Force)
{
  bool result = false;
  int oldTimerMatch = timerMatch;
  //Timers.GetMatch(event, &timerMatch);
  int oldTimerType = timerType;
  GetBestMatch(event, MASK_FROM_SETUP(RemoteTimersSetup.userFilterSchedule), &timerMatch, &timerType, NULL);
  if (Force || timerMatch != oldTimerMatch || timerType != oldTimerType) {
     cString buffer;
     char t = TimerMatchChars[timerMatch + 2 * timerType];
     char v = event->Vps() && (event->Vps() - event->StartTime()) ? 'V' : ' ';
     char r = event->SeenWithin(30) && event->IsRunning() ? '*' : ' ';
     const char *csn = channel ? channel->ShortName(true) : NULL;
     cString eds = event->GetDateString();
#if VDRVERSNUM < 10504
#define Utf8SymChars(a,b) b
#endif
#if VDRVERSNUM < 10728
#define CSN_SYMBOLS 6
#else
#define CSN_SYMBOLS 999
#endif
     if (channel && withDate)
        buffer = cString::sprintf("%d\t%.*s\t%.*s\t%s\t%c%c%c\t%s", channel->Number(), Utf8SymChars(csn, CSN_SYMBOLS), csn, Utf8SymChars(eds, 6), *eds, *event->GetTimeString(), t, v, r, event->Title());
     else if (channel) {
	if (withBar && RemoteTimersSetup.showProgressBar) {
	   int progress = (time(NULL) - event->StartTime()) * MAXPROGRESS / event->Duration();
	   progress = progress < 0 ? 0 : progress >= MAXPROGRESS ? MAXPROGRESS - 1 : progress;
           buffer = cString::sprintf("%d\t%.*s\t%s\t%s\t%c%c%c\t%s", channel->Number(), Utf8SymChars(csn, CSN_SYMBOLS), csn, *event->GetTimeString(), ProgressBar[progress], t, v, r, event->Title());
	   }
	else
           buffer = cString::sprintf("%d\t%.*s\t%s\t%c%c%c\t%s", channel->Number(), Utf8SymChars(csn, 6), csn, *event->GetTimeString(), t, v, r, event->Title());
	}
     else
        buffer = cString::sprintf("%.*s\t%s\t%c%c%c\t%s", Utf8SymChars(eds, 6), *eds, *event->GetTimeString(), t, v, r, event->Title());
     SetText(buffer);
     result = true;
     }
  return result;
}

// --- cMenuWhatsOn ----------------------------------------------------------

class cMenuWhatsOn : public cOsdMenu {
private:
  bool now;
  int helpKeys;
  int timerState;
  eOSState Record(void);
  eOSState Switch(void);
  static int currentChannel;
  static const cEvent *scheduleEvent;
  bool Update(void);
  void SetHelpKeys(void);
public:
  cMenuWhatsOn(const cSchedules *Schedules, bool Now, int CurrentChannelNr);
  static int CurrentChannel(void) { return currentChannel; }
  static void SetCurrentChannel(int ChannelNr) { currentChannel = ChannelNr; }
  static const cEvent *ScheduleEvent(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

int cMenuWhatsOn::currentChannel = 0;
const cEvent *cMenuWhatsOn::scheduleEvent = NULL;

cMenuWhatsOn::cMenuWhatsOn(const cSchedules *Schedules, bool Now, int CurrentChannelNr)
:cOsdMenu(Now ? tr("What's on now?") : tr("What's on next?"), CHNUMWIDTH, CHNAMWIDTH, 6, 4, 4)
{
#if VDRVERSNUM >= 10728
  SetMenuCategory(mcSchedule);
#endif
  now = Now;
  helpKeys = -1;
  timerState = 0;
  Timers.Modified(timerState);
  for (cChannel *Channel = Channels.First(); Channel; Channel = Channels.Next(Channel)) {
      if (!Channel->GroupSep()) {
         const cSchedule *Schedule = Schedules->GetSchedule(Channel);
         if (Schedule) {
            const cEvent *Event = Now ? Schedule->GetPresentEvent() : Schedule->GetFollowingEvent();
            if (Event)
               Add(new cMenuScheduleItem(Event, Channel, false, Now), Channel->Number() == CurrentChannelNr);
            }
         }
      }
  currentChannel = CurrentChannelNr;
  Display();
  SetHelpKeys();
  int userFilter = USER_FROM_SETUP(RemoteTimersSetup.userFilterSchedule);
  if (userFilter != 0)
     SetTitle(cString::sprintf("%s %s %d", Now ? tr("What's on now?") : tr("What's on next?"), trREMOTETIMERS("User"), userFilter));
}

bool cMenuWhatsOn::Update(void)
{
  bool result = false;
  if (Timers.Modified(timerState)) {
     for (cOsdItem *item = First(); item; item = Next(item)) {
         if (((cMenuScheduleItem *)item)->Update())
            result = true;
         }
     }
  return result;
}

void cMenuWhatsOn::SetHelpKeys(void)
{
  cMenuScheduleItem *item = (cMenuScheduleItem *)Get(Current());
  int NewHelpKeys = 0;
  if (item) {
     if (item->timerMatch == tmFull)
        NewHelpKeys = 2;
     else
        NewHelpKeys = 1;
     }
  if (NewHelpKeys != helpKeys) {
     const char *Red[] = { NULL, tr("Button$Record"), tr("Button$Timer") };
     SetHelp(Red[NewHelpKeys], now ? tr("Button$Next") : tr("Button$Now"), tr("Button$Schedule"), RemoteTimersSetup.swapOkBlue ? tr("Button$Info") : tr("Button$Switch"));
     helpKeys = NewHelpKeys;
     }
}

const cEvent *cMenuWhatsOn::ScheduleEvent(void)
{
  const cEvent *ei = scheduleEvent;
  scheduleEvent = NULL;
  return ei;
}

eOSState cMenuWhatsOn::Switch(void)
{
  cMenuScheduleItem *item = (cMenuScheduleItem *)Get(Current());
  if (item) {
     cChannel *channel = Channels.GetByChannelID(item->event->ChannelID(), true);
     if (channel && cDevice::PrimaryDevice()->SwitchChannel(channel, true))
        return osEnd;
     }
  Skins.Message(mtError, tr("Can't switch channel!"));
  return osContinue;
}

eOSState cMenuWhatsOn::Record(void)
{
  cMenuScheduleItem *item = (cMenuScheduleItem *)Get(Current());
  if (item) {
     int tm = tmNone;
     bool isRemote = false;
     if (item->timerMatch == tmFull) {
        //cTimer *timer = Timers.GetMatch(item->event, &tm);
        cTimer *timer = GetBestMatch(item->event, MASK_FROM_SETUP(RemoteTimersSetup.userFilterSchedule), &tm, NULL, &isRemote);
        if (timer)
           return AddSubMenu(new cMenuEditRemoteTimer(timer, isRemote));
        }

     tm = tmNone;
     isRemote = true;
     cTimer *timer = new cTimer(item->event);
     cTimer *t = GetBestMatch(item->event, 0, &tm, NULL, &isRemote);
     if (!t || tm != tmFull) {
        isRemote = true;
        t = RemoteTimers.GetTimer(timer);
	}
     if (!t) {
        isRemote = false;
        t = Timers.GetTimer(timer);
        }
     if (t) {
        delete timer;
        timer = t;
	if (tm != tmFull) 
           return AddSubMenu(new cMenuEditRemoteTimer(timer, isRemote));
	else {
           // matching timer, but it was filtered
	   int user = cMenuTimerItem::ParseUser(t);
	   cMenuTimerItem::UpdateUser(*t, user | MASK_FROM_SETUP(RemoteTimersSetup.defaultUser));
	   if (isRemote) {
	      eRemoteTimersState state = RemoteTimers.Modify((cRemoteTimer*)t);
	      if (state > rtsRefresh)
	         Skins.Message(state == rtsLocked ? mtWarning : mtError, tr(RemoteTimers.GetErrorMessage(state)));
	      }
	   else {
	      Timers.SetModified();
	      }
	   }
        if (HasSubMenu())
           CloseSubMenu();
        if (Update())
           Display();
        SetHelpKeys();
        }
     else {
        isRemote = false;
        cMenuTimerItem::UpdateUser(*timer, MASK_FROM_SETUP(RemoteTimersSetup.defaultUser));
        Timers.Add(timer);
        if (RemoteTimersSetup.addToRemote && !cSvdrp::GetInstance()->Offline()) {
           isRemote = true;
           cRemoteTimer *rt = new cRemoteTimer();
           *(cTimer*) rt = *timer;
           eRemoteTimersState state = RemoteTimers.New(rt);
           if (state <= rtsRefresh) {
              Timers.Del(timer);
              timer = rt;
              }
           else {
              Skins.Message(state == rtsLocked ? mtWarning : mtError, tr(RemoteTimers.GetErrorMessage(state)));
              Timers.Del(timer);
              delete rt;
              return osContinue;
              }
           }
        Timers.SetModified();
        isyslog("timer %s added (active)", *timer->ToDescr());
        if (timer->Matches(0, false, NEWTIMERLIMIT))
           return AddSubMenu(new cMenuEditRemoteTimer(timer, isRemote));
        if (HasSubMenu())
           CloseSubMenu();
        if (Update())
           Display();
        SetHelpKeys();
        }
     }
  return osContinue;
}

eOSState cMenuWhatsOn::ProcessKey(eKeys Key)
{
  bool HadSubMenu = HasSubMenu();
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kRecord:
       case kRed:    return Record();
       case kYellow: state = osBack;
                     // continue with kGreen
       case kGreen:  {
                       cMenuScheduleItem *mi = (cMenuScheduleItem *)Get(Current());
                       if (mi) {
                          scheduleEvent = mi->event;
                          currentChannel = mi->channel->Number();
                          }
                     }
                     break;
       case kBlue:
       case kOk:     if ((Key == kOk && RemoteTimersSetup.swapOkBlue) || (Key == kBlue && !RemoteTimersSetup.swapOkBlue))
                        return Switch();
       case kInfo:   if (Count())
                        return AddSubMenu(new cMenuEvent(((cMenuScheduleItem *)Get(Current()))->event, true, true));
                     break;
       default:      break;
       }
     }
  else if (!HasSubMenu()) {
     if (HadSubMenu && Update())
        Display();
     if (Key != kNone)
        SetHelpKeys();
     }
  return state;
}

// --- cMenuSchedule ---------------------------------------------------------

/*
class cMenuSchedule : public cOsdMenu {
private:
  cSchedulesLock schedulesLock;
  const cSchedules *schedules;
  bool now, next;
  int otherChannel;
  int helpKeys;
  int timerState;
  eOSState Number(void);
  eOSState Record(void);
  eOSState Switch(void);
  void PrepareScheduleAllThis(const cEvent *Event, const cChannel *Channel);
  void PrepareScheduleThisThis(const cEvent *Event, const cChannel *Channel);
  void PrepareScheduleThisAll(const cEvent *Event, const cChannel *Channel);
  void PrepareScheduleAllAll(const cEvent *Event, const cChannel *Channel);
  bool Update(void);
  void SetHelpKeys(void);
public:
  cMenuSchedule(void);
  virtual ~cMenuSchedule();
  virtual eOSState ProcessKey(eKeys Key);
  };
*/

cMenuSchedule::cMenuSchedule(void)
:cOsdMenu("")
{
#if VDRVERSNUM >= 10728
  SetMenuCategory(mcSchedule);
#endif
  now = next = false;
  otherChannel = 0;
  helpKeys = -1;
  timerState = 0;
  userFilter = USER_FROM_SETUP(RemoteTimersSetup.userFilterSchedule);
  Timers.Modified(timerState);
  cMenuScheduleItem::SetSortMode(cMenuScheduleItem::ssmAllThis);
  if (!cSvdrp::GetInstance()->Connect() || RemoteTimers.Refresh() != rtsOk)
     Skins.Message(mtWarning, tr(MSG_UNAVAILABLE));
  cChannel *channel = Channels.GetByNumber(cDevice::CurrentChannel());
  if (channel) {
     cMenuWhatsOn::SetCurrentChannel(channel->Number());
     schedules = cSchedules::Schedules(schedulesLock);
     PrepareScheduleAllThis(NULL, channel);
     SetHelpKeys();
     }
}

cMenuSchedule::~cMenuSchedule()
{
  cMenuWhatsOn::ScheduleEvent(); // makes sure any posted data is cleared
  cSvdrp::GetInstance()->Disconnect();
}

void cMenuSchedule::PrepareScheduleAllThis(const cEvent *Event, const cChannel *Channel)
{
  Clear();
  SetCols(7, 6, 4);
  cString title(cString::sprintf(tr("Schedule - %s"), Channel->Name()));
  if (userFilter != 0)
     title = cString::sprintf("%s - %s %d", *title, trREMOTETIMERS("User"), userFilter);
  SetTitle(title);
  if (schedules && Channel) {
     const cSchedule *Schedule = schedules->GetSchedule(Channel);
     if (Schedule) {
        const cEvent *PresentEvent = Event ? Event : Schedule->GetPresentEvent();
        time_t now = time(NULL) - Setup.EPGLinger * 60;
        for (const cEvent *ev = Schedule->Events()->First(); ev; ev = Schedule->Events()->Next(ev)) {
            if (ev->EndTime() > now || ev == PresentEvent)
               Add(new cMenuScheduleItem(ev), ev == PresentEvent);
            }
        }
     }
}

void cMenuSchedule::PrepareScheduleThisThis(const cEvent *Event, const cChannel *Channel)
{
  Clear();
  SetCols(7, 6, 4);
  cString title(cString::sprintf(tr("This event - %s"), Channel->Name()));
  if (userFilter != 0)
     title = cString::sprintf("%s - %s %d", *title, trREMOTETIMERS("User"), userFilter);
  SetTitle(title);
  if (schedules && Channel && Event) {
     const cSchedule *Schedule = schedules->GetSchedule(Channel);
     if (Schedule) {
        time_t now = time(NULL) - Setup.EPGLinger * 60;
        for (const cEvent *ev = Schedule->Events()->First(); ev; ev = Schedule->Events()->Next(ev)) {
            if ((ev->EndTime() > now || ev == Event) && !strcmp(ev->Title(), Event->Title()))
               Add(new cMenuScheduleItem(ev), ev == Event);
            }
        }
     }
}

void cMenuSchedule::PrepareScheduleThisAll(const cEvent *Event, const cChannel *Channel)
{
  Clear();
  SetCols(CHNUMWIDTH, CHNAMWIDTH, 7, 6, 4);
  cString title(tr("This event - all channels"));
  if (userFilter != 0)
     title = cString::sprintf("%s - %s %d", *title, trREMOTETIMERS("User"), userFilter);
  SetTitle(title);
  if (schedules && Event) {
     for (cChannel *ch = Channels.First(); ch; ch = Channels.Next(ch)) {
         const cSchedule *Schedule = schedules->GetSchedule(ch);
         if (Schedule) {
            time_t now = time(NULL) - Setup.EPGLinger * 60;
            for (const cEvent *ev = Schedule->Events()->First(); ev; ev = Schedule->Events()->Next(ev)) {
                if ((ev->EndTime() > now || ev == Event) && !strcmp(ev->Title(), Event->Title()))
                   Add(new cMenuScheduleItem(ev, ch, true), ev == Event && ch == Channel);
                }
            }
         }
     }
}

void cMenuSchedule::PrepareScheduleAllAll(const cEvent *Event, const cChannel *Channel)
{
  Clear();
  SetCols(CHNUMWIDTH, CHNAMWIDTH, 7, 6, 4);
  cString title(tr("All events - all channels"));
  if (userFilter != 0)
     title = cString::sprintf("%s - %s %d", *title, trREMOTETIMERS("User"), userFilter);
  SetTitle(title);
  if (schedules) {
     for (cChannel *ch = Channels.First(); ch; ch = Channels.Next(ch)) {
         const cSchedule *Schedule = schedules->GetSchedule(ch);
         if (Schedule) {
            time_t now = time(NULL) - Setup.EPGLinger * 60;
            for (const cEvent *ev = Schedule->Events()->First(); ev; ev = Schedule->Events()->Next(ev)) {
                if (ev->EndTime() > now || ev == Event)
                   Add(new cMenuScheduleItem(ev, ch, true), ev == Event && ch == Channel);
                }
            }
         }
     }
}

bool cMenuSchedule::Update(void)
{
  bool result = false;
  if (Timers.Modified(timerState)) {
     for (cOsdItem *item = First(); item; item = Next(item)) {
         if (((cMenuScheduleItem *)item)->Update())
            result = true;
         }
     }
  return result;
}

void cMenuSchedule::SetHelpKeys(void)
{
  cMenuScheduleItem *item = (cMenuScheduleItem *)Get(Current());
  int NewHelpKeys = 0;
  if (item) {
     if (item->timerMatch == tmFull)
        NewHelpKeys = 2;
     else
        NewHelpKeys = 1;
     }
  if (NewHelpKeys != helpKeys) {
     const char *Red[] = { NULL, tr("Button$Record"), tr("Button$Timer") };
     SetHelp(Red[NewHelpKeys], tr("Button$Now"), tr("Button$Next"));
     helpKeys = NewHelpKeys;
     }
}

eOSState cMenuSchedule::Number(void)
{
  cMenuScheduleItem::IncSortMode();
  cMenuScheduleItem *CurrentItem = (cMenuScheduleItem *)Get(Current());
  const cChannel *Channel = NULL;
  const cEvent *Event = NULL;
  if (CurrentItem) {
     Event = CurrentItem->event;
     Channel = Channels.GetByChannelID(Event->ChannelID(), true);
     }
  else
     Channel = Channels.GetByNumber(cDevice::CurrentChannel());
  switch (cMenuScheduleItem::SortMode()) {
    case cMenuScheduleItem::ssmAllThis:  PrepareScheduleAllThis(Event, Channel); break;
    case cMenuScheduleItem::ssmThisThis: PrepareScheduleThisThis(Event, Channel); break;
    case cMenuScheduleItem::ssmThisAll:  PrepareScheduleThisAll(Event, Channel); break;
    case cMenuScheduleItem::ssmAllAll:   PrepareScheduleAllAll(Event, Channel); break;
    default: esyslog("ERROR: unknown SortMode %d (%s %d)", cMenuScheduleItem::SortMode(), __FUNCTION__, __LINE__);
    }
  CurrentItem = (cMenuScheduleItem *)Get(Current());
  Sort();
  SetCurrent(CurrentItem);
  Display();
  return osContinue;
}

eOSState cMenuSchedule::Record(void)
{
  cMenuScheduleItem *item = (cMenuScheduleItem *)Get(Current());
  if (item) {
     int tm = tmNone;
     bool isRemote = false;
     if (item->timerMatch == tmFull) {
        //cTimer *timer = Timers.GetMatch(item->event, &tm);
        cTimer *timer = GetBestMatch(item->event, MASK_FROM_SETUP(RemoteTimersSetup.userFilterSchedule), &tm, NULL, &isRemote);
        if (timer)
           return AddSubMenu(new cMenuEditRemoteTimer(timer, isRemote));
        }

     tm = tmNone;
     isRemote = true;
     cTimer *timer = new cTimer(item->event);
     cTimer *t = GetBestMatch(item->event, 0, &tm, NULL, &isRemote);
     if (!t || tm != tmFull) {
	isRemote = true;
        t = RemoteTimers.GetTimer(timer);
        }
     if (!t) {
        isRemote = false;
        t = Timers.GetTimer(timer);
        }
     if (t) {
        delete timer;
        timer = t;
	if (tm != tmFull)
           return AddSubMenu(new cMenuEditRemoteTimer(timer, isRemote));
	else {
	   // matching timer, but it was filtered
	   int user = cMenuTimerItem::ParseUser(t);
	   cMenuTimerItem::UpdateUser(*t, user | MASK_FROM_SETUP(RemoteTimersSetup.defaultUser));
	   if (isRemote) {
	      eRemoteTimersState state = RemoteTimers.Modify((cRemoteTimer*)t);
	      if (state > rtsRefresh)
	         Skins.Message(state == rtsLocked ? mtWarning : mtError, tr(RemoteTimers.GetErrorMessage(state)));
	      }
	   else {
	      Timers.SetModified();
	      }
	   }
        if (HasSubMenu())
           CloseSubMenu();
        if (Update())
           Display();
        SetHelpKeys();
        }
     else {
        isRemote = false;
        cMenuTimerItem::UpdateUser(*timer, MASK_FROM_SETUP(RemoteTimersSetup.defaultUser));
        Timers.Add(timer);
        if (RemoteTimersSetup.addToRemote && !cSvdrp::GetInstance()->Offline()) {
           isRemote = true;
           cRemoteTimer *rt = new cRemoteTimer();
           *(cTimer*) rt = *timer;
           eRemoteTimersState state = RemoteTimers.New(rt);
           if (state <= rtsRefresh) {
              Timers.Del(timer);
              timer = rt;
              }
           else {
              Skins.Message(state == rtsLocked ? mtWarning : mtError, tr(RemoteTimers.GetErrorMessage(state)));
              Timers.Del(timer);
              delete rt;
              return osContinue;
              }
           }
        Timers.SetModified();
        isyslog("timer %s added (active)", *timer->ToDescr());
        if (timer->Matches(0, false, NEWTIMERLIMIT))
           return AddSubMenu(new cMenuEditRemoteTimer(timer, isRemote));
        if (HasSubMenu())
           CloseSubMenu();
        if (Update())
           Display();
        SetHelpKeys();
        }
     }
  return osContinue;
}

eOSState cMenuSchedule::Switch(void)
{
  if (otherChannel) {
     if (Channels.SwitchTo(otherChannel))
        return osEnd;
     }
  Skins.Message(mtError, tr("Can't switch channel!"));
  return osContinue;
}

eOSState cMenuSchedule::ProcessKey(eKeys Key)
{
  bool HadSubMenu = HasSubMenu();
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case k0:      return Number();
       case kRecord:
       case kRed:    return Record();
       case kGreen:  if (schedules) {
                        if (!now && !next) {
                           int ChannelNr = 0;
                           if (Count()) {
                              cChannel *channel = Channels.GetByChannelID(((cMenuScheduleItem *)Get(Current()))->event->ChannelID(), true);
                              if (channel)
                                 ChannelNr = channel->Number();
                              }
                           now = true;
                           return AddSubMenu(new cMenuWhatsOn(schedules, true, ChannelNr));
                           }
                        now = !now;
                        next = !next;
                        return AddSubMenu(new cMenuWhatsOn(schedules, now, cMenuWhatsOn::CurrentChannel()));
                        }
       case kYellow: if (schedules)
                        return AddSubMenu(new cMenuWhatsOn(schedules, false, cMenuWhatsOn::CurrentChannel()));
                     break;
       case kBlue:   if (Count() && otherChannel)
                        return Switch();
                     break;
       case kInfo:
       case kOk:     if (Count())
                        return AddSubMenu(new cMenuEvent(((cMenuScheduleItem *)Get(Current()))->event, otherChannel, true));
                     break;
       default:      break;
       }
     }
  else if (!HasSubMenu()) {
     now = next = false;
     const cEvent *ei = cMenuWhatsOn::ScheduleEvent();
     if (ei) {
        cChannel *channel = Channels.GetByChannelID(ei->ChannelID(), true);
        if (channel) {
           cMenuScheduleItem::SetSortMode(cMenuScheduleItem::ssmAllThis);
           PrepareScheduleAllThis(NULL, channel);
           if (channel->Number() != cDevice::CurrentChannel()) {
              otherChannel = channel->Number();
              SetHelp(Count() ? tr("Button$Record") : NULL, tr("Button$Now"), tr("Button$Next"), tr("Button$Switch"));
              }
           Display();
           }
        }
     else if (HadSubMenu && Update())
        Display();
     if (Key != kNone)
        SetHelpKeys();
     }
  return state;
}

#if VDRVERSNUM < 10713
// --- cMenuCommands ---------------------------------------------------------

class cMenuCommands : public cOsdMenu {
private:
#if APIVERSNUM < 10712
  cCommands *commands;
  char *parameters;
#else
  cList<cNestedItem> *commands;
  cString parameters;
  cString title;
  cString command;
  bool confirm;
  char *result;
  bool Parse(const char *s);
#endif
  eOSState Execute(void);
public:
#if APIVERSNUM < 10712
  cMenuCommands(const char *Title, cCommands *Commands, const char *Parameters = NULL);
#else
  cMenuCommands(const char *Title, cList<cNestedItem> *Commands, const char *Parameters = NULL);
#endif
  virtual ~cMenuCommands();
  virtual eOSState ProcessKey(eKeys Key);
  };

#if APIVERSNUM < 10712
cMenuCommands::cMenuCommands(const char *Title, cCommands *Commands, const char *Parameters)
:cOsdMenu(Title)
{
  SetHasHotkeys();
  commands = Commands;
  parameters = Parameters ? strdup(Parameters) : NULL;
  for (cCommand *command = commands->First(); command; command = commands->Next(command))
      Add(new cOsdItem(hk(command->Title())));
}
#else
cMenuCommands::cMenuCommands(const char *Title, cList<cNestedItem> *Commands, const char *Parameters)
:cOsdMenu(Title)
{
  result = NULL;
  SetHasHotkeys();
  commands = Commands;
  parameters = Parameters;
  for (cNestedItem *Command = commands->First(); Command; Command = commands->Next(Command)) {
      const char *s = Command->Text();
      if (Command->SubItems())
         Add(new cOsdItem(hk(cString::sprintf("%s...", s))));
      else if (Parse(s))
         Add(new cOsdItem(hk(title)));
      }
}
#endif

cMenuCommands::~cMenuCommands()
{
#if APIVERSNUM < 10712
  free(parameters);
#else
  free(result);
#endif
}

#if APIVERSNUM < 10712
eOSState cMenuCommands::Execute(void)
{
  cCommand *command = commands->Get(Current());
  if (command) {
     bool confirmed = true;
     if (command->Confirm())
        confirmed = Interface->Confirm(cString::sprintf("%s?", command->Title()));
     if (confirmed) {
        Skins.Message(mtStatus, cString::sprintf("%s...", command->Title()));
        const char *Result = command->Execute(parameters);
        Skins.Message(mtStatus, NULL);
        if (Result)
           return AddSubMenu(new cMenuText(command->Title(), Result, fontFix));
        return osEnd;
        }
     }
  return osContinue;
}
#else
bool cMenuCommands::Parse(const char *s)
{
  const char *p = strchr(s, ':');
  if (p) {
     int l = p - s;
     if (l > 0) {
        char t[l + 1];
        stripspace(strn0cpy(t, s, l + 1));
        l = strlen(t);
        if (l > 1 && t[l - 1] == '?') {
           t[l - 1] = 0;
           confirm = true;
           }
        else
           confirm = false;
        title = t;
        command = skipspace(p + 1);
        return true;
        }
     }
  return false;
}

eOSState cMenuCommands::Execute(void)
{
  cNestedItem *Command = commands->Get(Current());
  if (Command) {
     if (Command->SubItems())
        return AddSubMenu(new cMenuCommands(Title(), Command->SubItems(), parameters));
     if (Parse(Command->Text())) {
        if (!confirm || Interface->Confirm(cString::sprintf("%s?", *title))) {
           Skins.Message(mtStatus, cString::sprintf("%s...", *title));
           free(result);
           result = NULL;
           cString cmdbuf;
           if (!isempty(parameters))
              cmdbuf = cString::sprintf("%s %s", *command, *parameters);
           const char *cmd = *cmdbuf ? *cmdbuf : *command;
           dsyslog("executing command '%s'", cmd);
           cPipe p;
           if (p.Open(cmd, "r")) {
              int l = 0;
              int c;
              while ((c = fgetc(p)) != EOF) {
                    if (l % 20 == 0) {
                       if (char *NewBuffer = (char *)realloc(result, l + 21))
                          result = NewBuffer;
                       else {
                          esyslog("ERROR: out of memory");
                          break;
                          }
                       }
                    result[l++] = char(c);
                    }
              if (result)
                 result[l] = 0;
              p.Close();
              }
           else
              esyslog("ERROR: can't open pipe for command '%s'", cmd);
           Skins.Message(mtStatus, NULL);
           if (result)
              return AddSubMenu(new cMenuText(title, result, fontFix));
           return osEnd;
           }
        }
     }
  return osContinue;
}
#endif

eOSState cMenuCommands::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kRed:
       case kGreen:
       case kYellow:
       case kBlue:   return osContinue;
       case kOk:     return Execute();
       default:      break;
       }
     }
  return state;
}
#endif

// --- cMenuRecording --------------------------------------------------------

class cMenuRecording : public cOsdMenu {
private:
  const cRecording *recording;
  bool withButtons;
public:
  cMenuRecording(const cRecording *Recording, bool WithButtons = false);
  virtual void Display(void);
  virtual eOSState ProcessKey(eKeys Key);
};

cMenuRecording::cMenuRecording(const cRecording *Recording, bool WithButtons)
:cOsdMenu(tr("Recording info"))
{
#if VDRVERSNUM >= 10728
  SetMenuCategory(mcRecording);
#endif
  recording = Recording;
  withButtons = WithButtons;
  if (withButtons)
     SetHelp(tr("Button$Play"), tr("Button$Rewind"));
  cIndexFile index(Recording->FileName(), false);
  int last = index.Last();
  SetTitle(cString::sprintf("%s - %d MB %s%s", tr("Recording info"), DirSizeMB(Recording->FileName()), last >= 0 ? "- " : "", last >= 0 ? *IndexToHMSF(last) : ""));
}

void cMenuRecording::Display(void)
{
  cOsdMenu::Display();
  DisplayMenu()->SetRecording(recording);
  if (recording->Info()->Description())
     cStatus::MsgOsdTextItem(recording->Info()->Description());
}

eOSState cMenuRecording::ProcessKey(eKeys Key)
{
  switch (int(Key)) {
    case kUp|k_Repeat:
    case kUp:
    case kDown|k_Repeat:
    case kDown:
    case kLeft|k_Repeat:
    case kLeft:
    case kRight|k_Repeat:
    case kRight:
                  DisplayMenu()->Scroll(NORMALKEY(Key) == kUp || NORMALKEY(Key) == kLeft, NORMALKEY(Key) == kLeft || NORMALKEY(Key) == kRight);
                  cStatus::MsgOsdTextItem(NULL, NORMALKEY(Key) == kUp || NORMALKEY(Key) == kLeft);
                  return osContinue;
    case kInfo:   return osBack;
    default: break;
    }

  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kRed:    if (withButtons)
                        Key = kOk; // will play the recording, even if recording commands are defined
       case kGreen:  if (!withButtons)
                        break;
                     cRemote::Put(Key, true);
                     // continue with osBack to close the info menu and process the key
       case kOk:     return osBack;
       default: break;
       }
     }
  return state;
}

// --- cMenuEditRecording --------------------------------------------------------

class cMenuEditRecording : public cOsdMenu {
private:
  int priority;
  int lifetime;
  char name[MaxFileName];
  int user;
  int tmpUser;
  cString fileName;
  cMenuFolder *subMenuFolder;
  cMenuEditStrItem *file;
  eOSState SetFolder(void);
  eOSState Commands(eKeys Key = kNone);
  eOSState Cut(void);
  eOSState Info(void);
  void SetHelpKeys(void);
  bool Rename(const cRecording *Recording, const char *NewName);
  bool UpdatePrioLife(cRecording *Recording);
  bool UpdateName(cRecording *Recording);
  static bool ModifyInfo(cRecording *Recording, const char *buffer);
public:
  static bool UpdateUser(cRecording *Recording, int NewUser);

  cMenuEditRecording(const cRecording *Recording);
  virtual ~cMenuEditRecording();
  virtual eOSState ProcessKey(eKeys Key);
};

cMenuEditRecording::cMenuEditRecording(const cRecording *Recording)
:cOsdMenu(trREMOTETIMERS("Edit recording"), 12)
{
#if VDRVERSNUM >= 10728
  SetMenuCategory(mcRecording);
#endif
  // Must be locked against Recordings
#if VDRVERSNUM < 10721
  priority = Recording->priority;
  lifetime = Recording->lifetime;
#else
  priority = Recording->Priority();
  lifetime = Recording->Lifetime();
#endif
  strn0cpy(name, Recording->Name(), sizeof(name));
  tmpUser = user = ParseUser(Recording->Info()->Aux());
  fileName = Recording->FileName();
  subMenuFolder = NULL;
  file = NULL;
  SetHelpKeys();
  cIndexFile index(*fileName, false);
  int last = index.Last();
  SetTitle(cString::sprintf("%s - %d MB %s%s", trREMOTETIMERS("Edit recording"), DirSizeMB(*fileName), last >= 0 ? "- " : "", last >= 0 ? *IndexToHMSF(last) : ""));
  Add(new cMenuEditIntItem(tr("Priority"), &priority, 0, MAXPRIORITY));
  Add(new cMenuEditIntItem(tr("Lifetime"), &lifetime, 0, MAXLIFETIME));
  Add(new cMenuEditUserItem(trREMOTETIMERS("User ID"), &tmpUser));
  Add(file = new cMenuEditStrItem(tr("File"), name, sizeof(name), tr(FileNameChars)));
  Display();
}

cMenuEditRecording::~cMenuEditRecording()
{
}

eOSState cMenuEditRecording::Commands(eKeys Key)
{
  if (HasSubMenu())
     return osContinue;
  cMenuCommands *menu;
  eOSState state = AddSubMenu(menu = new cMenuCommands(tr("Recording commands"), &RecordingCommands, cString::sprintf("\"%s\"", *strescape(*fileName, "\\\"$"))));
  if (Key != kNone) {
     state = menu->ProcessKey(Key);
     return state;
     }
  return osContinue;
}

eOSState cMenuEditRecording::Cut()
{
  cThreadLock RecordingsLock(&Recordings);
  cRecording *recording = Recordings.GetByName(*fileName);
  if (recording) {
     cMarks Marks;
#if VDRVERSNUM >= 10703
     if (Marks.Load(recording->FileName(), recording->FramesPerSecond(), recording->IsPesRecording()) && Marks.Count()) {
#else
     if (Marks.Load(recording->FileName()) && Marks.Count()) {
#endif
        const char *name = recording->Name();
        int len = strlen(RemoteTimersSetup.serverDir);
        bool remote = len == 0 || (strstr(name, RemoteTimersSetup.serverDir) == name && name[len] == FOLDERDELIMCHAR);
        if (!remote) {
           if (!cCutter::Active()) {
              if (cCutter::Start(*fileName))
                 Skins.Message(mtInfo, tr("Editing process started"));
              else
                 Skins.Message(mtError, tr("Can't start editing process!"));
              }
           else
              Skins.Message(mtError, tr("Editing process already active!"));
           }
        else if (cSvdrp::GetInstance()->Connect()) {
           char *date = strdup(recording->Title(' ', false));
           char *p = strchr(date, ' ');
           if (p && (p = strchr(++p, ' ')) && p) {
              *p = 0;
              eRemoteRecordingsState state = RemoteRecordings.Cut(date, name + (len ? len + 1 : 0));
              if (state == rrsOk)
                 Skins.Message(mtInfo, trREMOTETIMERS("Remote editing process started"));
              else
                 Skins.Message(state == rrsLocked ? mtWarning : mtError, tr(RemoteRecordings.GetErrorMessage(state)));
              }
           else
              esyslog("remotetimers: unexpected title format '%s'", date);
           free(date);
           cSvdrp::GetInstance()->Disconnect();
           }
        else {
           Skins.Message(mtError, tr(MSG_UNAVAILABLE));
           cSvdrp::GetInstance()->Disconnect();
           }
        }
     else
        Skins.Message(mtError, tr("No editing marks defined!"));
     }
  else
     Skins.Message(mtError, tr("Error while accessing recording!"));
  return osContinue;
}

eOSState cMenuEditRecording::Info(void)
{
  if (HasSubMenu())
     return osContinue;
  cThreadLock RecordingsLock(&Recordings);
  cRecording *recording = Recordings.GetByName(*fileName);
  if (recording && recording->Info()->Title())
     return AddSubMenu(new cMenuRecording(recording, true));
  return osContinue;
}

#define INFOFILE_PES "info.vdr"
#define INFOFILE_TS "info"
bool cMenuEditRecording::ModifyInfo(cRecording *Recording, const char *Info)
{
#if VDRVERSNUM >= 10703
  cString InfoFileName = cString::sprintf(Recording->IsPesRecording() ? "%s/" INFOFILE_PES : "%s/" INFOFILE_TS, Recording->FileName());
  FILE *f = fopen(InfoFileName, "a");
  if (f) {
     if (fprintf(f, "%s\n", Info) > 0) {
        if (fclose(f) == 0) {
           cRecording rec(Recording->FileName());
           // as of VDR 1.7.21 WriteInfo() always returns true
           return rec.WriteInfo();
           }
        f = NULL;
        }
     esyslog("remotetimers: Failed to update '%s': %m", *InfoFileName);
     if (f)
        fclose(f);
     }
  else
     esyslog("remotetimers: writing to '%s' failed: %m", *InfoFileName);
#else
  cString InfoFileName = cString::sprintf("%s/" INFOFILE_PES, Recording->FileName());
  // check for write access as cRecording::WriteInfo() always returns true
  if (access(InfoFileName, W_OK) == 0) {
     FILE *f = fmemopen((void *) Info, strlen(Info) * sizeof(char), "r");
     if (f) {
        // Casting const away is nasty, but what the heck?
        // The Recordings thread is locked and the object is going to be deleted anyway.
        if (((cRecordingInfo *)Recording->Info())->Read(f) && Recording->WriteInfo())
           return true;
        esyslog("remotetimers: error in info string '%s'", Info);
	}
     else
        esyslog("remotetimers: error in fmemopen: %m");
     }
  else
     esyslog("remotetimers: '%s' not writeable: %m", *InfoFileName);
#endif
  return false;
}

bool cMenuEditRecording::UpdateUser(cRecording *Recording, int NewUser)
{
  cString buffer(PluginRemoteTimers::UpdateUser(Recording->Info()->Aux(), NewUser));
  buffer = cString::sprintf("@ %s", *buffer);
  if (ModifyInfo(Recording, buffer))
     return true;
  Skins.Message(mtError, trREMOTETIMERS("Unable to update user ID"));
  return false;
}

#define PRIO_LIFE_FORMAT ".%02d.%02d.rec"
bool cMenuEditRecording::UpdatePrioLife(cRecording *Recording)
{
#if VDRVERSNUM >= 10703
  if (!Recording->IsPesRecording()) {
     cString buffer = cString::sprintf("P %d\nL %d", priority, lifetime);
     if (ModifyInfo(Recording, *buffer))
        return true;
     }
  else
#endif
  {
     char *newName = strdup(Recording->FileName());
     size_t len = strlen(newName);
     cString freeStr(newName, true);
#if VDRVERSNUM < 10721
     cString oldData = cString::sprintf(PRIO_LIFE_FORMAT, Recording->priority, Recording->lifetime);
#else
     cString oldData = cString::sprintf(PRIO_LIFE_FORMAT, Recording->Priority(), Recording->Lifetime());
#endif
     cString newData = cString::sprintf(PRIO_LIFE_FORMAT, priority, lifetime);
     size_t lenReplace = strlen(oldData);
     if (lenReplace < len) {
        if (strlen(newData) == lenReplace) {
           char *p = newName + len - lenReplace;
           if (strcmp(p, oldData) == 0) {
              strncpy(p, newData, lenReplace);
              if (Rename(Recording, newName)) {
                 fileName = newName;
                 return true;
                 }
              }
           else
              esyslog("remotetimers: unexpected filename '%s'", *fileName);
           }
        else
           esyslog("remotetimers: invalid priority/lifetime data for '%s'", *fileName);
        }
     else
        esyslog("remotetimers: short filename '%s'", *fileName);
     }
  Skins.Message(mtError, trREMOTETIMERS("Unable to change priority/lifetime"));
  return false;
}

bool cMenuEditRecording::UpdateName(cRecording *Recording)
{
  int len = strlen(name);
  if (!len) {
     // user cleared name - restore previous value
     strn0cpy(name, Recording->Name(), sizeof(name));
     return false;
  }

  const char *p = strrchr(Recording->FileName(), '/');
  if (p) {
     // cMenuEditStrItem strips trailing whitespace characters
     // If name ends with FOLDERDELIMCHAR, assume there was a space
     if (len < MaxFileName - 1 && name[len - 1] == FOLDERDELIMCHAR) {
         name[len++] = ' ';
         name[len] = '\0';
     }
     cString newName(ExchangeChars(strdup(name), true), true);
     newName = cString::sprintf("%s/%s%s", VideoDirectory, *newName, p);
     bool wasMoving = cMoveRec::IsMoving();
     if (Rename(Recording, newName)) {
        // keep old name when moving recording in background
        if (cMoveRec::IsMoving() && !wasMoving)
           return false;
	fileName = newName;
        return true;
        }
     }
  Skins.Message(mtError, trREMOTETIMERS("Unable to rename recording"));
  return false;
}

bool cMenuEditRecording::Rename(const cRecording *Recording, const char *NewName) {
  const char *oldName = Recording->FileName();
  if (access(NewName, F_OK) == 0) {
     esyslog("remotetimers: not renaming '%s' to '%s'. File exists", oldName, NewName);
     return false;
     }
  // false makes sure that the actual target directory itself is not created
  if (!MakeDirs(NewName, false))
     return false;
  if (rename(oldName, NewName) != 0) {
     if (errno == EXDEV) {
        if (Interface->Confirm(trREMOTETIMERS("Move to other filesystem in background?"))) {
           return cMoveRec::GetInstance()->Move(Recording, NewName);
           }
        }
     else
        esyslog("remotetimers: error renaming '%s' to '%s': %m", oldName, NewName);
     return false;
     }
  return true;
}

eOSState cMenuEditRecording::SetFolder(void)
{
  cMenuFolder *mf = subMenuFolder;
  if (mf) {
     cString Folder = mf->GetFolder();
     char *p = strrchr(name, FOLDERDELIMCHAR);
     if (p)
        p++;
     else
        p = name;
     if (!isempty(*Folder))
        strn0cpy(name, cString::sprintf("%s%c%s", *Folder, FOLDERDELIMCHAR, p), sizeof(name));
     else if (p != name)
        memmove(name, p, strlen(p) + 1);
     SetCurrent(file);
     Display();
     subMenuFolder = NULL;
     }
  return CloseSubMenu();
}

void cMenuEditRecording::SetHelpKeys()
{
  SetHelp(trREMOTETIMERS("Button$Folder"), trREMOTETIMERS("Cut"), RecordingCommands.Count() ? tr("Commands") : NULL);
}

eOSState cMenuEditRecording::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kRed:    return AddSubMenu(subMenuFolder = new cMenuFolder(trREMOTETIMERS("Select folder"), &Folders, name));
       case kGreen:  return Cut();
       case kYellow: return Commands();
       case kInfo:   return Info();
       case kOk:     {
                        cThreadLock recordingsLock(&Recordings);
                        cRecording *recording = Recordings.GetByName(*fileName);
                        if (recording) {
                           bool replace = false;
                           if (user != tmpUser)
                              replace |= UpdateUser(recording, tmpUser);
#if VDRVERSNUM < 10721
                           if (priority != recording->priority || lifetime != recording->lifetime)
#else
                           if (priority != recording->Priority() || lifetime != recording->Lifetime())
#endif
                              replace |= UpdatePrioLife(recording);
                           if (replace) {
                              Recordings.Del(recording);
                              Recordings.AddByName(*fileName);
                              recording = Recordings.GetByName(*fileName);
                              }
                           }
                        if (recording) {
                           if (strcmp(name, recording->Name()) != 0)
                              if (UpdateName(recording)) {
                                 Recordings.Del(recording);
                                 Recordings.AddByName(*fileName);
                              }
                           }
                        else
                           Skins.Message(mtError, tr("Error while accessing recording!"));
		        return osBack;
                     }
       default: break;
       }
     }
  else if (state == osEnd && HasSubMenu())
     state = SetFolder();
  else if (Key == kOk || Key == kBack)
     SetHelpKeys();
  return state;
}

// --- cRemoteTimersReplayControl -------------------------------------------------------

class cRemoteTimersReplayControl: public cReplayControl
{
private:
  // was PluginRemoteTimers::cMenuMain open when replay was started?
  bool openMainMenu;
public:
  virtual eOSState ProcessKey(eKeys Key);
  cRemoteTimersReplayControl(bool OpenMainMenu);
};

cRemoteTimersReplayControl::cRemoteTimersReplayControl(bool OpenMainMenu)
{
  openMainMenu = OpenMainMenu;
}

eOSState cRemoteTimersReplayControl::ProcessKey(eKeys Key)
{
  eOSState state = cReplayControl::ProcessKey(Key);
  if (state == osRecordings) {
     cMenuRecordings::SetReplayEnded();
     if (openMainMenu) {
        if (!cRemote::CallPlugin(PLUGIN_NAME_I18N))
           esyslog("remotetimers: Unable to return to recordings menu");
        state = osEnd;
     }
  }
  return state;
}

// --- cMenuRecordingItem ----------------------------------------------------

class cMenuRecordingItem : public cOsdItem {
private:
  char *fileName;
  char *name;
  int user;
  int totalEntries, newEntries;
public:
  cMenuRecordingItem(cRecording *Recording, int Level, int User);
  ~cMenuRecordingItem();
  void IncrementCounter(bool New);
  const char *Name(void) { return name; }
  const char *FileName(void) { return fileName; }
  int User(void) { return user; } const
  bool IsDirectory(void) { return name != NULL; }
  };

cMenuRecordingItem::cMenuRecordingItem(cRecording *Recording, int Level, int User)
{
  fileName = strdup(Recording->FileName());
  name = NULL;
  user = User;
  totalEntries = newEntries = 0;
  SetText(Recording->Title('\t', true, Level));
  if (*Text() == '\t')
     name = strdup(Text() + 2); // 'Text() + 2' to skip the two '\t'
}

cMenuRecordingItem::~cMenuRecordingItem()
{
  free(fileName);
  free(name);
}

void cMenuRecordingItem::IncrementCounter(bool New)
{
  totalEntries++;
  if (New)
     newEntries++;
#if VDRVERSNUM < 10721
  SetText(cString::sprintf("%d\t%d\t%s", totalEntries, newEntries, name));
#else
  SetText(cString::sprintf("%d\t\t%d\t%s", totalEntries, newEntries, name));
#endif
}

// --- cMenuRecordings -------------------------------------------------------

int cMenuRecordings::userFilter = 0;
bool cMenuRecordings::replayEnded = false;

cMenuRecordings::cMenuRecordings(const char *Base, int Level, bool OpenSubMenus)
#if VDRVERSNUM < 10721
:cOsdMenu(Base ? Base : tr("Recordings"), 9, 7)
#else
:cOsdMenu(Base ? Base : tr("Recordings"), 9, 6, 6)
#endif
{
#if VDRVERSNUM >= 10728
  SetMenuCategory(mcRecording);
#endif
  base = Base ? strdup(Base) : NULL;
  level = ::Setup.RecordingDirs ? Level : -1;
  if (level <= 0 && !replayEnded)
     userFilter = USER_FROM_SETUP(RemoteTimersSetup.userFilterRecordings);
  Recordings.StateChanged(recordingsState); // just to get the current state
  helpKeys = -1;
  Display(); // this keeps the higher level menus from showing up briefly when pressing 'Back' during replay
  Set();
#ifdef REMOTETIMERS_DISKSPACE
  SetFreeDiskDisplay(true);
#endif
  OpenSubMenus |= replayEnded;
  replayEnded = false;
  if (Current() < 0)
     SetCurrent(First());
  else if (OpenSubMenus && cReplayControl::LastReplayed() && Open(true))
     return;
  Display();
  SetHelpKeys();
}

cMenuRecordings::~cMenuRecordings()
{
  helpKeys = -1;
  free(base);
}

#ifdef REMOTETIMERS_DISKSPACE
bool cMenuRecordings::SetFreeDiskDisplay(bool Force)
{
#if VDRVERSNUM >= 10515
  if (FreeDiskSpace.HasChanged(base, Force)) {
#else
  if (FreeDiskSpace.HasChanged(Force)) {
#endif
     //XXX -> skin function!!!
     if (userFilter == 0)
        SetTitle(cString::sprintf("%s - %s", base ? base : tr("Recordings"), FreeDiskSpace.FreeDiskSpaceString()));
     else
        SetTitle(cString::sprintf("%s - %s - %s %d", base ? base : tr("Recordings"), FreeDiskSpace.FreeDiskSpaceString(), trREMOTETIMERS("User"), userFilter));
     return true;
     }
  return false;
}
#endif

#define B_RELEASE 0x100
void cMenuRecordings::SetHelpKeys(void)
{
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  int NewHelpKeys = 0;
  if (ri) {
     if (ri->IsDirectory())
        NewHelpKeys = 1;
     else {
        NewHelpKeys = 2;
        cRecording *recording = GetRecording(ri);
        if (recording && recording->Info()->Title())
           NewHelpKeys = 3;
        if (userFilter != 0 && ri->User() != 0 && ri->User() != USER_MASK(userFilter))
	   NewHelpKeys |= B_RELEASE;
        }
     }
  if (NewHelpKeys != helpKeys) {
     switch (NewHelpKeys) {
       case 0: SetHelp(NULL); break;
       case 1: SetHelp(tr("Button$Open")); break;
       //case 2:
       //case 3: SetHelp(RecordingCommands.Count() ? tr("Commands") : tr("Button$Play"), tr("Button$Rewind"), tr("Button$Delete"), NewHelpKeys == 3 ? tr("Button$Info") : NULL);
       // TRANSLATORS: Button displayed instead of "Delete" when there are other users who didn't watch the recording yet
       default: SetHelp(tr("Button$Edit"), tr("Button$Rewind"), (NewHelpKeys & B_RELEASE) ? trREMOTETIMERS("Release") : tr("Button$Delete"), (NewHelpKeys & ~B_RELEASE) == 3 ? tr("Button$Info") : NULL);
       }
     helpKeys = NewHelpKeys;
     }
}

void cMenuRecordings::Set(bool Refresh)
{
  const char *CurrentRecording = cReplayControl::LastReplayed();
  cMenuRecordingItem *LastItem = NULL;
  char *LastItemText = NULL;
  cThreadLock RecordingsLock(&Recordings);
  if (Refresh) {
     cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
     if (ri) {
        cRecording *Recording = Recordings.GetByName(ri->FileName());
        if (Recording)
           CurrentRecording = Recording->FileName();
        }
     }
  Clear();
  Recordings.Sort();
  for (cRecording *recording = Recordings.First(); recording; recording = Recordings.Next(recording)) {
      int user = ParseUser(recording->Info()->Aux());
      if (userFilter != 0 && user != 0 && (user & USER_MASK(userFilter)) == 0)
         continue;
      if (!base || (strstr(recording->Name(), base) == recording->Name() && recording->Name()[strlen(base)] == FOLDERDELIMCHAR)) {
         cMenuRecordingItem *Item = new cMenuRecordingItem(recording, level, user);
         if (*Item->Text() && (!Item->IsDirectory() || (!LastItem || !LastItem->IsDirectory() || strcmp(Item->Text(), LastItemText) != 0))) {
            Add(Item);
            LastItem = Item;
            free(LastItemText);
            LastItemText = strdup(LastItem->Text()); // must use a copy because of the counters!
            }
         else
            delete Item;
         if (LastItem) {
            if (CurrentRecording && strcmp(CurrentRecording, recording->FileName()) == 0)
               SetCurrent(LastItem);
            if (LastItem->IsDirectory())
               LastItem->IncrementCounter(recording->IsNew());
            }
         }
      }
  free(LastItemText);
#ifdef REMOTETIMERS_DISKSPACE
  Refresh |= SetFreeDiskDisplay(Refresh);
#endif
  if (Refresh)
     Display();
}

cRecording *cMenuRecordings::GetRecording(cMenuRecordingItem *Item)
{
  cRecording *recording = Recordings.GetByName(Item->FileName());
  if (!recording)
     Skins.Message(mtError, tr("Error while accessing recording!"));
  return recording;
}

bool cMenuRecordings::Open(bool OpenSubMenus)
{
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && ri->IsDirectory()) {
     const char *t = ri->Name();
     cString buffer;
     if (base) {
        buffer = cString::sprintf("%s~%s", base, t);
        t = buffer;
        }
     AddSubMenu(new cMenuRecordings(t, level + 1, OpenSubMenus));
     return true;
     }
  return false;
}

eOSState cMenuRecordings::Play(void)
{
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri) {
     if (ri->IsDirectory())
        Open();
     else {
        cRecording *recording = GetRecording(ri);
        if (recording) {
#if VDRVERSNUM < 10728
           cReplayControl::SetRecording(recording->FileName(), recording->Title());
#else
           cReplayControl::SetRecording(recording->FileName());
#endif
	   // use our own replay control which returns to this recordings menu
	   cControl::Shutdown();
	   cControl::Launch(new cRemoteTimersReplayControl(cMenuMain::IsOpen()));
	   return osEnd;
           // return osReplay;
           }
        }
     }
  return osContinue;
}

eOSState cMenuRecordings::Rewind(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && !ri->IsDirectory()) {
     cRecording *recording = GetRecording(ri);
     if (recording) {
        cDevice::PrimaryDevice()->StopReplay(); // must do this first to be able to rewind the currently replayed recording
#if VDRVERSNUM >= 10703
        cResumeFile ResumeFile(ri->FileName(), recording->IsPesRecording());
#else
        cResumeFile ResumeFile(ri->FileName());
#endif
        ResumeFile.Delete();
        return Play();
        }
     }
  return osContinue;
}

eOSState cMenuRecordings::Delete(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && !ri->IsDirectory()) {
     if (userFilter != 0 && ri->User() != 0 && ri->User() != USER_MASK(userFilter)) {
        cRecording *recording = Recordings.GetByName(ri->FileName());
        if (recording) {
	   if (cMenuEditRecording::UpdateUser(recording, ri->User() ^ USER_MASK(userFilter))) {
              cReplayControl::ClearLastReplayed(ri->FileName());
              Recordings.Del(recording);
              Recordings.AddByName(ri->FileName());
              cOsdMenu::Del(Current());
              SetHelpKeys();
              Display();
              if (!Count())
                 return osBack;
	      }
	   }
	else
           Skins.Message(mtError, tr("Error while accessing recording!"));
	}
     else if (Interface->Confirm(tr("Delete recording?"))) {
        cRecordControl *rc = cRecordControls::GetRecordControl(ri->FileName());
        if (rc) {
           if (Interface->Confirm(tr("Timer still recording - really delete?"))) {
              cTimer *timer = rc->Timer();
              if (timer) {
                 timer->Skip();
                 cRecordControls::Process(time(NULL));
                 if (timer->IsSingleEvent()) {
                    isyslog("deleting timer %s", *timer->ToDescr());
                    Timers.Del(timer);
                    }
                 Timers.SetModified();
                 }
              }
           else
              return osContinue;
           }
        cRecording *recording = GetRecording(ri);
        if (recording) {
#if VDRVERSNUM >= 10724
           if (cCutter::Active(ri->FileName())) {
              if (Interface->Confirm(tr("Recording is being edited - really delete?"))) {
                 cCutter::Stop();
                 recording = Recordings.GetByName(ri->FileName()); // cCutter::Stop() might have deleted it if it was the edited version
                 // we continue with the code below even if recording is NULL,
                 // in order to have the menu updated etc.
                 }
              else
                 return osContinue;
              }
#endif
#if VDRVERSNUM >= 10515
           if (cReplayControl::NowReplaying() && strcmp(cReplayControl::NowReplaying(), ri->FileName()) == 0)
              cControl::Shutdown();
#endif
           if (!recording || recording->Delete()) {
              cReplayControl::ClearLastReplayed(ri->FileName());
              Recordings.DelByName(ri->FileName());
              cOsdMenu::Del(Current());
              SetHelpKeys();
#ifdef REMOTETIMERS_DISKSPACE
              SetFreeDiskDisplay(true);
#else
              cVideoDiskUsage::ForceCheck();
#endif
              Display();
              if (!Count())
                 return osBack;
              }
           else
              Skins.Message(mtError, tr("Error while deleting recording!"));
           }
        }
     }
  return osContinue;
}

eOSState cMenuRecordings::Info(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && !ri->IsDirectory()) {
     cRecording *recording = GetRecording(ri);
     if (recording && recording->Info()->Title())
        return AddSubMenu(new cMenuRecording(recording, true));
     }
  return osContinue;
}

eOSState cMenuRecordings::Edit(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && !ri->IsDirectory()) {
     cRecording *recording = GetRecording(ri);
     if (recording) {
        cThreadLock RecordingsLock(&Recordings);
        return AddSubMenu(new cMenuEditRecording(recording));
        }
     }
  return osContinue;
}

/*
eOSState cMenuRecordings::Commands(eKeys Key)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && !ri->IsDirectory()) {
     cRecording *recording = GetRecording(ri);
     if (recording) {
        cMenuCommands *menu;
        eOSState state = AddSubMenu(menu = new cMenuCommands(tr("Recording commands"), &RecordingCommands, cString::sprintf("\"%s\"", *strescape(recording->FileName(), "\\\"$"))));
        if (Key != kNone)
           state = menu->ProcessKey(Key);
        return state;
        }
     }
  return osContinue;
}
*/

eOSState cMenuRecordings::ProcessKey(eKeys Key)
{
  bool HadSubMenu = HasSubMenu();
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kPlay:
       case kOk:     return Play();
       //case kRed:    return (helpKeys > 1 && RecordingCommands.Count()) ? Commands() : Play();
       case kRed:    return (helpKeys > 1) ? Edit() : Play();
       case kGreen:  return Rewind();
       case kYellow: return Delete();
       case kInfo:
       case kBlue:   return Info();
       //case k1...k9: return Commands(Key);
       case k0 ... k9:
                     userFilter = Key - k0;
		     if (RemoteTimersSetup.userFilterRecordings < 0 && ::Setup.ResumeID != Key - k0) {
		        ::Setup.ResumeID = Key - k0;
			Recordings.ResetResume();
			}
                     Set(true);
                     SetHelpKeys();
                     return osContinue;
       case kNone:   if (Recordings.StateChanged(recordingsState))
                        Set(true);
                     break;
       default: break;
       }
     }
  if (Key == kYellow && HadSubMenu && !HasSubMenu()) {
     // the last recording in a subdirectory was deleted, so let's go back up
     cOsdMenu::Del(Current());
     if (!Count())
        return osBack;
     Display();
     }
  if (!HasSubMenu()) {
     if (HadSubMenu)
	Set(true);
     if (!Count())
        return osBack;
     if (Key != kNone)
        SetHelpKeys();
     }
  return state;
}

// --- cMenuMain -------------------------------------------------------------

int cMenuMain::count = 0;

cMenuMain::cMenuMain(const char *Title, eOSState State)
:cOsdMenu(Title)
{
#if VDRVERSNUM >= 10728
  SetMenuCategory(mcPlugin);
#endif
  count++;
  SetHasHotkeys();
  Add(new cOsdItem(hk(tr("Schedule")),   osUser1));
  Add(new cOsdItem(hk(tr("Timers")),     osUser2));
  Add(new cOsdItem(hk(tr("Recordings")), osUser3));
  if (cMoveRec::IsMoving())
     Add(new cOsdItem(hk(trREMOTETIMERS("Abort moving recording")), osUser4));
  if (State == osRecordings)
     AddSubMenu(new cMenuRecordings(NULL, 0, true));
}

cMenuMain::~cMenuMain()
{
  count--;
}

eOSState cMenuMain::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  switch (state) {
    case osUser1:      return AddSubMenu(new cMenuSchedule);
    case osUser2:      return AddSubMenu(new cMenuTimers);
    case osUser3:      return AddSubMenu(new cMenuRecordings);
#if VDRVERSNUM >= 10404
    case osUser4:      cMoveRec::Abort(-1); return osBack;
#else
    case osUser4:      cMoveRec::Abort(10); return osBack;
#endif
    default:           return state;
    }
}

}; // namespace PluginRemoteTimers
