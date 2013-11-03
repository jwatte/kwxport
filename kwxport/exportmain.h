
#if !defined(kwxport_exportmain_h)
#define kwxport_exportmain_h


#include <set>
#include <string>

struct AnimationRange;
struct Settings;
struct FatVert;

extern HMODULE dbgHelp;
extern BOOL (WINAPI *WriteDumpFunc)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, 
    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
extern void GetDbgHelp();
extern DWORD MiniDumpHandler(LPEXCEPTION_POINTERS lpe);
extern std::string SettingsFilename(Interface *core);
extern void DoLoadSettings(Settings &set, Interface *core);
extern bool DoLoadSettings(Settings &set, char const *filename);
extern void DoSaveSettings(Settings &set, Interface *core, char const *exportTime = NULL);
extern bool DoSaveSettings(Settings &set, char const *filename, Interface *core = NULL);
extern void GetAnimationRanges(std::string const & b, std::vector<AnimationRange> & cont);
extern void PutAnimationRanges(std::vector<AnimationRange> const & cont, std::string & b);
extern INT_PTR CALLBACK SettingsProc(HWND hWndDlg, UINT msg, WPARAM wParam, LPARAM lParam);
extern Settings &GetMySettings();
extern void SetMySettings(Settings &s);

#define IDB_ADD 39001
#define IDB_UPDATE 39002
#define IDB_REMOVE 39003
#define IDC_ANIMATIONS 39004
#define IDC_FIRSTFRAME 39005
#define IDC_NUMFRAMES 39006
#define IDC_STRETCHTIME 39007
#define IDC_ANIMATIONNAME 39008
#define IDC_STATICFRAME 39009

#define ANIMATIONS_LIST_VIEW "allAnimations"

struct AnimationRange {
  std::string name;
  int first;
  int length;
  float timeStretch;
};


struct Settings {
  Settings();
  bool exportNormal_;
  bool exportTangent_;
  bool exportColor_;
  bool flipV_;
  bool flipTangent_;
  bool yUp_;
  bool rightHanded_;
  bool flipWinding_;
  int exportUv_;
  float scaleNumer_;
  float scaleDenom_;
  bool exportSkinning_;
  bool exportBinary_;
  bool exportCompressed_;
  bool exportMaterial_;
  bool fullPath_;
  bool renameTex_;
  std::string prefixTex_;
  bool copyTex_;
  bool ignoreUntextured_;
  bool exportAnimation_;
  bool exportComments_;
  bool exportHidden_;
  int firstFrame_;
  int numFrames_;
  float timeStretch_;
  std::string animationName_;
  std::string allAnimations_;
};
//  Users of visitor may rely on the fact that the same string 
//  constant will always be passed as the "name" parameter (the 
//  third parameter).
template<typename Visitor> bool Visit(Settings & s, Visitor & v)
{
  return
      v("Geometry", s.exportNormal_, "normals", true, "Export Normals", 1) &&
      v("Geometry", s.exportTangent_, "tangents", true, "Export Tangents", 2) &&
      v("Geometry", s.exportColor_, "colors", false, "Colors as Diffuse", 3) &&
      v("Geometry", s.exportUv_, "uvChannels", 2, "Num UV Channels", 4) &&
      v("Geometry", s.flipV_, "flipV", false, "Flip V Channel", 9) &&
      v("Geometry", s.flipTangent_, "flipTangent", false, "Flip Tangent Channel", 14) &&
      v("Geometry", s.yUp_, "yUp", true, "Make Y Up", 10) &&
      v("Geometry", s.rightHanded_, "rightHanded", true, "Export Right-handed Mesh", 11) &&
      v("Geometry", s.flipWinding_, "flipWinding", true, "Flip Winding", 13) &&
      v("Geometry", s.scaleNumer_, "scaleNumer", 1.0f, "Scale Numerator", 5) &&
      v("Geometry", s.scaleDenom_, "scaleDenom", 40.0f, "Scale Denominator", 6) &&
      v("Geometry", s.exportSkinning_, "exportSkinning", false, "Export Skinning", 7) &&
      v("Materials", s.exportMaterial_, "materials", true, "Export Materials", 20) &&
      v("Materials", s.fullPath_, "fullPath", false, "Full Texture Path", 21) &&
      v("Materials", s.renameTex_, "renameDds", false, "Rename to DDS", 22) &&
      v("Materials", s.prefixTex_, "prefixTex", std::string(""), "Prefix", 23) &&
      v("Materials", s.copyTex_, "copyTex", true, "Copy Textures", 26) &&
      v("Materials", s.ignoreUntextured_, "ignoreUntextured", false, "Ignore Untextured", 24) &&
      v("Animation", s.exportAnimation_, "exportAnimation", false, "Export Animation", 40) &&
      v("Animation", s.firstFrame_, "firstFrame", 0, "First Frame", IDC_FIRSTFRAME) &&
      v("Animation", s.numFrames_, "numFrames", 100, "Num. Frames", IDC_NUMFRAMES) &&
      v("Animation", s.timeStretch_, "timeStretch", 1.0f, "Stretch Time", IDC_STRETCHTIME) &&
      v("Animation", s.animationName_, "animationName", std::string("Idle"), "Animation Name", IDC_ANIMATIONNAME) &&
      v("Animation", s.allAnimations_, ANIMATIONS_LIST_VIEW, std::string(""), "Animations", IDC_ANIMATIONS) &&
      v("Misc", s.exportComments_, "comments", false, "Export Comments", 60) &&
      v("Misc", s.exportHidden_, "hidden", false, "Export Hidden", 61) &&
      v("Misc", s.exportCompressed_, "exportCompressed", false, "Export Compressed", 12) &&
      v("Misc", s.exportBinary_, "exportBinary", false, "Export Binary", 8) &&
      true;
}

