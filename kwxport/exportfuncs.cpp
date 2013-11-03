#include "kwxport.h"
#include "resource.h"

#include <dbghelp.h>
#include <hold.h>
#include <quat.h>
#include <IDxMaterial.h>
#include <IPathConfigMgr.h>
#include <iFnPub.h>
#include <notify.h>
#include <guplib.h>
#include <gup.h>
#include <sstream>

#include "kwlog.h"
#include "exportmain.h"
#pragma comment(lib, "gup.lib")

#define KWX_FN_INTERFACE Interface_ID(0x69ae66aa, 0x357b4bac)
#define KWX_GUP_CLASS Class_ID(0x69ae66aa, 0x357b4bad)

enum {
  kwx_count_settings, kwx_get_setting_names, kwx_get_setting_values, kwx_get_setting, kwx_set_setting,
  kwx_count_animations, kwx_get_animation_names, kwx_get_animation_starts, kwx_get_animation_lengths, kwx_get_animation_scales, kwx_set_animation, kwx_remove_animation,
  kwx_export, kwx_show_settings, kwx_load_settings, kwx_save_settings,
};

class KWFunctions : public FPStaticInterface
{
  public:
    virtual int CountSettings() = 0;
    virtual TCHAR const *GetSettingName(int ix) = 0;
    virtual TCHAR const *GetSettingValue(int ix) = 0;
    virtual TCHAR const *GetSetting(char const *name) = 0;
    virtual void SetSetting(char const *name, char const *value) = 0;

    virtual int CountAnimations() = 0;
    virtual TCHAR const *GetAnimationName(int ix) = 0;
    virtual int GetAnimationStart(int ix) = 0;
    virtual int GetAnimationLength(int ix) = 0;
    virtual float GetAnimationScale(int ix) = 0;
    virtual void SetAnimation(char const *name, int start, int length, float scale) = 0;
    virtual void RemoveAnimation(char const *name) = 0;

    virtual void Export(char const *path) = 0;
    virtual void ShowSettings() = 0;
    virtual bool LoadSettings(char const *path, int includeAnimations) = 0;
    virtual bool SaveSettings(char const *path) = 0;
};

class KWFunctionsImp : public GUP, public KWFunctions
{
  public:

    DWORD Start();
    void Stop() { return; }
    IOResult Save(ISave *save) { return IO_OK; }
    IOResult Load(ILoad *load) { return IO_OK; }
    void DeleteThis() {}

    DECLARE_DESCRIPTOR(KWFunctionsImp)

    BEGIN_FUNCTION_MAP

      FN_0(kwx_count_settings, TYPE_INT, CountSettings)
      FN_1(kwx_get_setting_names, TYPE_STRING, GetSettingName, TYPE_INT)
      FN_1(kwx_get_setting_values, TYPE_STRING, GetSettingValue, TYPE_INT)
      FN_1(kwx_get_setting, TYPE_STRING, GetSetting, TYPE_STRING)
      VFN_2(kwx_set_setting, SetSetting, TYPE_STRING, TYPE_STRING)

      FN_0(kwx_count_animations, TYPE_INT, CountAnimations)
      FN_1(kwx_get_animation_names, TYPE_STRING, GetAnimationName, TYPE_INT)
      FN_1(kwx_get_animation_starts, TYPE_INT, GetAnimationStart, TYPE_INT)
      FN_1(kwx_get_animation_lengths, TYPE_INT, GetAnimationLength, TYPE_INT)
      FN_1(kwx_get_animation_scales, TYPE_FLOAT_TAB, GetAnimationScale, TYPE_INT)
      VFN_4(kwx_set_animation, SetAnimation, TYPE_STRING, TYPE_INT, TYPE_INT, TYPE_FLOAT)
      VFN_1(kwx_remove_animation, RemoveAnimation, TYPE_STRING)

      VFN_1(kwx_export, Export, TYPE_STRING)
      VFN_0(kwx_show_settings, ShowSettings)
      FN_2(kwx_load_settings, TYPE_BOOL, LoadSettings, TYPE_STRING, TYPE_BOOL)
      FN_1(kwx_save_settings, TYPE_BOOL, SaveSettings, TYPE_STRING)

    END_FUNCTION_MAP


