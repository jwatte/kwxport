#include "kwxport.h"
#include "resource.h"

#include <d3dx9xof.h>
#include "rmxftmpl.h"
#include "rmxfguid.h"
#include <d3dx9mesh.h>

#include <dbghelp.h>
#include <plugapi.h>
#include <hold.h>
#include <quat.h>
#include <IDxMaterial.h>
#include <IPathConfigMgr.h>
#include <crtdbg.h>

#include <set>
#include <algorithm>
#include <sstream>

#include "kwlog.h"
#include "exportmain.h"

#define USE_FILE_RESOLUTION 0
#define IGAME_BUG 0
#if defined(MAX_RELEASE)
#if defined(MAX_RELEASE_R12_ALPHA) && MAX_RELEASE >= MAX_RELEASE_R12_ALPHA
#undef USE_FILE_RESOLUTION
#define USE_FILE_RESOLUTION 1
#include <AssetManagement/AssetUser.h>
#include <IFileResolutionManager.h>
#pragma comment(lib, "assetmanagement.lib")
#if defined(_M_X64)
#undef IGAME_BUG
#if !defined(MAX_RELEASE_R13) || MAX_RELEASE < MAX_RELEASE_R13
#define IGAME_BUG 1
#else
#define IGAME_BUG 0
#endif
#endif
#endif
#else
#error "MAX_RELEASE must be defined!"
#endif

//TODO: for 1.5, support sorting transparent material triangles and splitting 2-sided triangles
//TODO: for 1.5, convert textures to DDS when "rename to dds" and "copy textures" are on
//TODO: for 1.5, convert the settings interface to use real ParamBlk2, and expose a Utility 
//        that allows editing it.

//BUG:  There appears to be a pivot center offset problem
//FIXED:  There appears to be some problem with non-contiguous texture map channels

#if 0 //  todo: remember to set to 0 before ship!
#define OPTIMIZED 0
#else
#define OPTIMIZED 1
#endif

#if !OPTIMIZED
#pragma optimize("", off)
#endif

#pragma comment(lib, "bmm.lib")
//#pragma warning(disable: 4312) // cast int/pointer

#define MAX_UV_SETS 4

static int const labelWidth = 110;
static int const rowHeight = 18;
static int const numCols = 1;
static bool inited = false;


typedef std::string string;


HMODULE dbgHelp;
BOOL (WINAPI *WriteDumpFunc)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, 
    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

float zMultiply = 1.0f;
float scale = 1.0f;
float yUp = false;
bool rightHanded = false;
bool flipWinding = false;

bool CheckBad(float &f) {
  //  these tests weed out NaNs and the like
  if (f > 1.0 && f < 0.9) return true;
  if (!(f > 0.9) && !(f < 1.0)) return true;
  //  test for too large numbers (and inf)
  if (f > 1e30) return true;
  if (f < -1e30) return true;
  //  nuke denormals
  if (::fabsf(f) < 1e-30) f = 0;
  //  OK, the number is acceptable
  return false;
}

//  Note: don't apply scale. This function is also 
//  used for normals. Could potentially split it into 2.
void AdjustPoint(Point3 &p)
{
  if (CheckBad(p.x) || CheckBad(p.y) || CheckBad(p.z)) {
    LogPrint("Bad point in scene: %.3f,%.3f,%.3f\n", p.x, p.y, p.z);
    p.x = 1; p.y = 0; p.z = 0;
  }
  if (rightHanded) {
    p.y = -p.y;
  }
  if (yUp) {
    float y = p.y;
    p.y = p.z;
    p.z = -y;
  }
}

GMatrix YtoZ;
GMatrix ZtoY;
class InitMat {
  public:
    InitMat() {
      YtoZ.SetRow(0, Point4(1, 0, 0, 0));
      YtoZ.SetRow(1, Point4(0, 0, 1, 0));
      YtoZ.SetRow(2, Point4(0, -1, 0, 0));
      YtoZ.SetRow(3, Point4(0, 0, 0, 1));
      ZtoY = YtoZ.Inverse();
    }
};
InitMat theMatInit;

void AdjustMatrix(GMatrix &m)
{
  if (rightHanded) {
    //  I mirror Y in Max space (Y == out)
    m.SetColumn(1, m.GetColumn(1) * -1);
    m.SetRow(1, m.GetRow(1) * -1);
  }
  if (yUp) {
    m = YtoZ * m * ZtoY;
  }
  Point3 x = m.Translation() * scale;
  m.SetRow(3, Point4(x, 1));
}

void MergeDoubleSlashes(std::string &str)
{
    std::string::size_type off;
    //  first, turn forward slashes to backward
    while ((off = str.find("/")) != std::string::npos) {
        str.replace(off, 2, "\\");
    }
    //  second, turn double backward slashes to single
    while ((off = str.find("\\\\")) != std::string::npos) {
        str.replace(off, 2, "\\");
    }
}

#if USE_FILE_RESOLUTION
std::string GetFullPath(char const *path)
{
  std::string texPath(IFileResolutionManager::GetInstance()->GetFullFilePath(
      path, MaxSDK::AssetManagement::kBitmapAsset));
  if (texPath.empty())
    texPath = path;
  MergeDoubleSlashes(texPath);
  return texPath;
}
#else
std::string GetFullPath(char const *path)
{
  std::string texPath(IPathConfigMgr::GetPathConfigMgr()->GetFullFilePath(path, false));
  MergeDoubleSlashes(texPath);
  return texPath;
}
#endif

std::string MakeRelativePath(std::string const &input, std::string const &prefix)
{
  char const *outputName = strrchr(input.c_str(), '\\');
  if (!outputName) outputName = strrchr(input.c_str(), '/');
  if (!outputName) outputName = input.c_str(); else ++outputName;
  if (*outputName) {
    return prefix + outputName;
  }
  return input;
}