struct SetDefaults {
  template<typename T>
  bool operator()(char const * category, T & b, char const * name, T value, char const * desc, int id) {
    b = value;
    return true;
  }
};

struct AssertUniqueIds {
  std::set<int> ids_;
  template<typename T>
  bool operator()(char const *category, T &b, char const *name, T value, char const *desc, int id) {
    if (ids_.find(id) != ids_.end()) {
      ::LogPrint("Setting ID %d is re-used for settings %s.\n", id, name);
      assert(false);
    }
    else {
      ids_.insert(id);
    }
    return true;
  }
};

class IGameExporter : public SceneExport, public Settings {
    public:

    std::vector<IGameNode *> animated_;
        IGameScene * igame_;
        Interface * core_;
        std::string name_;
    std::map<std::string, std::string> textureCopy_;
        char exportTime_[100];
        TimeValue coreFrame_;

        int                ExtCount();                    // Number of extensions supported
        const TCHAR *    Ext(int n);                    // Extension #n (i.e. "3DS")
        const TCHAR *    LongDesc();                    // Long ASCII description (i.e. "Autodesk 3D Studio File")
        const TCHAR *    ShortDesc();                // Short ASCII description (i.e. "3D Studio")
        const TCHAR *    AuthorName();                // ASCII Author name
        const TCHAR *    CopyrightMessage();            // ASCII Copyright message
        const TCHAR *    OtherMessage1();            // Other message #1
        const TCHAR *    OtherMessage2();            // Other message #2
        unsigned int    Version();                    // Version number * 100 (i.e. v3.01 = 301)
        void            ShowAbout(HWND hWnd);        // Show DLL's "About..." box

        BOOL SupportsOptions(int ext, DWORD options);
        int    DoExport(const TCHAR *name,ExpInterface *ei,Interface *i, BOOL suppressPrompts=FALSE, DWORD options=0);
        int    DoExport2(const TCHAR *name, BOOL suppressPrompts=FALSE, DWORD options=0);

    void WriteXFile(IGameScene * ig, TCHAR const * name);
    template<typename T> void ExportNode(IGameScene * ig, IGameNode * node, T * root, bool selected);
    void WriteGeometry(IGameScene * ig, IGameNode * node, IGameObject * obj, ID3DXFileSaveData * frame);
    void WriteGeometry2(IGameScene * ig, IGameNode * node, IGameObject * obj, ID3DXFileSaveData * frame);
    static DWORD VertexAdd(std::vector<FatVert> & vec, std::vector<std::pair<float, DWORD> > & acc, FatVert const & fv);
    void ExportAnimations(ID3DXFileSaveObject * save);
    void ExportFileData(ID3DXFileSaveObject * save);
    template<typename T> void AddKeyValue(T * cont, char const * key, char const * value);

    void LoadSettings();
    void SaveSettings();

        IGameExporter();
        ~IGameExporter();        

};

//  This object doesn't actually support undo -- it just 
//  implements the interface to be able to provoke the
//  "file dirty" bit of the scene file, when settings 
//  change during export.
//  Some time in the future, perhaps real undo/redo would 
//  be supported -- would only take two hours to implement.
class NullRestoreObj : public RestoreObj {
  public:
    NullRestoreObj() {}
    virtual void Restore(int) {}
    virtual void Redo() {}
    virtual int Size() { return 10; }
};


#endif  //  kwxport_exportmain_h