    virtual int CountSettings();
    virtual TCHAR const *GetSettingName(int ix);
    virtual TCHAR const *GetSettingValue(int ix);
    virtual TCHAR const *GetSetting(char const *name);
    virtual void SetSetting(char const *name, char const *value);

    virtual int CountAnimations();
    virtual TCHAR const *GetAnimationName(int ix);
    virtual int GetAnimationStart(int ix);
    virtual int GetAnimationLength(int ix);
    virtual float GetAnimationScale(int ix);
    virtual void SetAnimation(char const *name, int start, int length, float scale);
    virtual void RemoveAnimation(char const *name);

    virtual void Export(char const *path);
    virtual void ShowSettings();
    virtual bool LoadSettings(char const *path, int includeAnimation);
    virtual bool SaveSettings(char const *path);

    //  commit settings to document properties and external defaults
    void CommitMySettings();
    void ParseAnimations();
    static void PostNew(void *param, NotifyInfo *info);
    static void PostOpen(void *param, NotifyInfo *info);
    static void PostSave(void *param, NotifyInfo *info);
    
    bool dirtied;
    Settings mySettings;
    std::vector<AnimationRange> myAnimations;
};

KWFunctionsImp funcs(
    KWX_FN_INTERFACE,
    _T("KWFunctions"), IDS_KWFUNCTIONS, NULL, FP_STATIC_METHODS + FP_CORE,
    kwx_count_settings, _T("CountSettings"), IDS_COUNTSETTINGS, TYPE_INT, 0, 0,
    kwx_get_setting_names, _T("GetSettingName"), IDS_GETSETTINGNAMES, TYPE_STRING, 0, 1,
      _T("index"), IDS_INDEX, TYPE_INT,
    kwx_get_setting_values, _T("GetSettingValue"), IDS_GETSETTINGVALUES, TYPE_STRING, 0, 1,
      _T("index"), IDS_INDEX, TYPE_INT,
    kwx_get_setting, _T("GetSetting"), IDS_GETSETTING, TYPE_STRING, 0, 1,
      _T("setting"), IDS_SETTING, TYPE_STRING,
    kwx_set_setting, _T("SetSetting"), IDS_SETSETTING, TYPE_VOID, 0, 2,
      _T("setting"), IDS_SETTING, TYPE_STRING,
      _T("value"), IDS_VALUE, TYPE_STRING,
    kwx_count_animations, _T("CountAnimations"), IDS_COUNTANIMATIONS, TYPE_INT, 0, 0,
    kwx_get_animation_names, _T("GetAnimationName"), IDS_GETANIMATIONNAMES, TYPE_STRING, 0, 1,
      _T("index"), IDS_INDEX, TYPE_INT,
    kwx_get_animation_starts, _T("GetAnimationStart"), IDS_GETANIMATIONSTARTS, TYPE_INT, 0, 1,
      _T("index"), IDS_INDEX, TYPE_INT,
    kwx_get_animation_lengths, _T("GetAnimationLength"), IDS_GETANIMATIONLENGTHS, TYPE_INT, 0, 1,
      _T("index"), IDS_INDEX, TYPE_INT,
    kwx_get_animation_scales, _T("GetAnimationScale"), IDS_GETANIMATIONSCALES, TYPE_FLOAT, 0, 1,
      _T("index"), IDS_INDEX, TYPE_INT,
    kwx_set_animation, _T("SetAnimation"), IDS_SETANIMATION, TYPE_VOID, 0, 4,
      _T("name"), IDS_NAME, TYPE_STRING,
      _T("start"), IDS_START, TYPE_INT,
      _T("length"), IDS_LENGTH, TYPE_INT,
      _T("scale"), IDS_SCALE, TYPE_FLOAT,
    kwx_remove_animation, _T("RemoveAnimation"), IDS_REMOVEANIMATION, TYPE_VOID, 0, 1,
      _T("name"), IDS_NAME, TYPE_STRING,
    kwx_export, _T("Export"), IDS_EXPORT, TYPE_VOID, 0, 1,
      _T("filename"), IDS_FILENAME, TYPE_STRING,
    kwx_show_settings, _T("ShowSettings"), IDS_SHOWSETTINGS, TYPE_VOID, 0, 0,
    kwx_load_settings, _T("LoadSettings"), IDS_LOADSETTINGS, TYPE_BOOL, 0, 2,
      _T("filename"), IDS_FILENAME, TYPE_STRING,
      _T("animations"), IDS_INCLUDEANIMATIONS, TYPE_BOOL,
    kwx_save_settings, _T("SaveSettings"), IDS_SAVESETTINGS, TYPE_BOOL, 0, 1,
      _T("filename"), IDS_FILENAME, TYPE_STRING,
    end
);