//  Copy the source file to the dst file name, if there is not 
//  already a file at dst that is the same size with the same mod time.
void CopyTexture(char const *src, char const *dst)
{
  HANDLE srcFile, dstFile;
  srcFile = ::CreateFile(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
  if (srcFile == INVALID_HANDLE_VALUE) {
    LogError("Can't open the file %s for copying.\n", src);
    return;
  }

  DWORD lo, hi;
  lo = ::GetFileSize(srcFile, &hi);
  if (hi != 0) {
    LogError("File %s is too big to copy!\n", src);
    ::CloseHandle(srcFile);
    return;
  }

  FILETIME writeTimeSrc, writeTimeDst;
  ::GetFileTime(srcFile, NULL, NULL, &writeTimeSrc);
  dstFile = ::CreateFile(dst, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
  bool exists = true;
  bool sameTime = false;
  bool error = false;
  if (dstFile == INVALID_HANDLE_VALUE) {
    int err = ::GetLastError();
    switch (err) {
      case ERROR_PATH_NOT_FOUND:
      case ERROR_FILE_NOT_FOUND:
        exists = false;
        break;
      default:
        error = true;
        break;
    }
  }
  else {
    ::GetFileTime(dstFile, NULL, NULL, &writeTimeDst);
    sameTime = (writeTimeSrc.dwHighDateTime == writeTimeDst.dwHighDateTime)
        && (writeTimeSrc.dwLowDateTime == writeTimeDst.dwLowDateTime);
    DWORD lo2, hi2;
    lo2 = ::GetFileSize(dstFile, &hi2);
    //  only consider them the same if time & size are both the same
    sameTime = sameTime && (hi2 == 0) && (lo2 == lo);
    ::CloseHandle(dstFile);
  }
  if (error) {
    ::LogError("Could not inspect destination file %s for copying texture.\n", dst);
    ::CloseHandle(srcFile);
    return;
  }
  if (exists && sameTime) {
    LogPrint("File %s has same mod time as %s; not copying.\n", src, dst);
    ::CloseHandle(srcFile);
    return;
  }

  ::LogPrint("Copying file %s to %s.\n", src, dst);
  dstFile = ::CreateFile(dst, GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, 0, 
      CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, 0);
  if (dstFile == INVALID_HANDLE_VALUE) {
    int err = ::GetLastError();
    ::LogError("Could not create destination file %s for copying texture (err = %d).\n", dst, err);
    ::CloseHandle(srcFile);
    return;
  }

  char *buf = new char[128*1024];
  while (lo > 0) {
    DWORD nRead = 0;
    DWORD nWrote = 0;
    DWORD nWrote2 = 0;
    if (!::ReadFile(srcFile, buf, std::min((int)lo, 128*1024), &nRead, 0)) {
      LogError("Error reading from %s while copying to %s: err = %d\n", src, dst, GetLastError());
      break;
    }
    if (nRead == 0) {
      LogError("Zero byte read from %s? Panic!\n", src);
      break;
    }
again:
    if (!::WriteFile(dstFile, buf + nWrote, nRead - nWrote, &nWrote2, 0)) {
      LogError("Error writing to %s while copying from %s: err = %d\n", dst, src, GetLastError());
      break;
    }
    if (nWrote2 == 0) {
      LogError("Zero byte write to %s? Panic!\n", dst);
      break;
    }
    nWrote += nWrote2;
    if (nWrote < nRead)
      goto again;
    lo -= nRead;
  }

  if (!::SetFileTime(dstFile, 0, 0, &writeTimeSrc)) {
    LogError("Could not update last-changed time of %s.\n", dst);
  }

  ::CloseHandle(srcFile);
  ::CloseHandle(dstFile);
}

//  If some structured exception (i e, "crash") happens, then I will 
//  end up inside this handler from the structured exception handler 
//  in the __try / __except block in DoExport().
//  Write a debug dump to c:\kwxport.dmp if possible. This dump can 
//  be used to debug the crash, if I have the symbol files for the 
//  build in question.
DWORD MiniDumpHandler(LPEXCEPTION_POINTERS lpe)
{
  if (WriteDumpFunc != 0) {
    HANDLE file = ::CreateFile("c:\\kwxport.dmp", GENERIC_READ | GENERIC_WRITE, 
        0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION x;
      x.ThreadId = ::GetCurrentThreadId();
      x.ExceptionPointers = lpe;
      x.ClientPointers = false;
      ::OutputDebugString("Writing minidump to c:\\kwxport.dmp\n");
      DWORD procId = ::GetCurrentProcessId();
      HANDLE hProc = ::OpenProcess(PROCESS_ALL_ACCESS, false, procId);
      BOOL b = (*WriteDumpFunc)(hProc, procId, file, 
          MiniDumpNormal, //MiniDumpWithIndirectlyReferencedMemory,
          &x, 0, 0);
      DWORD err = ::GetLastError();
      char buf[128];
      sprintf(buf, "File c:\\kwxport.dmp, result %d, error 0x%08lx\n", b, err);
      ::OutputDebugString(buf);
      ::FlushFileBuffers(file);
      ::CloseHandle(file);
      ::CloseHandle(hProc);
      ::OutputDebugString("Done writing minidump to c:\\kwxport.dmp\n");
      ::LogError("kW X-port crashed; wrote a dump to c:\\kwxport.dmp, result %d, error 0x%08lx\n", b, err);
    }
    else {
      ::OutputDebugString("Could not create minidump file at c:\\kwxport.dmp\n");
      ::LogError("kW X-port crashed; could not create a dump to c:\\kwxport.dmp\n");
    }
  }
  else {
    ::OutputDebugString("Minidump writing is disabled.\n");
    ::LogError("kW X-port crashed; Minidump writing is disabled.\n");
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

void GetAnimationRanges(std::string const & b, std::vector<AnimationRange> & cont)
{
  AnimationRange ar;
  char const * x = b.c_str();
  char * e = (char *)x;
  long l = strtol(x, &e, 10);
  if (e != x) {
    x = e+1;
    for (long q = 0; q < l; ++q) {
      //  what a horrible parsing loop!
      e = (char *)strchr(x, ',');
      if (!e) goto done;
      std::string wt(x, e-x);
      ar.name = wt;
      x = e+1;
      e = (char *)strchr(x, ',');
      if (!e) goto done;
      wt = std::string(x, e);
      ar.first = strtol(wt.c_str(), (char **)&x, 10);
      x = e+1;
      e = (char *)strchr(x, ',');
      if (!e) goto done;
      wt = std::string(x, e);
      ar.length = strtol(wt.c_str(), (char **)&x, 10);
      x = e+1;
      e = (char *)strchr(x, ';');
      if (!e) goto done;
      wt = std::string(x, e);
      ar.timeStretch = (float)strtod(wt.c_str(), (char **)&x);
      x = e+1;
      cont.push_back(ar);
    }
  done:
    ;
  }
}

void PutAnimationRanges(std::vector<AnimationRange> const & cont, std::string & b)
{
  std::stringstream oot;
  oot << cont.size() << ";";
  for (std::vector<AnimationRange>::const_iterator ptr = cont.begin(), end = cont.end();
      ptr != end; ++ptr)
  {
    oot << (*ptr).name << "," << (*ptr).first << "," << (*ptr).length << "," << (*ptr).timeStretch << ";";
  }
  b = oot.str();
}


std::string GetEffectFilename(IDxMaterial *mtl)
{
#if USE_FILE_RESOLUTION
  MaxSDK::AssetManagement::AssetUser const &au = mtl->GetEffectFile();
  return std::string(au.GetFullFilePath());
#else
  char const *fn = mtl->GetEffectFilename();
  return std::string(fn ? fn : "null");
#endif
}

int WindowId(int i)
{
  if (i < 1000) return 40000 + i;
  return i;
}

//  Transform a point (such as a vertex)
static Point3 TransformPoint(GMatrix const & gm, Point3 const & p)
{
  float out[3];
  float const * v = &p.x;
  GRow const * m = gm.GetAddr();
  for (int i = 0; i < 3; ++i) {
    float o = m[3][i];    //  different from TransformVector here
    for (int j = 0; j < 3; ++j) {
      o += m[j][i] * v[j];
    }
    out[i] = o;
  }
  return (Point3 &)out;
}

//  Transform a vector (such as a normal)
static Point3 TransformVector(GMatrix const & gm, Point3 const & p)
{
  float out[3];
  float const * v = &p.x;
  GRow const * m = gm.GetAddr();
  for (int i = 0; i < 3; ++i) {
    float o = 0;        //  different from TransformPoint here
    for (int j = 0; j < 3; ++j) {
      o += m[j][i] * v[j];
    }
    out[i] = o;
  }
  return (Point3 &)out;
}

static string wtocstr(wchar_t const * wstr)
{
  size_t l = wcslen(wstr);
  if (l > 1023) {
    l = 1023;
  }
  char buf[1024], *ptr = buf;
  while (l > 0) {
    *ptr = ((unsigned short)*wstr < 0xff) ? *wstr : '?';
    ++ptr;
    ++wstr;
    --l;
  }
  *ptr = 0;
  return string(buf);
}

#undef assert
#if !defined(NDEBUG)
 #define assert(x) if (x); else assertion_failure(#x, __FILE__, __LINE__)
void assertion_failure(char const * expr, char const * file, int line)
{
  static std::string str;
  str = "Internal error in kW X-porter:\n\n";
  str += expr;
  str += "\n\n";
  str += file;
  str += ":";
  char buf[20];
  _snprintf(buf, 20, "%d", line);
  buf[19] = 0;
  str += buf;
  str += "\n\nStop the export attempt?";
  if (::MessageBox(0, str.c_str(), "Assertion Failure!", MB_YESNO) == IDYES) {
    throw "Assertion failure prevented export";
  }
}
#else
 #define assert(x) (void)0
#endif

HINSTANCE hInstance;

static HRESULT hr;

#define CHECK(x) \
  hr = x; if( FAILED(hr) ) { ThrowHresult(#x,__FILE__,__LINE__,hr); }

static void ThrowHresult(char const * func, char const * file, int line, HRESULT h)
{
  static char buf[4096];
  char const * err = ::DXGetErrorString(h);
  if (!err) {
    ::FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_STRING,
        0, h, 0, (LPSTR)&err, 0, 0);
  }
  sprintf(buf, "Error saving .X file: 0x%08x (%s)\n%s:%d: %s\n", h, err ? err : "Unknown error", 
      file, line, func);
  buf[4095] = 0;
  LogError("%s", buf);
  throw buf;
}


BOOL WINAPI DllMain(HINSTANCE hDllHandle, DWORD nReason, LPVOID Reserved)
{
  switch ( nReason ) {
    case DLL_PROCESS_ATTACH:
      hInstance = hDllHandle;
      break;
    case DLL_PROCESS_DETACH:
      break;
    case DLL_THREAD_ATTACH:
      break;
    case DLL_THREAD_DETACH:
      break;
  }
  return TRUE;
}

ClassDesc2* GetIGameExporterDesc();
ClassDesc2 *GetKWInterfaceDesc();

__declspec( dllexport ) const TCHAR* LibDescription()
{
    return _T("Kilowatt X file exporter");
}

__declspec( dllexport ) int LibNumberClasses()
{
    return 2;
}

__declspec( dllexport ) ClassDesc* LibClassDesc(int i)
{
  if (!inited) {
    ::InitCustomControls(hInstance);    // Initialize MAX's custom controls
    INITCOMMONCONTROLSEX iccx;
    iccx.dwSize = sizeof(iccx);
    iccx.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_USEREX_CLASSES | ICC_LISTVIEW_CLASSES;
    bool initControls = ::InitCommonControlsEx(&iccx) != 0;
    assert(initControls != false);
    inited = true;
  }

    switch(i) {
        case 0: return GetIGameExporterDesc();
        case 1: return GetKWInterfaceDesc();
        default: return 0;
    }
}

__declspec( dllexport ) ULONG LibVersion()
{
    return VERSION_3DSMAX;
}

// Let the plug-in register itself for deferred loading
__declspec( dllexport ) ULONG CanAutoDefer()
{
    return 0;
}



#define KWXPORT_CLASS_ID    Class_ID(0x9387444A, 0xC8476283)

//corresponds to XML schema
TCHAR* mapSlotNames[] = {
        _T("Ambient"),
        _T("Diffuse"),
        _T("Specular"),
        _T("Glossiness"),
        _T("SpecularLevel"),
        _T("SelfIllumination"),
        _T("Opacity"),
        _T("Filter"),
        _T("Bump"),
        _T("Reflection"),
        _T("Refraction"),
        _T("Displacement"),
        _T("Unknown") };



Settings::Settings()
{
  Visit(*this, AssertUniqueIds());
  Visit(*this, SetDefaults());
}

struct FatVert {
  int           origVert;
  Point3        pos;
  Point3        normal;
  Point3        tangent;
  Point3        binormal;
  Point2        uv[MAX_UV_SETS];
  Point4        color;
};


//  This function fixes up degenerate tangent bases that may come 
//  from stretched UV mapping or degenerate faces.
static void FixTangent(FatVert &fv, bool &oFixed)
{
  if (fv.tangent.LengthSquared() < 1e-20f)
  {
    oFixed = true;
    if (fabsf(fv.normal.x) < fabsf(fv.normal.y) && fabsf(fv.normal.x) < fabsf(fv.normal.z))
    {
      //  if X is the smallest, go with it
      fv.tangent = Point3(1, 0, 0);
    }
    else
    {
      //  else go with Y -- we know that X is non-trivial, thus Y is not linearly dependent
      fv.tangent = Point3(0, 1, 0);
    }
  }
}

static void FixBinormal(FatVert &fv, bool &oFixed)
{
  if (fv.binormal.LengthSquared() < 1e-20f)
  {
    oFixed = true;
    fv.binormal = CrossProd(fv.normal, fv.tangent);
  }
}

Tab<INode *> gSelectedNodes;


FPInterface *GetKWInterface();

class IGameExporterClassDesc : public ClassDesc2 {
    public:
    int             IsPublic() { return TRUE; }
    void *            Create(BOOL loading = FALSE)
    {
      return new IGameExporter();
    }
    const TCHAR *    ClassName() { return _T("kwXport"); }
    SClass_ID        SuperClassID() { return SCENE_EXPORT_CLASS_ID; }
    Class_ID        ClassID() { return KWXPORT_CLASS_ID; }
    const TCHAR*     Category() { return _T("Export"); }

    const TCHAR*    InternalName() { return _T("kwXport"); }    // returns fixed parsable name (scripter-visible name)
    HINSTANCE        HInstance() { return hInstance; }                // returns owning module handle

};



static IGameExporterClassDesc IGameExporterDesc;

ClassDesc2* GetIGameExporterDesc()
{
  return &IGameExporterDesc;
}

IGameExporter::IGameExporter()
{
}

IGameExporter::~IGameExporter() 
{
}

int IGameExporter::ExtCount()
{
    return 1;
}

const TCHAR *IGameExporter::Ext(int n)
{        
    return _T("x");
}

const TCHAR *IGameExporter::LongDesc()
{
    return _T("Kilowatt X file exporter.");
}
    
const TCHAR *IGameExporter::ShortDesc() 
{            
    return _T("kW X-port");
}

const TCHAR *IGameExporter::AuthorName()
{            
    return _T("Jon Watte");
}

const TCHAR *IGameExporter::CopyrightMessage() 
{    
    return _T("");
}

const TCHAR *IGameExporter::OtherMessage1() 
{        
    return _T("Use KWFunctions in MaxScript.");
}

const TCHAR *IGameExporter::OtherMessage2() 
{        
    return _T("");
}

unsigned int IGameExporter::Version()
{                
    return 1 * 100 + 4 * 10 + 0;
}

INT_PTR CALLBACK AboutProc(
  HWND hwndDlg,
  UINT uMsg,
  WPARAM wParam,
  LPARAM lParam)
{
  if (uMsg == WM_COMMAND) {
    if (LOWORD(wParam) == IDOK) {
      ::EndDialog(hwndDlg, 0);
    }
    return TRUE;
  }
  if (uMsg == WM_INITDIALOG) {
    return TRUE;
  }
  return FALSE;
}


void IGameExporter::ShowAbout(HWND hWnd)
{
  ::DialogBox(hInstance, MAKEINTRESOURCE(IDD_ABOUT), hWnd, AboutProc);
}

BOOL IGameExporter::SupportsOptions(int ext, DWORD options)
{
    return TRUE;
}



// Dummy function for progress bar
DWORD WINAPI fn(LPVOID arg)
{
    return(0);
}



class MyErrorProc : public IGameErrorCallBack
{
public:
    void ErrorProc(IGameError error)
    {
      TCHAR * buf = GetLastIGameErrorText();
      if (error != 9 && error != 10) {    //  don't care if it says "no skin modifier"
        //  I don't care about missing materials on faces
        LogPrint(_T("IGame: ErrorCode = %d ErrorText = %s\n"), error,buf);
      }
    }
};

static string convert_name(TCHAR const * name)
{
  if (name == 0) {
    return string(_T("unnamed"));
  }
  string ret(name);
  if ((name[0] >= '0') && (name[0] <= '9')) {
    ret = std::string("_") + ret;
  }
  for (size_t i = 0; i < ret.size(); ++i) {
    TCHAR ch = ret[i];
    if (((ch >= '0') && (ch <= '9'))
        || ((ch >= 'A') && (ch <= 'Z'))
        || ((ch >= 'a') && (ch <= 'z'))) {
      continue;
    }
    ret[i] = '_';
  }
  return ret;
}

void append(std::vector<char> & v, int t)
{
  v.insert(v.end(), (char const *)&t, ((char const *)&t)+sizeof(int));
}

void append(std::vector<char> & v, DWORD t)
{
  v.insert(v.end(), (char const *)&t, ((char const *)&t)+sizeof(DWORD));
}

void append(std::vector<char> & v, float t)
{
  v.insert(v.end(), (char const *)&t, ((char const *)&t)+sizeof(float));
}

void append(std::vector<char> & v, Point2 const &t)
{
  v.insert(v.end(), (char const *)&t, ((char const *)&t)+sizeof(Point2));
}

void append(std::vector<char> & v, Point3 const &t)
{
  v.insert(v.end(), (char const *)&t, ((char const *)&t)+sizeof(Point3));
}

void append(std::vector<char> & v, Point4 const &t)
{
  v.insert(v.end(), (char const *)&t, ((char const *)&t)+sizeof(Point4));
}

void append(std::vector<char> & v, GMatrix const &t)
{
  v.insert(v.end(), (char const *)&t, ((char const *)&t)+sizeof(GMatrix));
}

//  do not compile!
template<typename T> void append(std::vector<char> & v, T const *t)
{
  //  If you get an error here, it's because you passed a pointer to something, 
  //  without that something being sized or a string.
  int error[sizeof(T)-100000];
  v.push_back(error);
}


template<typename T>
void append(std::vector<char> & v, std::vector<T> const & t)
{
  if (t.size() > 0) {
    v.insert(v.end(), (char const *)&t[0], ((char const *)&t[0])+sizeof(T)*t.size());
  }
}

template<typename T>
void append(std::vector<char> & v, T const * t, size_t cnt)
{
  v.insert(v.end(), (char const *)t, ((char const *)t)+cnt*sizeof(T));
}

template<> void append(std::vector<char> & v, char const *t)
{
  v.insert(v.end(), t, t+strlen(t)+1);
}

void append(std::vector<char> & v, std::string const & str)
{
  v.insert(v.end(), (char const *)str.c_str(), (char const *)str.c_str()+str.size()+1);
}

DWORD IGameExporter::VertexAdd(std::vector<FatVert> & vec, std::vector<std::pair<float, DWORD> > & acc, FatVert const & fv)
{
  size_t n = vec.size();
  size_t i = 0;
  std::pair<float, DWORD> p(fv.pos.x, 0);
  std::pair<float, DWORD> * base = 0;
  //  If the acceleration list has items in it, then do a binary search for 
  //  a matching X component, to make finding vertices to merge easier.
  //  TODO: This really should be a hash table.
  if (acc.size() > 0) {
    std::pair<float, DWORD> * low = &acc[0], * high = low+acc.size();
    base = low;
    for (;;) {
      std::pair<float, DWORD> * mid = low + (high-low)/2;
      if (mid == low) {
        i = mid-&acc[0];
        break;
      }
      if (mid->first < p.first) {
        low = mid;
      }
      else {
        high = mid;
      }
    }
  }
  for (; i < n; ++i) {
    if (vec[base[i].second].pos.x > fv.pos.x) {
      //  no need searching more
      i = base[i].second;
      break;
    }
    if (!memcmp(&vec[base[i].second], &fv, sizeof(fv))) {
      //  Put a breakpoint here to ensure that I get some vertex sharing.
      //  If I don't at all, then the binary search above is busted.
      return base[i].second;
    }
  }
  p.second = (DWORD)vec.size();
#if !defined(NDEBUG)
  size_t nv = vec.size();
  for (size_t i = 0; i < nv; ++i) {
    assert( memcmp(&fv, &vec[i], sizeof(FatVert)) );
  }
#endif
  //  update the sorted list that's used to accelerate finding shared vertices
  acc.push_back(p);
  std::inplace_merge(&acc[0], &acc[acc.size()-1], &acc[0]+acc.size());
  vec.push_back(fv);
  return p.second;
}

template<typename T>
void appendProperty(IGameProperty * prop, std::vector<char> & out)
{
  float f;
  int i;
  Point3 p3;
  Point4 p4;
  size_t size;

  switch (prop->GetType()) {
    case IGAME_POINT3_PROP:
      prop->GetPropertyValue(p3);
      append(out, p3);
      size = sizeof(p3);
      break;
    case IGAME_POINT4_PROP:
      prop->GetPropertyValue(p4);
      append(out, p4);
      size = sizeof(p4);
      break;
    case IGAME_FLOAT_PROP:
      prop->GetPropertyValue(f);
      append(out, f);
      size = sizeof(f);
      break;
    case IGAME_INT_PROP:
      prop->GetPropertyValue(i);
      append(out, i);
      size = sizeof(i);
      break;
    default:
      ::LogPrint("Unknown property type %d T %s\n", prop->GetType(), typeid(T).name());
      throw "Unknown property kind in appendProperty()\n";
  }
  if (size < sizeof(T)) {
    out.insert(out.end(), sizeof(T)-size, 0);
  }
  else if (size > sizeof(T)) {
    ::LogPrint("Unknown property type %d T %s\n", prop->GetType(), typeid(T).name());
    throw "Too big property in appendProperty()\n";
  }
}

class Remap {
  public:
    Remap() {
    }
    std::vector<std::vector<int> > oldToNew_;
    void SetOldCount(int count) {
        oldToNew_.clear();
        oldToNew_.resize(count);
    }
    void MapOldToNew(int old, int nu) {
        assert(old >= 0 && old < (int)oldToNew_.size());
        oldToNew_[old].push_back(nu);
    }
    template<typename T>
    void ForeachOld(int old, T & func) {
        assert(old >= 0 && old < oldToNew_.size());
        std::vector<int>::iterator ptr = oldToNew_[old].begin(), end = oldToNew_[old].end();
        while (ptr != end) {
            func(*ptr);
            ++ptr;
        }
    }
    int GetCount(int old) {
           if (old < 0 || old >= (int)oldToNew_.size()) return -1;
        return (int)oldToNew_[old].size();
    }
    int GetNew(int old, int i) {
        if (old < 0 || old >= (int)oldToNew_.size()) return -1;
        if (i < 0 || i >= (int)oldToNew_[old].size()) return -1;
        return oldToNew_[old][i];
    }
};

void IGameExporter::WriteGeometry2(IGameScene * ig, IGameNode * node, IGameObject * obj, ID3DXFileSaveData * frame)
{
  __try {
    WriteGeometry(ig, node, obj, frame);
  }
  __except(MiniDumpHandler(GetExceptionInformation())) {
    throw "The exporter crashed during export.\n\n"
        "Please close Max and re-start it before trying to export again.\n"
        "A minidump file called c:\\kwxport.dmp may have been created \n"
        "for further problem analysis.";
  }
}

char const * SafeName(IGameNode * node, char const *prefix)
{
  static char buf[256];
  char const * name = node->GetName();
  if (!name || !name[0]) {
    sprintf(buf, "%s_%lx", prefix, (long)(size_t)node);
    return buf;
  }
  sprintf(buf, "%s_%s", prefix, name);
  for (char *ptr = buf; *ptr; ++ptr) {
    if ((*ptr > 126) || (*ptr < 33) || (*ptr == '/') || (*ptr == '{') || (*ptr == '}') || (*ptr == '\"' || *ptr == '\'' || *ptr == '#')) {
      *ptr = '_';
    }
  }
  return buf;
}

//  This function is at least 10x too long.
void IGameExporter::WriteGeometry(IGameScene * ig, IGameNode * node, IGameObject * obj, ID3DXFileSaveData * frame)
{
  bool exportNormal = exportNormal_;
  bool exportTangent = exportTangent_;
  int exportUv = exportUv_;
  bool flipV = flipV_;
  bool flipTangent = flipTangent_;
  bool exportColor = exportColor_;
  bool exportSkinning = exportSkinning_;
  bool exportMaterial = exportMaterial_;
  bool fullPath = fullPath_;
  bool renameTex = renameTex_;
  bool copyTex = copyTex_;

  bool tangentBad = false;
  bool binormalBad = false;

  //  I don't allow any forward slashes in paths; it's too painful to deal with 
  //  multiple kinds of separators in the generated file.
  MergeDoubleSlashes(prefixTex_);

  //  retrieve the appropriate geometry
  std::vector<char> mesh;
  IGameMesh * gm = static_cast<IGameMesh *>(obj);
  IGameSkin * gs = obj->IsObjectSkinned() ? obj->GetIGameSkin() : 0;
  if (gs && exportSkinning) {
    gm = gs->GetInitialPose();
  }
  gm->SetCreateOptimizedNormalList();
  if (!gm->InitializeData()) {
    static char buf[4096];
    _snprintf(buf, 4095, "Could not read mesh data from mesh '%s'\n",
        SafeName(node, "mesh"));
    buf[4095] = 0;
    throw buf;
  }

  //  When I ask for vertices, the object offset is already 
  //  applied, so always use the node TM.
  GMatrix nwm = node->GetWorldTM(coreFrame_);
  //  inverse transpose of node-to-obj
  if (gs && exportSkinning) {
    //gs->GetInitSkinTM(nwm);
    //  Perhaps GetSkinPose() already includes InitSkinTM()?
    nwm.SetIdentity();
  }
  AdjustMatrix(nwm);
  GMatrix worldToNode = nwm.Inverse();
  GMatrix normalMat = worldToNode;
  

  DWORD numFaces = gm->GetNumberOfFaces();
  DWORD numFacesUsed = 0;
  std::vector<DWORD> indices;
  indices.resize(numFaces * 3);
  FatVert fv;
  std::vector<FatVert> fatVerts;
  std::vector<std::pair<float, DWORD> > accel;

  //  don't export texture coordinates that aren't available
  Tab<int> mapNums = gm->GetActiveMapChannelNum();
  //  look for the requested map numbers
  std::vector<int> useMapNums;
  for (int i = 0; i < mapNums.Count(); ++i)
    if (mapNums[i] >= 1)
      useMapNums.push_back(mapNums[i]);
  if (useMapNums.size() < (size_t)exportUv) {
    ::LogPrint("Limiting exported UV coordinates to %ld because of available maps.\n", useMapNums.size());
    exportUv = (int)useMapNums.size();
  }
  if (exportUv == 0) {
    ::LogPrint("No UV channels means no tangent mapping.\n");
    exportTangent = false;
  }
  if (exportUv > MAX_UV_SETS) {
    ::LogPrint("exportUv is %d; only %d channels will be exported.\n", exportUv, MAX_UV_SETS);
    exportUv = MAX_UV_SETS;
  }

  //  extract all unique vertices on a per-face basis
  std::vector<DWORD> materialIndices;
  //  some materials may not be included
  std::vector<std::pair<IGameMaterial *, bool> > materials;
  int maxIx = -1;
  std::vector<bool> includeFace;
  int nColor = gm->GetNumberOfColorVerts();
  int nAlpha = gm->GetNumberOfAlphaVerts();
  for (int i = 0; i < (int)numFaces; ++i) {
    FaceEx * fex = gm->GetFace(i);
    IGameMaterial * imat = gm->GetMaterialFromFace(fex);
    size_t nMat = materials.size();
    bool inc;
    for (size_t m = 0; m < nMat; ++m) {
      if (materials[m].first == imat) {
        includeFace.push_back(materials[m].second);
        if (!materials[m].second) {
          goto dont_include;
        }
        materialIndices.push_back((DWORD)m);
        goto got_mat;
      }
    }
    // assume DirectX materials are "textured"
    inc = !ignoreUntextured_ || 
      (imat && imat->GetDiffuseData() && imat->GetNumberOfTextureMaps()) || 
      (imat && imat->GetMaxMaterial()->GetInterface(IDXMATERIAL_INTERFACE));
    materials.push_back(std::pair<IGameMaterial *, bool>(imat, inc));
    includeFace.push_back(inc);
    if (!inc) {
      goto dont_include;
    }
    materialIndices.push_back((DWORD)materials.size()-1);
got_mat:
    for (int j = 0; j < 3; ++j) {
      memset(&fv, 0, sizeof(fv));
      fv.origVert = fex->vert[j];
      fv.pos = gm->GetVertex(fex->vert[j], false) * scale;
      AdjustPoint(fv.pos);
      fv.pos = fv.pos * worldToNode;
      //  additional fields for each vertex based on options
      if (exportNormal) {
        fv.normal = gm->GetNormal(fex->norm[j], false);
        AdjustPoint(fv.normal);
        fv.normal = TransformVector(normalMat, fv.normal);
      }
      if (exportTangent) {
        int tbi = gm->GetFaceVertexTangentBinormal(i, j, 1);
        fv.tangent = gm->GetTangent(tbi, 1);
        AdjustPoint(fv.tangent);
        fv.tangent = TransformVector(normalMat, fv.tangent);
        FixTangent(fv, tangentBad);
        fv.binormal = gm->GetBinormal(tbi, 1);
        AdjustPoint(fv.binormal);
        fv.binormal = TransformVector(normalMat, fv.binormal);
        FixBinormal(fv, binormalBad);
        if (flipTangent) {
          fv.tangent *= -1;
        }
      }
      for (int q = 0; q < exportUv; ++q) {
        DWORD index[3] = {0, 0, 0};
        //  maps are indexed by their Max usage, not by index.
        //  Texture coordinates start at 1, and we've verified 
        //  that the maps at index 0..exportUv-1 are available.
        gm->GetMapFaceIndex(useMapNums[q], i, index);
        fv.uv[q] = (Point2&)gm->GetMapVertex(q+1, index[j]);
        //  The default is a need to flip the texture, at least 
        //  when it's a JPG or similar.
        if (!flipV_) {
          fv.uv[q].y = 1.0f - fv.uv[q].y;
        }
      }
      if (exportColor) {
        Point3 p3(1, 1, 1);
        int ix = fex->color[j];
        if (ix >= 0 && ix < nColor) {
          p3 = gm->GetColorVertex(fex->color[j]);
        }
        Point4 color(p3.x, p3.y, p3.z, 1.0f);
        ix = fex->alpha[j];
        if (ix >= 0 && ix < nAlpha) {
          color.w = gm->GetAlphaVertex(fex->alpha[j]);
          //  this particular formulation tests properly for NaN
          if (!(color.w >= 0 && color.w <= 65536))
            color.w = 0;
        }
        fv.color = color;
      }
      indices[numFacesUsed * 3 + j] = VertexAdd(fatVerts, accel, fv);
    }
    ++numFacesUsed;
dont_include:
    ;
  }
  indices.resize(numFacesUsed*3);
  //  flip faces if swapping to right-handed space
  if (rightHanded != flipWinding) {
    for (int i = 0; i < (int)numFacesUsed; ++i) {
      std::swap(indices[i*3+1], indices[i*3+2]);
    }
  }

  //  generate the initial Mesh geometry data
  DWORD numVertices = (DWORD)fatVerts.size();
  if (numVertices == 0) {
    return; //  no idea trying to add anything else
  }
  append(mesh, numVertices);
  std::vector<Point3> verts;
  verts.resize(numVertices);
  for (int i = 0; i < (int)numVertices; ++i) {
    verts[i] = fatVerts[i].pos;
  }
  append(mesh, verts);
  append(mesh, numFacesUsed);
  //  re-pack indices to include a "3" in front of each triangle
  DWORD tri[4];
  tri[0] = 3;
  DWORD curUsedIx = 0;
  for (int i = 0; i < (int)numFaces; ++i) {
    if (includeFace[i]) {
      for (int j = 0; j < 3; ++j) {
        tri[j+1] = indices[curUsedIx*3+j];
      }
      append(mesh, tri, 4);
      ++curUsedIx;
    }
  }
  assert(curUsedIx == numFacesUsed);

  CComPtr<ID3DXFileSaveData> stored;
  CHECK( frame->AddDataObject(TID_D3DRMMesh, SafeName(node, "mesh"), 0, mesh.size(), &mesh[0], &stored) );

  //  Calculate mapping from old vertex index to new vertex indices
  Remap remap;
  if (fatVerts.size() > 0) {
    FatVert * fvp = &fatVerts[0], * base = fvp, * end = fvp + fatVerts.size();
    int mo = -1;
    while (fvp < end) {
      if (mo < fvp->origVert) {
        mo = fvp->origVert;
      }
      ++fvp;
    }
    remap.SetOldCount(mo + 1);
    fvp = base;
    while (fvp < end) {
      remap.MapOldToNew((int)fvp->origVert, (int)(fvp-base));
      ++fvp;
    }
  }

  //  write normals
  if (exportNormal) {
    std::vector<char> normals;
    DWORD numNormals = (DWORD)fatVerts.size();
    append(normals, numNormals);
    verts.resize(numNormals);
    for (int i = 0; i < (int)numNormals; ++i) {
      verts[i] = fatVerts[i].normal;
    }
    append(normals, verts);
    append(normals, numFacesUsed);
    curUsedIx = 0;
    for (int i = 0; i < (int)numFaces; ++i) {
      if (includeFace[i]) {
        for (int j = 0; j < 3; ++j) {
          tri[j+1] = indices[curUsedIx*3+j];
        }
        append(normals, tri, 4);
        ++curUsedIx;
      }
    }
    assert(curUsedIx == numFacesUsed);
    CComPtr<ID3DXFileSaveData> sNormals;
    CHECK( stored->AddDataObject(TID_D3DRMMeshNormals, _T("normals"), 0, normals.size(), &normals[0], &sNormals) );
  }

  //  write first texcoords
  if (exportUv > 0) {
    std::vector<char> texcoords;
    DWORD numTexCoords = numVertices;
    append(texcoords, numTexCoords);
    std::vector<Point2> tex;
    tex.resize(numTexCoords);
    for (int i = 0; i < (int)numTexCoords; ++i) {
      tex[i] = fatVerts[i].uv[0];
    }
    append(texcoords, tex);
    CComPtr<ID3DXFileSaveData> sTexcoords;
    CHECK( stored->AddDataObject(TID_D3DRMMeshTextureCoords, _T("tc0"), 0, texcoords.size(), &texcoords[0], &sTexcoords) );
  }

  //  write vertex colors
  if (exportColor) {
    std::vector<char> colors;
    DWORD numVertexColors = numVertices;
    append(colors, numVertexColors);
    for (int i = 0; i < (int)numVertexColors; ++i) {
      DWORD target = i;
      append(colors, target);
      append(colors, fatVerts[i].color);
    }
    CComPtr<ID3DXFileSaveData> sColors;
    CHECK( stored->AddDataObject(TID_D3DRMMeshVertexColors, _T("col0"), 0, colors.size(), &colors[0], &sColors) );
  }

  // write tangent basis and extra texcoords
  if (exportUv > 1 || exportTangent) {
    D3DVERTEXELEMENT9 elems[MAX_UV_SETS+5] = {
      {0xFF,0,D3DDECLTYPE_UNUSED, 0,0,0},
      {0xFF,0,D3DDECLTYPE_UNUSED, 0,0,0},
      {0xFF,0,D3DDECLTYPE_UNUSED, 0,0,0},
      {0xFF,0,D3DDECLTYPE_UNUSED, 0,0,0},
      {0xFF,0,D3DDECLTYPE_UNUSED, 0,0,0},
      {0xFF,0,D3DDECLTYPE_UNUSED, 0,0,0},
      {0xFF,0,D3DDECLTYPE_UNUSED, 0,0,0},
      {0xFF,0,D3DDECLTYPE_UNUSED, 0,0,0},
      {0xFF,0,D3DDECLTYPE_UNUSED, 0,0,0},
    };
    int i = 0;
    WORD offset = 0;
    for (int uvSet = 1; uvSet < exportUv; ++uvSet) {
      D3DVERTEXELEMENT9 ve = { 0, offset, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, uvSet };
      elems[i] = ve;
      ++i;
      offset += 8;
    }
    if (exportTangent) {
      D3DVERTEXELEMENT9 ve1 = { 0, offset, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL, 0 };
      offset += 12;
      D3DVERTEXELEMENT9 ve2 = { 0, offset, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0 };
      offset += 12;
      elems[i] = ve1;
      ++i;
      elems[i] = ve2;
      ++i;
    }
    std::vector<char> data;
    append(data, (DWORD)i);
    for (int j = 0; j < i; ++j) {
      append(data, (DWORD)elems[j].Type);
      append(data, (DWORD)elems[j].Method);
      append(data, (DWORD)elems[j].Usage);
      append(data, (DWORD)elems[j].UsageIndex);
    }
    DWORD dSize = offset/4 * numVertices;
    append(data, dSize);
    for (DWORD d = 0; d < numVertices; ++d) {
      for (int uvSet = 1; uvSet < exportUv; ++uvSet) {
        append(data, (Point2 const &)fatVerts[d].uv[uvSet]);
      }
      if (exportTangent) {
        append(data, (Point3 const &)fatVerts[d].binormal);
        append(data, (Point3 const &)fatVerts[d].tangent);
      }
    }
    CComPtr<ID3DXFileSaveData> fsd;
    CHECK( stored->AddDataObject(DXFILEOBJ_DeclData, 0, 0, data.size(), &data[0], &fsd) );
  }

  //todo: write morph targets if present

  std::vector<int> unweightedVerts;

  //  write skinning data if present
  if (gs && exportSkinning) {
    //  build a table from vertex to array of bones
    struct BoneWeight {
      int index;
      float weight;
      bool operator==(BoneWeight const & o) const {
        return o.weight == weight;
      }
      bool operator<(BoneWeight const & o) const {
        //  sort biggest first
        return o.weight < weight;
      }
    };
    //  build the cross-mapping of verticies used by bones
    std::vector<std::vector<BoneWeight> > assignments;
    std::map<IGameNode *, int> boneIndices;
    std::map<IGameNode *, int>::iterator ptr;
    int vc = gs->GetNumOfSkinnedVerts();
    LogPrint("%d skinned vertices in %s\n", vc, node->GetName());
    //  If a single vertex is weighted to more than 40 bones, 
    //  truncate at 40.
    BoneWeight bw[40];
    int numMax = 0;
    for (int vi = 0; vi < vc; ++vi) {
      int nb = gs->GetNumberOfBones(vi);
      if (nb > 40) {
        nb = 40;
      }
      for (int i = 0; i < nb; ++i) {
        IGameNode * node = gs->GetIGameBone(vi, i);
        if (node == NULL) {
          if (i == 0) {
            unweightedVerts.push_back(vi);
          }
          continue;
        }
        ptr = boneIndices.find(node);
        if (ptr != boneIndices.end()) {
          bw[i].index = (*ptr).second;
        }
        else {
          bw[i].index = (int)boneIndices.size();
          boneIndices[node] = bw[i].index;
          assignments.push_back(std::vector<BoneWeight>());
        }
        bw[i].weight = gs->GetWeight(vi, i);
      }
      std::sort(bw, &bw[nb]);
      if (nb < 4) {
        memset(&bw[nb], 0, (4-nb)*sizeof(BoneWeight));
        if (nb > numMax) {
          numMax = nb;
        }
      }
      else {
        numMax = 4;
      }
      //  normalize the 4 highest weighted
      float sum = bw[0].weight + bw[1].weight + bw[2].weight + bw[3].weight;
      sum = 1.0f / sum;
      bw[0].weight *= sum; bw[1].weight *= sum; bw[2].weight *= sum; bw[3].weight *= sum;
      for (int i = 0; i < 4; ++i) {
        BoneWeight ww;
        ww.index = vi;
        ww.weight = bw[i].weight;
        //  don't include a vert that's not actually affected
        if (ww.weight > 0) {
          assignments[bw[i].index].push_back(ww);
        }
      }
    }
    //  calculate number of bones that affect the mesh
    int numAffect = 0;
    for (size_t as = 0; as < assignments.size(); ++as) {
      if (assignments[as].size() > 0) {
        ++numAffect;
      }
    }

    if (unweightedVerts.size() > 0) {
      size_t n = unweightedVerts.size();
      char const *str = "";
      if (n > 30) {
        n = 30;
        str = "...";
      }
      std::stringstream ss;
      ss << "The following vertices are not weighted to any bone:\n";
      for (size_t i = 0; i < n; ++i) {
        ss << (i > 0 ? ", " : "") << unweightedVerts[i];
      }
      ss << str;
      ss << "\n(mesh name " << SafeName(node, "mesh") << ")";
      LogError("%s\n", ss.str().c_str());
    }
    //  write XSkinMeshHeader data
    CComPtr<ID3DXFileSaveData> skinHeader;
    struct hdr {
      WORD weights_per_vertex;
      WORD weights_per_face;
      WORD num_bones;
    };
    hdr h;
    h.weights_per_vertex = numMax;
    h.weights_per_face = 0;
    h.num_bones = numAffect;
    CHECK( stored->AddDataObject(DXFILEOBJ_XSkinMeshHeader, 0, 0, 12, &h, &skinHeader) );

    //  write an XSkinInfo for each bone that affects the mesh
    for (std::map<IGameNode *, int>::iterator ptr = boneIndices.begin(), end = boneIndices.end();
        ptr != end; ++ptr) {
      IGameNode * bone = (*ptr).first;
      std::vector<char> skinData;
      std::string s(convert_name(bone->GetName()));
      skinData.insert(skinData.end(), s.c_str(), s.c_str()+s.length()+1);
      int bix = (*ptr).second;
      std::vector<BoneWeight> & bwv = assignments[bix];
      if (bwv.size()) {
        std::vector<DWORD> skinIndices;
        std::vector<float> skinWeights;
        BoneWeight * bw = &bwv[0];
        DWORD n = (DWORD)bwv.size();
        for (DWORD t = 0; t < n; ++t) {
          int oix = bw[t].index;
          float ow = bw[t].weight;
          int oixCnt = remap.GetCount(oix);
          //  Each old vert may generate more than one new vert
          for (int i = 0; i < oixCnt; ++i) {
            int x = remap.GetNew(oix, i);
            if (x == -1) break;
            assert((unsigned int)x < numVertices && x >= 0);
            skinIndices.push_back(x);
            skinWeights.push_back(ow);
          }
        }
        append(skinData, (DWORD)skinIndices.size());
        append(skinData, skinIndices);
        append(skinData, skinWeights);
        //  Calculate bone matrix at init time.
        //  Then calculate the matrix that takes the init vertex to the 
        //  init bone space.
        GMatrix boneInitTM;
        gs->GetInitBoneTM(bone, boneInitTM);
        AdjustMatrix(boneInitTM);
#define BONE_INIT_IS_PARENT_RELATIVE 0
#if BONE_INIT_IS_PARENT_RELATIVE
        GMatrix invParent;
        IGameNode *boneParent = bone->GetNodeParent();
        if (boneParent != NULL) {
          gs->GetInitBoneTM(boneParent, invParent);
          AdjustMatrix(invParent);
          boneInitTM = invParent * boneInitTM;
        }
#endif
#define BONE_INIT_INCLUDES_SKIN_INIT 0
#if BONE_INIT_INCLUDES_SKIN_INIT
        GMatrix invSkin;
        gs->GetInitSkinTM(invSkin);
        AdjustMatrix(invSkin);
        boneInitTM = boneInitTM * invSkin;
#endif
        append(skinData, boneInitTM.Inverse());
        CComPtr<ID3DXFileSaveData> saveSkin;
        CHECK( stored->AddDataObject(DXFILEOBJ_SkinWeights, (std::string("W-") + s).c_str(), 0, skinData.size(), &skinData[0], &saveSkin) );
      }
    }
  }

  if (tangentBad || binormalBad)
  {
    LogError("Mesh %s: bad %s found (degenerate face in UV map?)\nkW X-port will patch up the geometry in the exported file.\n", 
        node->GetName(), tangentBad ? binormalBad ? "tangent and binormal" : "tangent" : "binormal");
  }

  //  write materials
  if (exportMaterial) {
    std::vector<char> material;
    append(material, (DWORD)materials.size());
    append(material, (DWORD)materialIndices.size());
    append(material, materialIndices);
    CComPtr<ID3DXFileSaveData> mlist;
    CHECK( stored->AddDataObject(TID_D3DRMMeshMaterialList, _T("mtls"), 0, material.size(), &material[0], &mlist) );
    //  write textures
    for (size_t i = 0; i < materials.size(); ++i) {
      material.resize(0);
      //  extract parameter block data
      float f;
      Point3 p3(0,0,0);
      Point4 p4(0,0,0,1);
      std::string mname;
      bool hasDiffuseTexture = false;
      if (!materials[i].first) {
not_stdmat:
        char *mtlName = materials[i].first ? materials[i].first->GetMaterialName() : "null";
        if (mtlName == NULL) mtlName = "null";
        LogPrint("Not a standard material at index %d (%s)\n", i, mtlName);
        IPoint3 ip3 = node->GetWireframeColor();
        append(material, Point4(ip3.x / 255.0f, ip3.y / 255.0f, ip3.z / 255.0f, 1.0f));
        append(material, 16.0f);
        append(material, Point3(1, 1, 1));
        append(material, Point3(0, 0, 0));
        mname = "Dflt_Material";
      }
      else {
        IGameMaterial * theMat = materials[i].first;
        IGameProperty * matProp = theMat->GetDiffuseData();
        if (!matProp) goto not_stdmat;
        int type = matProp->GetType();
        assert(type == IGAME_POINT3_PROP);
        matProp->GetPropertyValue((Point3&)p4);
        matProp = theMat->GetOpacityData();
        if (!matProp) goto not_stdmat;
        type = matProp->GetType();
        assert(type == IGAME_FLOAT_PROP);
        matProp->GetPropertyValue(p4.w);
        append(material, p4);
        matProp = theMat->GetGlossinessData();
        if (!matProp) goto not_stdmat;
        type = matProp->GetType();
        assert(type == IGAME_FLOAT_PROP);
        matProp->GetPropertyValue(f);
        f *= 100;
        append(material, f);
        matProp = theMat->GetSpecularData();
        if (!matProp) goto not_stdmat;
        matProp->GetPropertyValue(p3);
        type = matProp->GetType();
        assert(type == IGAME_POINT3_PROP);
        matProp = theMat->GetSpecularLevelData();
        if (!matProp) goto not_stdmat;
        matProp->GetPropertyValue(f);
        if (f > 0) {
          p3.x = f; p3.y = f; p3.z = f;
        }
        append(material, p3);
        matProp = theMat->GetEmissiveData();
        if (!matProp) goto not_stdmat;
        matProp->GetPropertyValue(p3);
        type = matProp->GetType();
        assert(type == IGAME_POINT3_PROP);
        matProp = theMat->GetEmissiveAmtData();
        if (!matProp) goto not_stdmat;
        matProp->GetPropertyValue(f);
        if (f > 0) {
          p3.x = f; p3.y = f; p3.z = f;
        }
        append(material, p3);
        mname = convert_name(theMat->GetMaterialName());
      }

      CComPtr<ID3DXFileSaveData> mtl;
      CHECK( mlist->AddDataObject(TID_D3DRMMaterial, mname.c_str(), 
          0, material.size(), &material[0], &mtl) );

      IDxMaterial3 * gfx = NULL;
      if (i < materials.size() && materials[i].first != NULL) {
        gfx = (IDxMaterial3 *)materials[i].first->GetMaxMaterial()->GetInterface(IDXMATERIAL_INTERFACE);
        std::string effectName = (gfx == NULL) ? std::string("null") : GetEffectFilename(gfx);
        LogPrint("Material %d shader returned effect file name '%s'\n", i, effectName.c_str());
      }
      bool mapExemption = gfx != NULL && gfx != NULL;
        
      //  write texture map names
      int nText = materials[i].first ? materials[i].first->GetNumberOfTextureMaps() : 0;
      std::map<string, string> textures;
      for (int bi = 0; bi < nText; ++bi) {
        IGameTextureMap * gtx = materials[i].first->GetIGameTextureMap(bi);
        if (!gtx || !gtx->IsEntitySupported()) {
          if (!mapExemption)
            LogError("%s", (std::string("Material ") + materials[i].first->GetMaterialName() +
              " contains a texture map type that is not supported.\n").c_str());
          continue;
        }
        int slot = gtx->GetStdMapSlot();
        if (slot < 0 || slot >= sizeof(mapSlotNames)/sizeof(mapSlotNames[0])) {
          // work around crash when there's no actual map in the material
          if (!mapExemption)
            LogError("Material %s contains a bad or missing texture map (slot id %d).\n",
                materials[i].first->GetMaterialName(), slot);
          continue;
        }
        TCHAR const *texPathData = gtx->GetBitmapFileName();
        if (texPathData == NULL || texPathData[0] == 0) {
          // work around crash when there's no path in the map
          if (!mapExemption)
            LogError("%s", (std::string("material ") + materials[i].first->GetMaterialName() +
              " contains a bitmap without a file path.\n").c_str());
          continue;
        }
        std::string texPath(texPathData);
        texPath = GetFullPath(texPath.c_str());
        std::string origPath(texPath.c_str());
        if (renameTex) {
          std::string::size_type st = texPath.rfind('.');
          if (st != texPath.npos) {
            texPath.replace(st, texPath.size()-st, ".dds");
          }
        }
        if (!fullPath) {
          texPath = MakeRelativePath(texPath, prefixTex_);
        }
        TCHAR const * outputName = texPath.c_str();
        assert(outputName != NULL);
        if (outputName[0]) {
          textures[mapSlotNames[slot]] = outputName;
          if (origPath.size() > 0)
            textureCopy_[origPath] = outputName;
        }
        if (slot == ID_DI) {
          if (outputName[0]) {
            CComPtr<ID3DXFileSaveData> tex;
            CHECK( mtl->AddDataObject(TID_D3DRMTextureFilename, "Diffuse", 0, strlen(outputName)+1, outputName, &tex) );
            hasDiffuseTexture = true;
          }
        }
      }
      std::string backupDiffuse;

      //  write an effect
      if (gfx) {
        ::OutputDebugString("kW X-port exporting shader\n");
        std::string fxPath = GetEffectFilename(gfx);

        fxPath = GetFullPath(fxPath.c_str());
        std::string origPath(fxPath.c_str());
        if (!fullPath) {
          fxPath = MakeRelativePath(fxPath, prefixTex_);
        }
        TCHAR const * outputName = fxPath.c_str();
        assert(outputName != NULL);
        if (outputName[0]) {
          if (origPath.size() > 0)
            textureCopy_[origPath] = outputName;
        }

        TCHAR const * slash = fxPath.c_str();
        assert(slash != NULL);
        if (slash[0]) {
          CComPtr<ID3DXFileSaveData> fxData;
          CHECK( mtl->AddDataObject(DXFILEOBJ_EffectInstance, "Effect", 0, strlen(slash)+1, slash, &fxData) );
          //  write effect parameters
          IParamBlock2 *fxpb = materials[i].first->GetMaxMaterial()->GetParamBlock(0);
          if (fxpb == NULL)
          {
            LogError("Effect %s does not have param block 0?!\n", slash);
          }
          int nProp = fxpb ? fxpb->NumParams() : 0;
          for (int fxi = 0; fxi < nProp; ++fxi) {
            int pid = fxpb->IndextoID(fxi);
            ParamDef pdef = fxpb->GetParamDef(pid);
            std::string val;
            std::string name = fxpb->GetLocalName(pid);
            float fdata[4];
            int i;
            TCHAR * str;
            int wrFloat = 0;
            bool wrDword = false;
            bool wrString = false;
            bool isPath = false;
            char buf[50];
            switch (pdef.type) {
              case TYPE_FLOAT:
              case TYPE_PCNT_FRAC:
                fdata[0] = fxpb->GetFloat(pid);
                sprintf(buf, "%.f", fdata[0]);
                val = buf;
                wrFloat = 1;
                break;
              case TYPE_BOOL:
              case TYPE_INT:
                i = fxpb->GetInt(pid);
                sprintf(buf, "%d", i);
                val = buf;
                wrDword = true;
                break;
              case TYPE_POINT3:
                (Point3 &)fdata[0] = fxpb->GetPoint3(pid);
                sprintf(buf, "float3(%f,%f,%f)", fdata[0], fdata[1], fdata[2]);
                val = buf;
                wrFloat = 3;
                break;
              case TYPE_POINT4:
                (Point4 &)fdata[0] = fxpb->GetPoint4(pid);
                sprintf(buf, "float4(%f,%f,%f,%f)", fdata[0], fdata[1], fdata[2], fdata[3]);
                val = buf;
                wrFloat = 4;
                break;
              case TYPE_FRGBA:
                (Point4 &)fdata[0] = fxpb->GetAColor(pid);
                sprintf(buf, "float4(%f,%f,%f,%f)", fdata[0], fdata[1], fdata[2], fdata[3]);
                val = buf;
                wrFloat = 4;
                break;
              case TYPE_STRING:
              case TYPE_FILENAME:
                val = fxpb->GetStr(pid);
                wrString = true;
                isPath = (val[0] == '.' || val[1] == ':');
                break;
              case TYPE_BITMAP:
                {
                  isPath = true;
                  PBBitmap * pbb = fxpb->GetBitmap(pid);
                  if (pbb) {
                    val = (str = (TCHAR *)pbb->bi.Name());
                  }
                  wrString = val.size() > 0 && strcmp(str, "None");
                }
                break;
              case TYPE_TEXMAP:
                {
                  isPath = true;
                  Texmap * tmp = fxpb->GetTexmap(pid);
                  BitmapTex * bmt = 0;
                  if (tmp && tmp->ClassID().PartA() == BMTEX_CLASS_ID) {
                    bmt = static_cast<BitmapTex *>(tmp);
                  }
                  if (bmt) {
                    val = (str = bmt->GetMapName());
                    wrString = val.size() > 0;
                  }
                }
                break;
              default:
                LogPrint("Skipping unknown FX parameter named '%s' (id %d, type %d).\n", name.c_str(), pid, pdef.type);
                break;
            }
            if (wrFloat || wrDword || wrString) {
              LogPrint("%s", (name + " = " + val + "\n").c_str());
            }
            std::vector<char> data;
            if (wrFloat) {
              CComPtr<ID3DXFileSaveData> propData;
              data.resize(0);
              append(data, name);
              append(data, wrFloat);
              append(data, fdata, wrFloat);
              CHECK( fxData->AddDataObject(DXFILEOBJ_EffectParamFloats, 0, 0, data.size(), &data[0], &propData) );
            }
            if (wrDword) {
              CComPtr<ID3DXFileSaveData> propData;
              data.resize(0);
              append(data, name);
              append(data, i);
              assert(sizeof(i) == sizeof(DWORD));
              CHECK( fxData->AddDataObject(DXFILEOBJ_EffectParamDWord, 0, 0, data.size(), &data[0], &propData) );
            }
            if (wrString) {
              CComPtr<ID3DXFileSaveData> propData;
              data.resize(0);
              append(data, name);
              //  This is a hack to recognize absolute paths, and 
              //  turn them into relative paths if we don't want absolutes.
              if (isPath) {
                val = GetFullPath(val.c_str());
                std::string orig(val);
                if (!fullPath) {
                  std::string::size_type pos = val.rfind('\\');
                  if (pos != std::string::npos) {
                    val.erase(0, pos+1);
                    val = prefixTex_ + val;
                  }
                }
                textureCopy_[orig] = val;
                if (backupDiffuse.empty()) {
                  backupDiffuse = val;
                }
              }
              append(data, val);
              CHECK( fxData->AddDataObject(DXFILEOBJ_EffectParamString, 0, 0, data.size(), &data[0], &propData) );
            }
          }
        }
        ::OutputDebugString("kW X-port exporting shader done\n");
      }
      if (!hasDiffuseTexture && !backupDiffuse.empty()) {
        ::OutputDebugString("Writing backup diffuse texture parameter.\n");
        CComPtr<ID3DXFileSaveData> tex;
        CHECK( mtl->AddDataObject(TID_D3DRMTextureFilename, "Diffuse", 0, backupDiffuse.size() + 1, backupDiffuse.c_str(), &tex) );
      }
    }
  }
}

template<typename T>
bool Contains(Tab<T> &tab, T n) {
  size_t count = tab.Count();
  for (size_t i = 0; i < count; ++i) {
    if (n == tab[i]) {
      return true;
    }
  }
  return false;
}

template<typename T>
void IGameExporter::ExportNode(IGameScene * ig, IGameNode * node, T * parent, bool selected)
{
  bool notRenderable = false;
  IGameObject *go = node->GetIGameObject();
  if (go != NULL) {
    notRenderable = !node->GetMaxNode()->Renderable();
    //  Bones and dummies are not renderable, but needs to be exported anyway.
    //  If you really don't want them, hide them!
    if (go->GetMaxType() == IGameObject::IGAME_MAX_HELPER
        || go->GetMaxType() == IGameObject::IGAME_MAX_BONE) {
      notRenderable = false;
    }
  }
  if (!exportHidden_ && node->IsNodeHidden()) {
    //  do nothing to hidden nodes
    ::OutputDebugString("kW X-port not exporting hidden node.\n");
    return;
  }

  if (!selected) {
    if (Contains(gSelectedNodes, node->GetMaxNode())) {
      selected = true;
    }
  }

  if (selected) {
    ::OutputDebugString((std::string("kW X-port exporting node: ") + node->GetName() + "\n").c_str());
    animated_.push_back(node);

    GMatrix otm = node->GetWorldTM(coreFrame_);
    AdjustMatrix(otm);
    //  Ack! The node needs to know about the object!
    IGameObject *gobj = notRenderable ? NULL : node->GetIGameObject();
    if (gobj != NULL && gobj->GetIGameType() == IGameObject::IGAME_MESH) {
      IGameSkin *gs = gobj->GetIGameSkin();
      if (gs && exportSkinning_) {
        gs->GetInitSkinTM(otm);
        AdjustMatrix(otm);
        if (node->GetChildCount() > 0) {
          LogError("Node %s has %d children, but is also skinned -- expect transform problems.\n",
              node->GetName(), node->GetChildCount());
        }
      }
    }
    GMatrix ptm;
    ptm.SetIdentity();
    if (node->GetNodeParent() != 0) {
      ptm = node->GetNodeParent()->GetWorldTM(coreFrame_);
      AdjustMatrix(ptm);
      //  otm is local relative to parent
      otm = otm * ptm.Inverse();
    }

    CComPtr<ID3DXFileSaveData> frame;
    std::string n = convert_name(node->GetName());
    LogPrint(_T("Saving node: %s\n"), n.c_str());
    CHECK( parent->AddDataObject(TID_D3DRMFrame, n.c_str(), 0, 0, 0, &frame) );

    //  todo: set transformation to identity if "trim parents" is turned on

    CComPtr<ID3DXFileSaveData> temp;
    CHECK( frame->AddDataObject(TID_D3DRMFrameTransformMatrix, "relative", 0, sizeof(otm), &otm, &temp) );
    temp = 0;

    //  todo: save user props, if chosen
    if (exportComments_) {
      TSTR uPropBuf;
      node->GetMaxNode()->GetUserPropBuffer(uPropBuf);
      TCHAR const *str = uPropBuf.data();
      TCHAR const *end = str + uPropBuf.length();
      TCHAR const *base = str;
      TCHAR const *eq = 0;
      while (str < end) {
        if (base == 0 && !isspace(*str)) {
          base = str;
        }
        if (*str == '\n' || *str == '\r') {
store:
          //  treat '#' as a comment field
          if (eq != 0 && eq > base && base < str && *base != '#') {
            std::string key(base, eq);
            std::string value(eq+1, str);
            AddKeyValue( frame.p, key.c_str(), value.c_str() );
          }
          eq = 0;
          base = 0;
        }
        else if (str == end) {
          ++str;
          goto store;
        }
        else if (*str == '=') {
          eq = str;
        }
        ++str;
      }
    }

    GMatrix nmat2 = node->GetWorldTM(coreFrame_);
    AdjustMatrix(nmat2);
    nmat2 = nmat2.Inverse();
    GMatrix omat = node->GetObjectTM(coreFrame_);
    AdjustMatrix(omat);
    omat = omat * nmat2;
    CHECK( frame->AddDataObject(kW_ObjectMatrixComment, "object", 0, sizeof(omat), &omat, &temp) );
    temp = 0;

    IGameObject * obj = notRenderable ? 0 : node->GetIGameObject();
    if (obj) {
      if (obj->GetIGameType() == IGameObject::IGAME_SPLINE) {
        //  This test -- before checking whether it's supported -- is a
        //  necessary work-around to avoid crashing, due to a bug in 3ds Max 3DXI.
      }
      else if (obj->IsEntitySupported()) {
        switch (obj->GetIGameType()) {
          case IGameObject::IGAME_MESH:
            WriteGeometry2(ig, node, obj, frame);
            break;
          //todo: write cameras, lights and dummies
          default:
            // do nothing
            break;
        }
      }
    }
    int nc = node->GetChildCount();
    for (int i = 0; i < nc; ++i) {
      ExportNode(ig, node->GetNodeChild(i), frame.p, true);
    }
  }
  else {
    int nc = node->GetChildCount();
    for (int i = 0; i < nc; ++i) {
      ExportNode(ig, node->GetNodeChild(i), parent, false);
    }
  }
}

static bool almostEqual(Point3 const & a, Point3 const & b)
{
  return fabsf(a.x-b.x) < 1e-4 && fabsf(a.y-b.y) < 1e-4 && fabsf(a.z-b.z) < 1e-4;
}

static bool almostEqual(Quat const & a, Quat const & b)
{
  return fabsf(a.x-b.x) < 1e-4 && fabsf(a.y-b.y) < 1e-4 && fabsf(a.z-b.z) < 1e-4 && fabsf(a.w-b.w) < 1e-4;
}

static float dot(Quat const & q, Quat const & p)
{
  return q.x*p.x + q.y*p.y + q.z*p.z + q.w*p.w;
}

void IGameExporter::ExportAnimations(ID3DXFileSaveObject * save)
{
  ::OutputDebugString("kW X-port exporting animations\n");
  std::vector<AnimationRange> vec;
  GetAnimationRanges(allAnimations_, vec);
  AnimationRange ar;
  ar.first = firstFrame_;
  ar.length = numFrames_;
  ar.name = animationName_;
  ar.timeStretch = timeStretch_;
  if (vec.size() == 0) {
    //  If there are no animations configured, use the 
    //  settings from the settings fields.
    vec.push_back(ar);
  }
  for (std::vector<AnimationRange>::iterator arp = vec.begin(), are = vec.end();
      arp != are; ++arp) {
    ar = *arp;
    std::vector<IGameNode *>::iterator ptr, end;
    CComPtr<ID3DXFileSaveData> animSet;
    DWORD dwd = 4800;   //  3dsMax tick rate is 4800 ticks per second
    CComPtr<ID3DXFileSaveData> anim(0), temp(0);
    CHECK( save->AddDataObject(DXFILEOBJ_AnimTicksPerSecond, "fps", 0, 4, &dwd, &temp) );
    dwd /= GetFrameRate();
    DWORD outTime = (DWORD)(dwd * ar.timeStretch);
    if (outTime < 0) outTime = 0;
    temp = 0;
    LogPrint("Extracting animation %s.\n", ar.name.c_str());
    CHECK( save->AddDataObject(TID_D3DRMAnimationSet, ar.name.c_str(), 0, 0, 0, &animSet) );
    for (ptr = animated_.begin(), end = animated_.end(); ptr != end; ++ptr) {
      IGameNode * node = *ptr;
      std::vector<std::pair<std::pair<DWORD, DWORD>, Point3> > posAnim;
      std::vector<std::pair<std::pair<DWORD, DWORD>, Point3> > scaleAnim;
      std::vector<std::pair<std::pair<DWORD, DWORD>, Quat> > quatAnim;
      Point3 op(-101010, 101010, -101010), os(0,0,0);
      Quat oq(1.0f, 1.0f, 1.0f, 1.0f);
      //  Remember how many frames were dropped, so that the last keyframe 
      //  before a change can be put back in.
      int compPos = 0;
      int compRot = 0;
      int compScale = 0;
      for (int time = 0; time < ar.length; ++time) {
        GMatrix ip;
        IGameNode * np = node->GetNodeParent();
        if (np) {
          ip = np->GetWorldTM(TimeValue((time + ar.first)*dwd));
          AdjustMatrix(ip);
          ip = ip.Inverse();
        }
        else {
          ip.SetIdentity();
        }
        GMatrix tm = node->GetWorldTM(TimeValue((time + ar.first)*dwd));
        AdjustMatrix(tm);
        tm = tm * ip;
        //  decompose
        Matrix3 m3 = tm.ExtractMatrix3();
        Point3 p;
        Quat q;
        Point3 s;
        DecomposeMatrix(m3, p, q, s);
        //  add components
        if (!almostEqual(p, op) || time == 0) {
          if (compPos > 0) {
            posAnim.push_back(std::pair<std::pair<DWORD, DWORD>, Point3>(
              std::pair<DWORD, DWORD>((time-1)*outTime, 3), op));
          }
          posAnim.push_back(std::pair<std::pair<DWORD, DWORD>, Point3>(
              std::pair<DWORD, DWORD>(time*outTime, 3), p));
          op = p;
          compPos = 0;
        }
        else {
          ++compPos;
        }
        if (!almostEqual(s, os) || time == 0) {
          if (compScale > 0) {
            scaleAnim.push_back(std::pair<std::pair<DWORD, DWORD>, Point3>(
                std::pair<DWORD, DWORD>((time-1)*outTime, 3), os));
          }
          scaleAnim.push_back(std::pair<std::pair<DWORD, DWORD>, Point3>(
              std::pair<DWORD, DWORD>(time*outTime, 3), s));
          os = s;
          compScale = 0;
        }
        else {
          ++compScale;
        }
        if (dot(q, oq) < 0) {
          //  take the short way around
          q = Quat(-q.x, -q.y, -q.z, -q.w);
        }
        if (!almostEqual(q, oq) || time == 0) {
          //  X files store quats the old way (W first)
          if (compRot > 0) {
            quatAnim.push_back(std::pair<std::pair<DWORD, DWORD>, Quat>(
                std::pair<DWORD, DWORD>((time-1)*outTime, 4), Quat(oq.w, oq.x, oq.y, oq.z)));
          }
          quatAnim.push_back(std::pair<std::pair<DWORD, DWORD>, Quat>(
              std::pair<DWORD, DWORD>(time*outTime, 4), Quat(q.w, q.x, q.y, q.z)));
          oq = q;
          compRot = 0;
        }
        else {
          ++compRot;
        }
      }
      anim = 0;
      CHECK( animSet->AddDataObject(TID_D3DRMAnimation, (std::string("Anim-") + convert_name(ar.name.c_str()) + 
          "-" + convert_name((*ptr)->GetName())).c_str(), 0, 0, 0, &anim) );
      CHECK( anim->AddDataReference(convert_name((*ptr)->GetName()).c_str(), 0) );
      CComPtr<ID3DXFileSaveData> temp;
      std::vector<char> foo;
      append(foo, (DWORD)0); // rotation animation
      append(foo, (DWORD)quatAnim.size());
      append(foo, quatAnim);
      temp = 0;
      CHECK( anim->AddDataObject(TID_D3DRMAnimationKey, "rot", 0, foo.size(), &foo[0], &temp) );
      temp = 0;
      foo.clear();
      append(foo, (DWORD)1); // scale animation
      append(foo, (DWORD)scaleAnim.size());
      append(foo, scaleAnim);
      CHECK( anim->AddDataObject(TID_D3DRMAnimationKey, "scale", 0, foo.size(), &foo[0], &temp) );
      temp = 0;
      foo.clear();
      append(foo, (DWORD)2); // position animation
      append(foo, (DWORD)posAnim.size());
      append(foo, posAnim);
      CHECK( anim->AddDataObject(TID_D3DRMAnimationKey, "pos", 0, foo.size(), &foo[0], &temp) );
    }
  }
}

// {95A48E28-7EF4-4419-A16A-BA9DBDF0D2BC}
static const GUID kW_ObjectMatrixComment = 
{ 0x95a48e28, 0x7ef4, 0x4419, { 0xa1, 0x6a, 0xba, 0x9d, 0xbd, 0xf0, 0xd2, 0xbc } };
// {26E6B1C3-3D4D-4a1d-A437-B33668FFA1C2}
static const GUID kW_KeyValuePair = 
{ 0x26e6b1c3, 0x3d4d, 0x4a1d, { 0xa4, 0x37, 0xb3, 0x36, 0x68, 0xff, 0xa1, 0xc2 } };

char const * kwTemplates =
"xof 0303txt 0032\n"
"\n" 
"template ObjectMatrixComment {\n"
"  <95A48E28-7EF4-4419-A16A-BA9DBDF0D2BC>\n"
"  Matrix4x4 objectMatrix;\n"
"}\n"
"\n"
"template KeyValuePair {\n"
"  <26E6B1C3-3D4D-4a1d-A437-B33668FFA1C2>\n"
"  string key;\n"
"  string value;\n"
"}\n"
"\n"
;

template<typename T>
void IGameExporter::AddKeyValue(T * cont, char const * key, char const * value)
{
  std::vector<char> data;
  append(data, key);
  append(data, value);
  CComPtr<ID3DXFileSaveData> temp;
  CHECK( cont->AddDataObject(kW_KeyValuePair, 0, 0, data.size(), &data[0], &temp) );
}

void IGameExporter::ExportFileData(ID3DXFileSaveObject * file)
{
  CComPtr<ID3DXFileSaveData> data;
  AddKeyValue(file, "Date", exportTime_);
  AddKeyValue(file, "File", core_->GetCurFilePath().data());
  char uName[256] = "";
  DWORD bSize = 256;
  ::GetUserName(uName, &bSize);
  uName[255] = 0;
  AddKeyValue(file, "User", uName);
  char buf[100];
  sprintf(buf, "%ld", (long)coreFrame_);
  AddKeyValue(file, "CoreTime", buf);
}

void IGameExporter::WriteXFile(IGameScene * ig, TCHAR const * name)
{
  animated_.clear();
  CComPtr<ID3DXFile> file;
  CHECK( D3DXFileCreate(&file) );
  static char const * xExtensions = XEXTENSIONS_TEMPLATES;
  static char const * xSkinExp = XSKINEXP_TEMPLATES;
  CHECK( file->RegisterTemplates((void*)D3DRM_XTEMPLATES, D3DRM_XTEMPLATE_BYTES) );
  CHECK( file->RegisterTemplates((void*)xExtensions, strlen(xExtensions)) );
  CHECK( file->RegisterTemplates((void*)xSkinExp, strlen(xSkinExp)) );
  CHECK( file->RegisterTemplates((void*)kwTemplates, strlen(kwTemplates)) );
  CComPtr<ID3DXFileSaveObject> save;
  D3DXF_FILEFORMAT fileFormat = exportBinary_ ? D3DXF_FILEFORMAT_BINARY : D3DXF_FILEFORMAT_TEXT;
  if (exportCompressed_) {
    fileFormat |= D3DXF_FILEFORMAT_COMPRESSED;
  }
  CHECK( file->CreateSaveObject(name, D3DXF_FILESAVE_TOFILE, fileFormat, &save) );
  CComPtr<ID3DXFileSaveData> root;
  TCHAR const * slash = strrchr(name, '\\');
  if (!slash) slash = strrchr(name, '/');
  if (slash) ++slash; else slash = name;
  string n = convert_name(slash);

  ExportFileData(save.p);
  ::OutputDebugString("kW X-port extracted scene data\n");

//  Sometimes you may wish to keep a single frame at the top. However, 
//  to preserve compatibility with previous exporters, put each top-level 
//  frame at the top of the file.
//  CHECK( save->AddDataObject(TID_D3DRMFrame, n.c_str(), 0, 0, 0, &root) );

  int nObjects = ig->GetTopLevelNodeCount();
  std::vector<IGameNode *> animated;
  for (int i = 0; i < nObjects; ++i) {
    IGameNode * n = ig->GetTopLevelNode(i);
    ExportNode(ig, n, save.p, gSelectedNodes.Count() == 0);
  }

  //  todo: write animations, if selected
  if (exportAnimation_) {
    ExportAnimations(save.p);
  }

  ::OutputDebugString("kW X-port saving X file\n");
  CHECK( save->Save() );

  if (copyTex_)  {
    ::OutputDebugString("kW X-port copying textures\n");
    std::string prefix = name_;
    std::string::size_type offset = name_.find_last_of('\\');
    if (offset == std::string::npos)
      offset = name_.find_last_of('/');
    if (offset == std::string::npos) {
      LogError("Cannot copy textures: output path name %s is not a proper absolute path.\n", name_.c_str());
    }
    else {
      prefix.erase(offset+1);
      for (std::map<string, string>::iterator ptr = textureCopy_.begin(), end = textureCopy_.end();
          ptr != end; ++ptr) {
        if (fullPath_) {
          LogPrint("Not copying %s as full paths are used\n", (*ptr).first.c_str());
        }
        else if ((*ptr).first == (*ptr).second) {
          LogPrint("Not copying %s as the destination is the same as the source.\n", (*ptr).first.c_str());
        }
        else {
          std::string dstName = prefix + (*ptr).second;
          CopyTexture((*ptr).first.c_str(), dstName.c_str());
        }
      }
    }
  }
}

WNDPROC animationParentProc_;
int selectedAnimation_ = -1;
LRESULT CALLBACK AnimationWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg == WM_COMMAND || msg == WM_NOTIFY) {
    return ::SendMessage(::GetParent(hWnd), msg, wParam, lParam);
  }
  return CallWindowProc(animationParentProc_, hWnd, msg, wParam, lParam);
}

class MakeControlsVisitor {
  public:
    MakeControlsVisitor(HWND hWndDlg, IGameExporter * gex)
      : dlg_(hWndDlg)
      , gex_(gex)
      {
        LOGFONT lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = rowHeight-4;
        lf.lfWidth = 0;
        lf.lfWeight = FW_REGULAR;
        lf.lfCharSet = ANSI_CHARSET;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = 0; // ANTIALIASED_QUALITY | CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
        strcpy(lf.lfFaceName, "Arial");
        font_ = ::CreateFontIndirect(&lf);
      }
    bool operator()(char const * group, bool & b, char const * name, bool v, char const * text, int id)
      {
        std::map<std::string, Info>::iterator ptr = groups_.find(group);
        Info i;
        if (ptr == groups_.end()) {
          i.frame = ::FindWindowEx(dlg_, 0, WC_BUTTON, group);
          if (i.frame == 0) {
            i.frame = dlg_;
          }
          i.row = 0;
          i.column = 0;
          groups_[group] = i;
          ptr = groups_.find(group);
        }
        else {
          i = (*ptr).second;
        }
        RECT r;
        ::GetWindowRect(i.frame, &r);
        int left = 10;
        int top = 15;
        int colWidth = (r.right-r.left-15)/numCols;
        int numRows = (r.bottom-r.top-30)/rowHeight;
        HWND btn = ::CreateWindow(WC_BUTTON, text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
            i.column * colWidth + left, i.row * rowHeight + top, 
            colWidth-5, rowHeight-2, i.frame, 0, hInstance, 0);
        //  I'm assuming the same string constant will always be used!
        SetWindowFont(btn, font_, false);
        SetWindowLong(btn, GWL_ID, WindowId(id));
        bool q = ::CheckDlgButton(i.frame, WindowId(id), b) != 0;
        i.row++;
        if (i.row >= numRows) {
          i.column++;
          i.row = 0;
        }
        (*ptr).second = i;
        return true;
      }
    bool operator()(char const * group, float & b, char const * name, float v, char const * text, int id)
      {
        std::map<std::string, Info>::iterator ptr = groups_.find(group);
        Info i;
        if (ptr == groups_.end()) {
          i.frame = ::FindWindowEx(dlg_, 0, WC_BUTTON, group);
          if (i.frame == 0) {
            i.frame = dlg_;
          }
          i.row = 0;
          i.column = 0;
          groups_[group] = i;
          ptr = groups_.find(group);
        }
        else {
          i = (*ptr).second;
        }
        RECT r;
        ::GetWindowRect(i.frame, &r);
        int left = 10;
        int top = 15;
        int colWidth = (r.right-r.left-20)/numCols;
        int numRows = (r.bottom-r.top-30)/rowHeight;
        char val[20];
        _snprintf(val, 20, "%.3f", b);
        val[19] = 0;
        HWND lbl = ::CreateWindow(WC_STATIC, text, WS_CHILD | WS_VISIBLE,
            i.column * colWidth + left, i.row * rowHeight + top,
            labelWidth, rowHeight-2, i.frame, 0, hInstance, 0);
        HWND btn = ::CreateWindowEx(WS_EX_STATICEDGE,
            WC_EDIT, val, WS_CHILD | WS_VISIBLE, 
            i.column * colWidth + left+labelWidth, i.row * rowHeight + top, 
            colWidth-labelWidth-5, rowHeight-2, i.frame, 0, hInstance, 0);
        //  I'm assuming the same string constant will always be used!
        SetWindowFont(lbl, font_, false);
        SetWindowFont(btn, font_, false);
        ::SetWindowLong(btn, GWL_ID, WindowId(id));
        i.row++;
        if (i.row >= numRows) {
          i.column++;
          i.row = 0;
        }
        (*ptr).second = i;
        return true;
      }
    bool operator()(char const * group, int & b, char const * name, int v, char const * text, int id)
      {
        std::map<std::string, Info>::iterator ptr = groups_.find(group);
        Info i;
        if (ptr == groups_.end()) {
          i.frame = ::FindWindowEx(dlg_, 0, WC_BUTTON, group);
          if (i.frame == 0) {
            i.frame = dlg_;
          }
          i.row = 0;
          i.column = 0;
          groups_[group] = i;
          ptr = groups_.find(group);
        }
        else {
          i = (*ptr).second;
        }
        RECT r;
        ::GetWindowRect(i.frame, &r);
        int left = 10;
        int top = 15;
        int colWidth = (r.right-r.left-20)/numCols;
        int numRows = (r.bottom-r.top-30)/rowHeight;
        char val[20];
        _snprintf(val, 20, "%d", b);
        val[19] = 0;
        HWND lbl = ::CreateWindow(WC_STATIC, text, WS_CHILD | WS_VISIBLE,
            i.column * colWidth + left, i.row * rowHeight + top,
            labelWidth, rowHeight-2, i.frame, 0, hInstance, 0);
        HWND btn = ::CreateWindowEx(WS_EX_STATICEDGE,
            WC_EDIT, val, WS_CHILD | WS_VISIBLE, 
            i.column * colWidth + left+labelWidth, i.row * rowHeight + top, 
            colWidth-labelWidth-5, rowHeight-2, i.frame, 0, hInstance, 0);
        //  I'm assuming the same string constant will always be used!
        SetWindowFont(lbl, font_, false);
        SetWindowFont(btn, font_, false);
        ::SetWindowLong(btn, GWL_ID, WindowId(id));
        i.row++;
        if (i.row >= numRows) {
          i.column++;
          i.row = 0;
        }
        (*ptr).second = i;
        return true;
      }
    bool operator()(char const * group, std::string & b, char const * name, std::string v, char const * text, int id)
      {
        std::map<std::string, Info>::iterator ptr = groups_.find(group);
        Info i;
        if (ptr == groups_.end()) {
          i.frame = ::FindWindowEx(dlg_, 0, WC_BUTTON, group);
          if (i.frame == 0) {
            i.frame = dlg_;
          }
          i.row = 0;
          i.column = 0;
          groups_[group] = i;
          ptr = groups_.find(group);
        }
        else {
          i = (*ptr).second;
        }
        RECT r;
        ::GetWindowRect(i.frame, &r);
        int left = 10;
        int top = 15;
        int colWidth = (r.right-r.left-20)/numCols;
        int numRows = (r.bottom-r.top-30)/rowHeight;
        HWND btn = 0;
        if (!strcmp(name, ANIMATIONS_LIST_VIEW)) {
          btn = ::CreateWindow(WC_BUTTON, "Add", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
              i.column * colWidth + left, i.row * rowHeight + top,
              (colWidth-20)/3, rowHeight+1, i.frame, 0, hInstance, 0);
          SetWindowFont(btn, font_, false);
          ::SetWindowLong(btn, GWL_ID, IDB_ADD);
          btn = ::CreateWindow(WC_BUTTON, "Update", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
              i.column * colWidth + left + (colWidth-20)/3+10, i.row * rowHeight + top,
              (colWidth-20)/3, rowHeight+1, i.frame, 0, hInstance, 0);
          SetWindowFont(btn, font_, false);
          ::SetWindowLong(btn, GWL_ID, IDB_UPDATE);
          btn = ::CreateWindow(WC_BUTTON, "Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
              i.column * colWidth + left + (colWidth-20)/3*2+20, i.row * rowHeight + top,
              (colWidth-20)/3, rowHeight+1, i.frame, 0, hInstance, 0);
          SetWindowFont(btn, font_, false);
          ::SetWindowLong(btn, GWL_ID, IDB_REMOVE);
          ++i.row;
          btn = ::CreateWindow(WC_LISTVIEW, "Animations", WS_CHILD | WS_VISIBLE | LVS_REPORT
              | LVS_EDITLABELS | LVS_NOLABELWRAP | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
              i.column * colWidth + left, i.row * rowHeight + top + 3,
              colWidth, rowHeight*6-5, i.frame, 0, hInstance, 0);
          ListView_SetExtendedListViewStyleEx(btn, 
              LVS_EX_FULLROWSELECT | LVS_EX_ONECLICKACTIVATE,
              LVS_EX_FULLROWSELECT | LVS_EX_ONECLICKACTIVATE);
          i.row += 6;
          LVCOLUMN lvc;
          lvc.mask = LVCF_TEXT | LVCF_WIDTH;
          lvc.pszText = "Name";
          lvc.cx = colWidth-120;
          ListView_InsertColumn(btn, 0, &lvc);
          lvc.mask = LVCF_TEXT | LVCF_WIDTH;
          lvc.pszText = "Start";
          lvc.cx = 40;
          ListView_InsertColumn(btn, 1, &lvc);
          lvc.mask = LVCF_TEXT | LVCF_WIDTH;
          lvc.pszText = "Len";
          lvc.cx = 40;
          ListView_InsertColumn(btn, 2, &lvc);
          lvc.mask = LVCF_TEXT | LVCF_WIDTH;
          lvc.pszText = "Stretch";
          lvc.cx = 40;
          ListView_InsertColumn(btn, 3, &lvc);
          SetWindowFont(btn, font_, false);
          ::SetWindowLong(btn, GWL_ID, IDC_ANIMATIONS);
          animationParentProc_ = (WNDPROC)::GetWindowLongPtr(i.frame, GWLP_WNDPROC);
          ::SetWindowLongPtr(i.frame, GWLP_WNDPROC, (LONG_PTR)&AnimationWndProc);
          std::vector<AnimationRange> vec;
          GetAnimationRanges(b, vec);
          LVITEM lvi;
          lvi.mask = LVIF_TEXT;
          char buf[512];
          buf[511] = 0;
          for (std::vector<AnimationRange>::iterator ptr = vec.begin(), end = vec.end();
              ptr != end; ++ptr) {
            lvi.iItem = ListView_GetItemCount(btn);
            lvi.iSubItem = 0;
            lvi.pszText = (LPSTR)(*ptr).name.c_str();
            lvi.iItem = ListView_InsertItem(btn, &lvi);
            sprintf(buf, "%d", (*ptr).first);
            lvi.pszText = buf;
            lvi.iSubItem = 1;
            ListView_SetItem(btn, &lvi);
            sprintf(buf, "%d", (*ptr).length);
            lvi.iSubItem = 2;
            ListView_SetItem(btn, &lvi);
            sprintf(buf, "%g", (*ptr).timeStretch);
            lvi.iSubItem = 3;
            ListView_SetItem(btn, &lvi);
          }
        }
        else {
          HWND lbl = ::CreateWindow(WC_STATIC, text, WS_CHILD | WS_VISIBLE,
              i.column * colWidth + left, i.row * rowHeight + top,
              labelWidth, rowHeight-2, i.frame, 0, hInstance, 0);
          btn = ::CreateWindowEx(WS_EX_STATICEDGE,
              WC_EDIT, b.c_str(), WS_CHILD | WS_VISIBLE, 
              i.column * colWidth + left+labelWidth, i.row * rowHeight + top, 
              colWidth-labelWidth-5, rowHeight-2, i.frame, 0, hInstance, 0);
          //  I'm assuming the same string constant will always be used!
          SetWindowFont(lbl, font_, false);
          SetWindowFont(btn, font_, false);
          ::SetWindowLong(btn, GWL_ID, WindowId(id));
          i.row++;
        }
        if (i.row >= numRows) {
          i.column++;
          i.row = 0;
        }
        (*ptr).second = i;
        return true;
      }
    HWND dlg_;
    HFONT font_;
    struct Info {
      Info() { frame = 0; column = 0; row = 0; }
      HWND frame;
      int column;
      int row;
    };
    std::map<std::string, Info> groups_;
    IGameExporter * gex_;
};

class GetSettingsVisitor {
  public:
    HWND dlg_;
    int lastItem_;
    std::string lastItemName_;
    GetSettingsVisitor(HWND dlg) : dlg_(dlg) {}
    bool operator()(char const * group, bool & b, char const * name, bool v, char const * title, int id)
      {
        lastItem_ = WindowId(id);
        lastItemName_ = title;
        HWND t = ::FindWindowEx(dlg_, 0, WC_BUTTON, group);
        if (t == 0) t = dlg_;
        HWND i = ::GetDlgItem(t, WindowId(id));
        assert(i != 0);
        b = (::IsDlgButtonChecked(t, WindowId(id)) != 0);
        return true;
      }
    bool operator()(char const * group, float & b, char const * name, float v, char const * title, int id)
      {
        lastItem_ = WindowId(id);
        lastItemName_ = title;
        HWND t = ::FindWindowEx(dlg_, 0, WC_BUTTON, group);
        if (t == 0) t = dlg_;
        HWND i = ::GetDlgItem(t, WindowId(id));
        assert(i != 0);
        char text[512];
        ::GetWindowText(i, text, 512);
        text[511] = 0;
        if (sscanf(text, "%f", &b) != 1) {
          return false;
        }
        //  there's a minimum and maximum value for floats
        if (b < 1e-3 || b > 1e3) {
          return false;
        }
        return true;
      }
    bool operator()(char const * group, int & b, char const * name, int v, char const * title, int id)
      {
        lastItem_ = WindowId(id);
        lastItemName_ = title;
        HWND t = ::FindWindowEx(dlg_, 0, WC_BUTTON, group);
        if (t == 0) t = dlg_;
        HWND i = ::GetDlgItem(t, WindowId(id));
        assert(i != 0);
        char text[512];
        ::GetWindowText(i, text, 512);
        text[511] = 0;
        return sscanf(text, "%d", &b) == 1;
      }
    bool operator()(char const * group, std::string & b, char const * name, std::string v, char const * title, int id)
      {
        HWND t = ::FindWindowEx(dlg_, 0, WC_BUTTON, group);
        if (t == 0) t = dlg_;
        HWND i = ::GetDlgItem(t, WindowId(id));
        assert(i != 0);
        lastItem_ = WindowId(id);
        lastItemName_ = title;
        char text[512];
        text[511] = 0;
        if (!strcmp(name, ANIMATIONS_LIST_VIEW)) {
          int ni = ListView_GetItemCount(i);
          sprintf(text, "%d", ni);
          b = text;
          b += ";";
          for (int z = 0; z < ni; ++z) {
            ListView_GetItemText(i, z, 0, text, 511);
            b += convert_name(text);
            b += ",";
            ListView_GetItemText(i, z, 1, text, 511);
            b += text;
            b += ",";
            ListView_GetItemText(i, z, 2, text, 511);
            b += text;
            b += ",";
            ListView_GetItemText(i, z, 3, text, 511);
            b += text;
            b += ";";
          }
        }
        else {
          ::GetWindowText(i, text, 511);
          b = text;
        }
        return true;
      }
};

INT_PTR CALLBACK SettingsProc(HWND hWndDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
  IGameExporter * gex = (IGameExporter *)::GetWindowLongPtr(hWndDlg, GWLP_USERDATA);
  HWND parent = ::FindWindowEx(hWndDlg, 0, WC_BUTTON, "Animation");
  HWND list = ::GetDlgItem(parent, IDC_ANIMATIONS);
  char buf[512];
  switch (msg) {
    case WM_INITDIALOG:
      gex = (IGameExporter *)lParam;
      ::SetWindowLongPtr(hWndDlg, GWLP_USERDATA, lParam);
      Visit(*gex, MakeControlsVisitor(hWndDlg, gex));
      ::SetWindowTextInt(::GetDlgItem(hWndDlg, IDC_FRAMERATE), GetFrameRate());
      ::SetDlgItemText(hWndDlg, IDC_CREDITS, 
          "kW X-port by Jon Watte; built " __DATE__ 
          "\nhttp://www.kwport.org/");
      return TRUE;
    case WM_CLOSE:
      ::EndDialog(hWndDlg, 1);
      return TRUE;
    case WM_NOTIFY: {
        NMHDR const * nmh = (NMHDR const *)lParam;
        if (nmh->idFrom == IDC_ANIMATIONS) {
          switch (nmh->code) {
            case LVN_ITEMACTIVATE: {
                NMITEMACTIVATE const * nmi = (NMITEMACTIVATE const *)nmh;
                selectedAnimation_ = nmi->iItem;
                buf[511] = 0;
                ListView_GetItemText(list, selectedAnimation_, 0, buf, 511);
                ::SetWindowText(::GetDlgItem(parent, IDC_ANIMATIONNAME), buf);
                ListView_GetItemText(list, selectedAnimation_, 1, buf, 511);
                ::SetWindowText(::GetDlgItem(parent, IDC_FIRSTFRAME), buf);
                ListView_GetItemText(list, selectedAnimation_, 2, buf, 511);
                ::SetWindowText(::GetDlgItem(parent, IDC_NUMFRAMES), buf);
                ListView_GetItemText(list, selectedAnimation_, 3, buf, 511);
                ::SetWindowText(::GetDlgItem(parent, IDC_STRETCHTIME), buf);
              }
              break;
            default:
              break;
          }
        }
      }
      break;
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDB_ADD: {
            Settings s;
            if (!Visit(s, GetSettingsVisitor(hWndDlg))) {
              ::MessageBox(hWndDlg, "Please enter valid data before continuing.", "kW X-port", MB_OK);
              return 0;
            }
            s.animationName_ = convert_name(s.animationName_.c_str());
            LVITEM lvi;
            lvi.mask = LVIF_TEXT;
            lvi.iItem = 0;
            lvi.iSubItem = 0;
            lvi.pszText = (LPSTR)s.animationName_.c_str();
            HWND parent = ::FindWindowEx(hWndDlg, 0, WC_BUTTON, "Animation");
            HWND list = ::GetDlgItem(parent, IDC_ANIMATIONS);
            int itm = (int)::SendMessage(list, LVM_INSERTITEM, 0, (LPARAM)&lvi);
            if (itm >= 0) {
              char b[40];
              sprintf(b, "%d", s.firstFrame_);
              lvi.mask = LVIF_TEXT;
              lvi.iItem = itm;
              lvi.iSubItem = 1;
              lvi.pszText = b;
              ::SendMessage(list, LVM_SETITEM, 0, (LPARAM)&lvi);
              sprintf(b, "%d", s.numFrames_);
              lvi.mask = LVIF_TEXT;
              lvi.iItem = itm;
              lvi.iSubItem = 2;
              lvi.pszText = b;
              ::SendMessage(list, LVM_SETITEM, 0, (LPARAM)&lvi);
              sprintf(b, "%g", s.timeStretch_);
              lvi.mask = LVIF_TEXT;
              lvi.iItem = itm;
              lvi.iSubItem = 3;
              lvi.pszText = b;
              ::SendMessage(list, LVM_SETITEM, 0, (LPARAM)&lvi);
            }
            selectedAnimation_ = ListView_GetSelectionMark(list);
          }
          break;
        case IDB_UPDATE: {
            selectedAnimation_ = ListView_GetSelectionMark(list);
            if (selectedAnimation_ < 0 || selectedAnimation_ >= ListView_GetItemCount(list)) {
              ::MessageBox(hWndDlg, "Please select an animation first.", "kW X-port", MB_OK);
              return 0;
            }
            Settings s;
            if (!Visit(s, GetSettingsVisitor(hWndDlg))) {
              ::MessageBox(hWndDlg, "Please enter valid data before continuing.", "kW X-port", MB_OK);
              return 0;
            }
            s.animationName_ = convert_name(s.animationName_.c_str());
            LVITEM lvi;
            lvi.mask = LVIF_TEXT;
            lvi.iItem = selectedAnimation_;
            lvi.iSubItem = 0;
            lvi.pszText = (LPSTR)s.animationName_.c_str();
            HWND parent = ::FindWindowEx(hWndDlg, 0, WC_BUTTON, "Animation");
            HWND list = ::GetDlgItem(parent, IDC_ANIMATIONS);
            ::SendMessage(list, LVM_SETITEM, 0, (LPARAM)&lvi);
            char b[40];
            sprintf(b, "%d", s.firstFrame_);
            lvi.mask = LVIF_TEXT;
            lvi.iItem = selectedAnimation_;
            lvi.iSubItem = 1;
            lvi.pszText = b;
            ::SendMessage(list, LVM_SETITEM, 0, (LPARAM)&lvi);
            sprintf(b, "%d", s.numFrames_);
            lvi.mask = LVIF_TEXT;
            lvi.iItem = selectedAnimation_;
            lvi.iSubItem = 2;
            lvi.pszText = b;
            ::SendMessage(list, LVM_SETITEM, 0, (LPARAM)&lvi);
            sprintf(b, "%g", s.timeStretch_);
            lvi.mask = LVIF_TEXT;
            lvi.iItem = selectedAnimation_;
            lvi.iSubItem = 3;
            lvi.pszText = b;
            ::SendMessage(list, LVM_SETITEM, 0, (LPARAM)&lvi);
          }
          break;
        case IDB_REMOVE: {
            selectedAnimation_ = ListView_GetSelectionMark(list);
            if (selectedAnimation_ < 0 || selectedAnimation_ >= ListView_GetItemCount(list)) {
              ::MessageBox(hWndDlg, "Please select an animation first.", "kW X-port", MB_OK);
              return 0;
            }
            ListView_DeleteItem(list, selectedAnimation_);
            selectedAnimation_ = -1;
          }
          break;
        case IDOK: {
            GetSettingsVisitor gsv(hWndDlg);
            if (!Visit(*gex, gsv)) {
              ::MessageBox(hWndDlg, (std::string("The field '") + gsv.lastItemName_ + 
                  "' contains an invalid value.").c_str(), "kW X-port", MB_OK);
            }
            else {
              ::EndDialog(hWndDlg, 0);
              SetMySettings(*gex);
            }
          }
          break;
        case IDCANCEL:
          ::EndDialog(hWndDlg, 1);
          break;
      }
      return FALSE;
    default:
      return FALSE;
  }
  return FALSE;
}

void GetDbgHelp()
{
  if (!dbgHelp) {
    dbgHelp = ::LoadLibrary("dbghelp.dll");
    if (dbgHelp != NULL) {
      *(void**)&WriteDumpFunc = (void *)::GetProcAddress(dbgHelp, "MiniDumpWriteDump");
    }
  }
}

int    IGameExporter::DoExport(const TCHAR *name, ExpInterface *,Interface *, BOOL suppressPrompts, DWORD options)
{
#if !OPTIMIZED
  ::MessageBox(0, "Not optimized!\nDon't forget to re-enable before ship.", "Not optimized!", MB_OK);
#endif
  GetDbgHelp();
  int ret = 0;
  __try {
    *static_cast<Settings *>(this) = GetMySettings();
    ::LogBegin(name, suppressPrompts != 0);
    ret = DoExport2(name, suppressPrompts, options);
    ::LogEnd();
    SetMySettings(*this);
  }
  __except(MiniDumpHandler(GetExceptionInformation())) {
    LogError("%s: Caught crash inside kW X-port.\n", name);
    ::LogEnd();
    if (!suppressPrompts) {
      ::MessageBox(0, "kW X-port crashed while exporting.\nPlease save your work, revert to a back-up\n"
          "copy, and try again.\nWe apologize for the inconvenience.\n", "kW X-port", MB_OK);
    }
  }
  return ret;
}

int    IGameExporter::DoExport2(const TCHAR *name, BOOL suppressPrompts, DWORD options)
{
  time_t t = 0;
  time(&t);
  strftime(exportTime_, sizeof(exportTime_)/sizeof(exportTime_[0]), "%Y-%m-%d %H:%M:%S", localtime(&t));

    core_ = GetCOREInterface();
    name_ = name;
    MyErrorProc pErrorProc;
    SetErrorCallBack(&pErrorProc);

    bool exportSelected = (options & SCENE_EXPORT_SELECTED) ? true : false;
    float igameVersion  = GetIGameVersion();

    LogPrint(_T("kW X-port: 3ds max compatible version %.2f%\n"),GetSupported3DSVersion());

    igame_ = GetIGameInterface();

    IGameConversionManager * cm = GetConversionManager();
    cm->SetCoordSystem(IGameConversionManager::IGAME_MAX);

    coreFrame_ = core_->GetTime();

  bool success = true;
  Settings old = *this;
  
  if (!suppressPrompts) {
    if (::DialogBoxParam(hInstance, MAKEINTRESOURCE(IDD_SETTINGS), 0, SettingsProc, 
        (LPARAM)this)) {
      //  restore previous settings
      *static_cast<Settings *>(this) = old;
      return true;
    }
  }

  scale = scaleNumer_ / scaleDenom_;

  gSelectedNodes.SetCount(0);
  coreFrame_ = core_->GetTime();
  if (exportSelected) {
    Interface * ip = GetCOREInterface();
    int snc = ip->GetSelNodeCount();
    for (int i = 0; i < snc; ++i) {
      INode *in = ip->GetSelNode(i);
      gSelectedNodes.Append(1, &in);
    }
    if (gSelectedNodes.Count() == 0) {
      ::OutputDebugString("There are no selected nodes to export.\n");
      if (!suppressPrompts) {
        ::MessageBox(0, "There are no selected nodes.", "kW X-port Error", MB_OK);
      }
      success = false;
      goto done;
    }
  }
  ::OutputDebugString("Initialize IGame.\n");
  igame_->InitialiseIGame(false);
  igame_->SetStaticFrame(coreFrame_);

  try {
    assert(_CrtCheckMemory());
    ::OutputDebugString("kW X-port begin writing X file\n");
    yUp = yUp_;
    rightHanded = rightHanded_;
    flipWinding = flipWinding_;
    WriteXFile(igame_, name);
    theHold.Begin();
    theHold.Put(new NullRestoreObj());
    theHold.Accept(_T("Export Settings"));
    ::OutputDebugString("kW X-port done writing X file\n");
    assert(_CrtCheckMemory());
  }
  catch(char const * error) {
#ifndef NDEBUG
   ::OutputDebugString(error);
#endif
    theHold.Cancel();
    ::MessageBox(0, error, "Error Writing X File", MB_OK);
    success = false;
  }
  catch(std::exception const &x) {
#ifndef NDEBUG
   ::OutputDebugString(x.what());
#endif
    theHold.Cancel();
    ::MessageBox(0, x.what(), "Error Writing X File", MB_OK);
    success = false;
  }

done:

  ::OutputDebugString("Releasing IGame.\n");
  /*  It turns out that calling ReleaseIGame on an IGame instance may crash 3ds Max in version 2010!
      This is acknowledged by Autodesk, but they appear in no hurry to fix this bug (nor the bug 
      where a skinned object can't have more than one modifier below it on the stack).
      Thus, I have to work around it by leaking memory. Eek!
    */
#if !IGAME_BUG
  igame_->ReleaseIGame();
#endif
  core_->ProgressEnd();    

  ::OutputDebugString("kW X-port returning success.\n");
  assert(_CrtCheckMemory());
  return success;
}

std::string SettingsFilename(Interface *core)
{
  TCHAR const * dir = core->GetDir(APP_PLUGCFG_DIR);
  string s(dir);
  s += "\\kwxport.ini";
  return s;
}

class DocSettingsVisitor {
  public:
    std::map<string, string> & s_;
    DocSettingsVisitor(std::map<string, string> & s) : s_(s) {}
    bool operator()(char const * group, bool & b, char const * name, bool v, char const * title, int id)
      {
        std::map<string, string>::iterator ptr = s_.find(name);
        if (ptr != s_.end()) {
          int i = 0;
          if (1 == sscanf((*ptr).second.c_str(), "%d", &i)) {
            b = i != 0;
          }
        }
        return true;
      }
    bool operator()(char const * group, int & b, char const * name, int v, char const * title, int id)
      {
        std::map<string, string>::iterator ptr = s_.find(name);
        if (ptr != s_.end()) {
          int i = 0;
          if (1 == sscanf((*ptr).second.c_str(), "%d", &i)) {
            b = i;
          }
        }
        return true;
      }
    bool operator()(char const * group, float & b, char const * name, float v, char const * title, int id)
      {
        std::map<string, string>::iterator ptr = s_.find(name);
        if (ptr != s_.end()) {
          float i = 0;
          if (1 == sscanf((*ptr).second.c_str(), "%f", &i)) {
            b = i;
          }
        }
        return true;
      }
    bool operator()(char const * group, std::string & b, char const * name, std::string v, char const * title, int id)
      {
        std::map<string, string>::iterator ptr = s_.find(name);
        if (ptr != s_.end()) {
          b = (*ptr).second;
        }
        return true;
      }
};


bool ReadSettingsFromFile(std::map<std::string, std::string> &settings, char const *filename)
{
  FILE * f = fopen(filename, "rb");
  if (f) {
    char line[1024];
    while (true) {
      line[0] = 0;
      fgets(line, 1024, f);
      line[1023] = 0;
      if (!line[0]) break;
      char * x = strrchr(line, '\n');
      if (x) *x = 0;
      char * key = line;
      char * val = strchr(key, '=');
      if (val != 0) {
        *val = 0;
        ++val;
        settings[key] = val;
      }
    }
    fclose(f);
    return true;
  }
  return false;
}

void ReadSettingsFromProperties(std::map<string, string> &settings, Interface *core)
{
  int np = core->GetNumProperties(PROPSET_USERDEFINED);
  for (int i = 0; i < np; ++i) {
    PROPSPEC const * ps = core->GetPropertySpec(PROPSET_USERDEFINED, i);
    if (ps->ulKind == PRSPEC_LPWSTR) {
      PROPVARIANT const * pv = core->GetPropertyVariant(PROPSET_USERDEFINED, i);
      string propName = wtocstr(ps->lpwstr);
      char str[1024];
      str[0] = 0;
      switch (pv->vt) {
        case VT_INT:
          sprintf(str, "%d", pv->intVal);
          break;
        case VT_I4:
          sprintf(str, "%d", pv->lVal);
          break;
        case VT_I8:
          sprintf(str, "%lld", pv->hVal);
          break;
        case VT_UINT:
          sprintf(str, "%u", pv->uintVal);
          break;
        case VT_UI4:
          sprintf(str, "%u", pv->ulVal);
          break;
        case VT_UI8:
          sprintf(str, "%llu", pv->uhVal);
          break;
        case VT_R4:
          sprintf(str, "%g", pv->fltVal);
          break;
        case VT_R8:
          sprintf(str, "%g", (float)pv->dblVal);
          break;
        case VT_BOOL:
          sprintf(str, "%d", pv->boolVal ? 1 : 0);
          break;
        case VT_LPSTR:
          _snprintf(str, 1024, "%s", (char const *)pv->pszVal);
          str[1023] = 0;
          break;
        default:
          LogPrint("Variant %s uses unknown variant type %d\n",
              propName.c_str(), pv->vt);
          break;
      }
      if (str[0]) {
        settings[propName] = str;
      }
    }
  }
}

//  Settings can live in one of three places:
//  1) The defaults specified where the Visit() function is defined.
//  2) The kwxport.ini plugin config settings file, which is updated 
//    with the settings of whatever the last export is.
//  3) The Max file itself, where settings are stored as file user props.
//  The end result is that new files will inherit whatever the settings 
//  were the last time an export was made, and previously exported files 
//  will retain their export settings.
//
void DoLoadSettings(Settings &set, Interface *core)
{
  std::map<string, string> settings;
  ReadSettingsFromFile(settings, SettingsFilename(core).c_str());
  ReadSettingsFromProperties(settings, core);
  Visit(set, DocSettingsVisitor(settings));
}

bool DoLoadSettings(Settings &set, char const *filename)
{
  std::map<string, string> settings;
  if (!ReadSettingsFromFile(settings, filename))
    return false;
  Visit(set, DocSettingsVisitor(settings));
  return true;
}


void IGameExporter::LoadSettings()
{
  DoLoadSettings(*this, core_);
}

class SaveSettingsVisitor {
  public:
    SaveSettingsVisitor(FILE * f, Interface *core = 0) : f_(f), core_(core) {}
    bool operator()(char const *, bool & b, char const * name, bool, char const *, int)
      {
        if (f_ != NULL)
        {
          fprintf(f_, "%s=%d\n", name, b ? 1 : 0);
        }
        PROPVARIANT pv;
        pv.vt = VT_BOOL;
        pv.boolVal = b;
        ReplaceProperty(name, pv);
        return true;
      }
    bool operator()(char const *, int & b, char const * name, int, char const *, int)
      {
        if (f_ != NULL)
        {
          fprintf(f_, "%s=%d\n", name, b);
        }
        PROPVARIANT pv;
        pv.vt = VT_I4;
        pv.lVal = b;
        ReplaceProperty(name, pv);
        return true;
      }
    bool operator()(char const *, float & b, char const * name, float, char const *, int)
      {
        if (f_ != NULL)
        {
          fprintf(f_, "%s=%g\n", name, b);
        }
        PROPVARIANT pv;
        pv.vt = VT_R4;
        pv.fltVal = b;
        ReplaceProperty(name, pv);
        return true;
      }
    bool operator()(char const *, std::string & b, char const * name, std::string, char const *, int)
      {
        if (f_ != NULL)
        {
          fprintf(f_, "%s=%s\n", name, b.c_str());
        }
        PROPVARIANT pv;
        pv.vt = VT_LPSTR;
        pv.pszVal = (LPSTR)b.c_str();
        ReplaceProperty(name, pv);
        return true;
      }
    void ReplaceProperty(char const * name, PROPVARIANT & pv)
      {
        if (!core_)
          return;
        PROPSPEC ps;
        wchar_t wstr[1024];
        mbstowcs(wstr, name, 1024);
        ps.ulKind = PRSPEC_LPWSTR;
        ps.lpwstr = wstr;
        core_->DeleteProperty(PROPSET_USERDEFINED, &ps);
        core_->AddProperty(PROPSET_USERDEFINED, &ps, &pv);
      }
    FILE * f_;
    Interface *core_;
};

bool DoSaveSettings(Settings &set, char const *filename, Interface *core)
{
  FILE * f = (filename == NULL) ? NULL : fopen(filename, "wb");
  if (f || filename == NULL) {
    Visit(set, SaveSettingsVisitor(f, core));
    if (f != NULL)
    {
      fclose(f);
    }
    return true;
  }
  return false;
}

void DoSaveSettings(Settings &set, Interface *core, char const *exportTime)
{
  if (DoSaveSettings(set, SettingsFilename(core).c_str(), core))
  {
    if (exportTime != NULL)
    {
      PROPSPEC ps;
      ps.ulKind = PRSPEC_LPWSTR;
      ps.lpwstr = L"lastExport";
      PROPVARIANT pv;
      pv.vt = VT_LPSTR;
      pv.pszVal = (LPSTR)exportTime;
      core->DeleteProperty(PROPSET_USERDEFINED, &ps);
      core->AddProperty(PROPSET_USERDEFINED, &ps, &pv);
    }
  }
}

void IGameExporter::SaveSettings()
{
  DoSaveSettings(*this, core_, exportTime_);
}




LRESULT __declspec(dllexport) __stdcall PluginCommit()
{
  //  called when installer is done committing installation
  ::ShellExecute(0, "open", "..\\d3dx_34\\DirectX End User EULA.txt", 0, 0, SW_SHOW);
  ::ShellExecute(0, "open", "http://kwxport.sourceforge.net/", 0, 0, SW_SHOW);
  return 0;
}


void assert_failure(char const *file, int line, char const *expr)
{
   char buf[1024];
   _snprintf(buf, 1024, "%s:%d: Assertion failed\n%s\n", file, line, expr);
   buf[1023] = 0;
   throw std::exception(buf);
}