FPInterface *GetKWInterface()
{
  return static_cast<FPInterface *>(&funcs);
}

GUP *GetKWGUP()
{
  return static_cast<GUP *>(&funcs);
}

class KWGUPClassDesc2 : public ClassDesc2
{
  public:
    KWGUPClassDesc2() { AddInterface(&funcs); }
    int IsPublic() { return TRUE; }
    void  *Create(BOOL loading) { return GetKWGUP(); }
    char const *ClassName() { return _T("KWInterface"); }
    SClass_ID SuperClassID() { return GUP_CLASS_ID; }
    Class_ID ClassID() { return KWX_GUP_CLASS; }
    char const *Category() { return _T("Export"); }
    HINSTANCE HInstance() { return hInstance; }
};

KWGUPClassDesc2 kWGUPDesc;

ClassDesc2 *GetKWInterfaceDesc()
{
  return &kWGUPDesc;
}




//  Fool the linker into pulling in this entire file.
__declspec(dllexport) FPStaticInterface *GetFPStatic()
{
  return &funcs;
}


class CountVisitor
{
  public:
    CountVisitor() { count = 0; }
    int count;
    template<typename T> bool operator()(char const *, T & b, char const * name, T, char const *, int)
      {
        ++count;
        return true;
      }
};

DWORD KWFunctionsImp::Start()
{
  dirtied = false;
  DoLoadSettings(mySettings, GetCOREInterface());
  RegisterNotification(PostNew, this, NOTIFY_SYSTEM_POST_NEW);
  RegisterNotification(PostOpen, this, NOTIFY_FILE_POST_OPEN);
  RegisterNotification(PostSave, this, NOTIFY_FILE_POST_SAVE);
  return GUPRESULT_KEEP;
}

int KWFunctionsImp::CountSettings()
{
  CountVisitor cv;
  Visit(mySettings, cv);
  return cv.count;
}

int KWFunctionsImp::CountAnimations()
{
  return (int)funcs.myAnimations.size();
}



static std::string lastStringRet;

class NameVisitor
{
  public:
    NameVisitor(int i) { count = 0; id = i; }
    int count, id;
    template<typename T> bool operator()(char const *, T & b, char const * name, T, char const *, int)
      {
        if (count == id)
        {
          lastStringRet = name;
          return false;
        }
        ++count;
        return true;
      }
};

TCHAR const *KWFunctionsImp::GetSettingName(int ix)
{
  NameVisitor nv(ix);
  Visit(mySettings, nv);
  return lastStringRet.c_str();
}

template<typename T> void mk_string(std::string & s, T const &t)
{
  std::stringstream ss;
  ss << t;
  s = ss.str();
}

class ValueVisitor
{
  public:
    ValueVisitor(int i) { count = 0; id = i; }
    int count, id;
    template<typename T> bool operator()(char const *, T & b, char const * name, T, char const *, int)
      {
        if (count == id)
        {
          mk_string(lastStringRet, b);
          return false;
        }
        ++count;
        return true;
      }
};

TCHAR const *KWFunctionsImp::GetSettingValue(int ix)
{
  ValueVisitor vv(ix);
  Visit(mySettings, vv);
  return lastStringRet.c_str();
}

class ValueVisitor2
{
  public:
    ValueVisitor2(char const *i) { id = i; }
    char const *id;
    template<typename T> bool operator()(char const *, T & b, char const * name, T, char const *, int)
      {
        if (!stricmp(name, id))
        {
          mk_string(lastStringRet, b);
          return false;
        }
        return true;
      }
};

TCHAR const *KWFunctionsImp::GetSetting(char const *name)
{
  ValueVisitor2 vv2(name);
  Visit(mySettings, vv2);
  return lastStringRet.c_str();
}

template<typename T> void de_string(std::string &val, T &t)
{
  std::stringstream ss(val);
  ss >> t;
}

class SetValueVisitor
{
  public:
    SetValueVisitor(char const *i, char const *v) { id = i; val = v; }
    char const *id;
    std::string val;
    template<typename T> bool operator()(char const *, T & b, char const * name, T, char const *, int)
      {
        if (!stricmp(name, id))
        {
          de_string(val, b);
          return false;
        }
        return true;
      }
};

void KWFunctionsImp::SetSetting(char const *name, char const *value)
{
  SetValueVisitor svv(name, value);
  Visit(mySettings, svv);
  CommitMySettings();
}

TCHAR const *KWFunctionsImp::GetAnimationName(int ix)
{
  if ((size_t)ix >= myAnimations.size())
    return 0;
  return myAnimations[ix].name.c_str();
}

int KWFunctionsImp::GetAnimationStart(int ix)
{
  if ((size_t)ix >= myAnimations.size())
    return -1;
  return myAnimations[ix].first;
}

int KWFunctionsImp::GetAnimationLength(int ix)
{
  if ((size_t)ix >= myAnimations.size())
    return -1;
  return myAnimations[ix].length;
}

float KWFunctionsImp::GetAnimationScale(int ix)
{
  if ((size_t)ix >= myAnimations.size())
    return 0;
  return myAnimations[ix].timeStretch;
}

void KWFunctionsImp::SetAnimation(char const *name, int start, int length, float scale)
{
  PutAnimationRanges(myAnimations, mySettings.allAnimations_);
  CommitMySettings();
}

void KWFunctionsImp::RemoveAnimation(char const *name)
{
  PutAnimationRanges(myAnimations, mySettings.allAnimations_);
  CommitMySettings();
}

void KWFunctionsImp::Export(char const *path)
{
  std::auto_ptr<IGameExporter> gx(new IGameExporter());
  gx->DoExport(path, 0, 0, true, 0);
  CommitMySettings();
}

void KWFunctionsImp::ShowSettings()
{
  std::auto_ptr<IGameExporter> gx(new IGameExporter());
  *(Settings *)gx.get() = mySettings;
  if (::DialogBoxParam(hInstance, MAKEINTRESOURCE(IDD_SETTINGS), 0, SettingsProc, 
      (LPARAM)gx.get())) {
    return;
  }
  CommitMySettings();
}

bool KWFunctionsImp::LoadSettings(char const *pathname, int includeAnimations)
{
  Settings s = mySettings;
  if (!::DoLoadSettings(s, pathname))
  {
    return false;
  }
  std::string aa = mySettings.allAnimations_;
  mySettings = s;
  if (!includeAnimations)
  {
    mySettings.allAnimations_ = aa;
  }
  ParseAnimations();
  CommitMySettings();
  return true;
}

bool KWFunctionsImp::SaveSettings(char const *pathname)
{
  PutAnimationRanges(myAnimations, mySettings.allAnimations_);
  return ::DoSaveSettings(mySettings, pathname);
}

void KWFunctionsImp::PostNew(void *param, NotifyInfo *info)
{
  DoLoadSettings(((KWFunctionsImp *)param)->mySettings, GetCOREInterface());
  ((KWFunctionsImp *)param)->ParseAnimations();
}

void KWFunctionsImp::PostOpen(void *param, NotifyInfo *info)
{
  DoLoadSettings(((KWFunctionsImp *)param)->mySettings, GetCOREInterface());
  ((KWFunctionsImp *)param)->ParseAnimations();
}

void KWFunctionsImp::PostSave(void *param, NotifyInfo *info)
{
  ((KWFunctionsImp *)param)->dirtied = false;
}

void KWFunctionsImp::CommitMySettings()
{
  if (!dirtied)
  {
    dirtied = true;
    theHold.Begin();
    theHold.Put(new NullRestoreObj());
    theHold.Accept(_T("Export Settings"));
  }
  PutAnimationRanges(myAnimations, mySettings.allAnimations_);
  DoSaveSettings(mySettings, GetCOREInterface());
}

void KWFunctionsImp::ParseAnimations()
{
  myAnimations.clear();
  GetAnimationRanges(mySettings.allAnimations_, myAnimations);
}

Settings &GetMySettings()
{
  return funcs.mySettings;
}

void SetMySettings(Settings &s)
{
  funcs.mySettings = s;
  funcs.ParseAnimations();
  funcs.CommitMySettings();
}
