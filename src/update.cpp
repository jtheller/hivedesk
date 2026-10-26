#include "hivedesk/update.h"
#include "update_key.h"
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hive::update {
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
struct Internet {HINTERNET value=nullptr;~Internet(){if(value)WinHttpCloseHandle(value);}};
struct Algorithm {BCRYPT_ALG_HANDLE value=nullptr;~Algorithm(){if(value)BCryptCloseAlgorithmProvider(value,0);}};
struct Hash {BCRYPT_HASH_HANDLE value=nullptr;~Hash(){if(value)BCryptDestroyHash(value);}};
struct Key {BCRYPT_KEY_HANDLE value=nullptr;~Key(){if(value)BCryptDestroyKey(value);}};
std::vector<uint8_t> unbase64(const char* text){
    DWORD size=0;require(CryptStringToBinaryA(text,0,CRYPT_STRING_BASE64,nullptr,&size,nullptr,nullptr)!=FALSE,"Bad public key");
    std::vector<uint8_t> bytes(size);require(CryptStringToBinaryA(text,0,CRYPT_STRING_BASE64,bytes.data(),&size,nullptr,nullptr)!=FALSE,"Bad public key");bytes.resize(size);return bytes;
}
std::array<uint8_t,32> digest(const std::vector<uint8_t>& data){
    Algorithm alg;require(BCryptOpenAlgorithmProvider(&alg.value,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 unavailable");
    std::array<uint8_t,32> result{};require(BCryptHash(alg.value,nullptr,0,const_cast<PUCHAR>(data.data()),ULONG(data.size()),result.data(),ULONG(result.size()))>=0,"SHA256 failed");return result;
}
bool number(std::string_view text,uint32_t& value){
    if(text.empty()||text.size()>7||(text.size()>1&&text.front()=='0'))return false;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);return parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()&&value<=1000000;
}
template<class Sink>void request(const std::wstring& url,uint64_t limit,Sink sink){
    URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
    require(WinHttpCrackUrl(url.c_str(),0,0,&parts)!=FALSE&&parts.nScheme==INTERNET_SCHEME_HTTPS&&parts.nPort==443,"Update URL must be HTTPS");
    const std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath,parts.dwUrlPathLength);if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
    Internet session{WinHttpOpen(L"HiveDesk-Updater/1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    require(session.value!=nullptr,"HTTP initialization failed");WinHttpSetTimeouts(session.value,15000,15000,15000,30000);
    Internet connection{WinHttpConnect(session.value,host.c_str(),443,0)};require(connection.value!=nullptr,"HTTP connect failed");
    Internet req{WinHttpOpenRequest(connection.value,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    require(req.value!=nullptr,"HTTP request failed");
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;WinHttpSetOption(req.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects));
    require(WinHttpSendRequest(req.value,L"Cache-Control: no-cache\r\n",DWORD(-1),nullptr,0,0,0)&&WinHttpReceiveResponse(req.value,nullptr),"Release download failed");
    DWORD status=0,size=sizeof(status);require(WinHttpQueryHeaders(req.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr)&&status==200,"Release HTTP status is not 200");
    uint64_t total=0;std::array<uint8_t,65536> buffer{};const auto started=GetTickCount64();
    for(;;){DWORD count=0;require(WinHttpReadData(req.value,buffer.data(),DWORD(buffer.size()),&count)!=FALSE,"Release read failed");if(!count)break;
        total+=count;require(total<=limit&&GetTickCount64()-started<10*60*1000,"Release download exceeded size/time bound");sink(buffer.data(),count);}
}
HANDLE object(const wchar_t* name,bool mutex,bool users){
    // Users may observe the event, and briefly acquire the startup barrier.
    const wchar_t* descriptor=users?L"O:SYG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00120001;;;AU)":
        L"O:SYG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00120000;;;AU)";
    PSECURITY_DESCRIPTOR security=nullptr;require(ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor,SDDL_REVISION_1,&security,nullptr)!=FALSE,"Update object ACL failed");
    SECURITY_ATTRIBUTES attrs{sizeof(attrs),security,FALSE};HANDLE handle=mutex?CreateMutexW(&attrs,FALSE,name):CreateEventW(&attrs,TRUE,FALSE,name);LocalFree(security);
    require(handle!=nullptr,"Update coordination object unavailable");if(!system_owned(handle)){CloseHandle(handle);throw std::runtime_error("Update coordination object has wrong owner");}return handle;
}
}
bool parse_version(const std::string& text,Version& version){
    version={};std::string_view s=text;const auto dash=s.find('-');const auto base=s.substr(0,dash);size_t at=0;
    for(size_t i=0;i<3;++i){const auto end=i==2?base.size():base.find('.',at);if(end==std::string_view::npos||!number(base.substr(at,end-at),version.parts[i]))return false;at=end+1;}
    version.parts[3]=3;
    if(dash!=std::string_view::npos){const auto pre=s.substr(dash+1);const auto dot=pre.find('.');if(dot==std::string_view::npos)return false;
        const auto kind=pre.substr(0,dot);if(kind=="alpha")version.parts[3]=0;else if(kind=="beta")version.parts[3]=1;else if(kind=="rc")version.parts[3]=2;else return false;
        if(!number(pre.substr(dot+1),version.parts[4]))return false;}
    return true;
}
Manifest parse_manifest(const std::vector<uint8_t>& data){
    require(!data.empty()&&data.size()<=4096,"Manifest size invalid");const std::string text(data.begin(),data.end());
    require(text.back()=='\n'&&text.find('\r')==std::string::npos&&text.find('\0')==std::string::npos,"Manifest encoding invalid");
    std::istringstream input(text);std::string line;std::getline(input,line);require(line=="HiveDesk-Update-1","Manifest schema invalid");
    const auto field=[&](const char* name){std::string next;require(bool(std::getline(input,next)),"Missing manifest field");const std::string prefix=std::string(name)+"=";require(next.starts_with(prefix),"Unexpected manifest field");return next.substr(prefix.size());};
    Manifest m;m.version=field("version");m.file=field("file");const auto size=field("size");m.sha256=field("sha256");require(!std::getline(input,line),"Extra manifest data");
    Version parsed;require(parse_version(m.version,parsed),"Invalid release version");
    require(m.file=="HiveDesk-"+m.version+"-windows-x64-Setup.exe","Invalid release filename");
    const auto number=std::from_chars(size.data(),size.data()+size.size(),m.size);
    require(number.ec==std::errc{}&&number.ptr==size.data()+size.size()&&m.size>0&&m.size<=256*1024*1024,"Invalid installer size");
    require(m.sha256.size()==64&&std::all_of(m.sha256.begin(),m.sha256.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}),"Invalid SHA256");return m;
}
Manifest verify_manifest(const std::vector<uint8_t>& data,const std::vector<uint8_t>& signature){
    require(data.size()<=4096,"Manifest too large");auto modulus=unbase64(PublicModulus),exponent=unbase64(PublicExponent);
    require(signature.size()==modulus.size(),"Invalid release signature length");
    BCRYPT_RSAKEY_BLOB header{BCRYPT_RSAPUBLIC_MAGIC,ULONG(modulus.size()*8),ULONG(exponent.size()),ULONG(modulus.size()),0,0};
    std::vector<uint8_t> blob(sizeof(header));memcpy(blob.data(),&header,sizeof(header));blob.insert(blob.end(),exponent.begin(),exponent.end());blob.insert(blob.end(),modulus.begin(),modulus.end());
    Algorithm alg;Key key;require(BCryptOpenAlgorithmProvider(&alg.value,BCRYPT_RSA_ALGORITHM,nullptr,0)>=0&&BCryptImportKeyPair(alg.value,nullptr,BCRYPT_RSAPUBLIC_BLOB,&key.value,blob.data(),ULONG(blob.size()),0)>=0,"Release public key unavailable");
    auto hash=digest(data);BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    require(BCryptVerifySignature(key.value,&padding,hash.data(),ULONG(hash.size()),const_cast<PUCHAR>(signature.data()),ULONG(signature.size()),BCRYPT_PAD_PKCS1)>=0,"Release signature verification failed");
    return parse_manifest(data);
}
std::string hash_file(const std::filesystem::path& path){
    Handle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};require(file.value!=INVALID_HANDLE_VALUE,"Cannot read installer");
    Algorithm alg;Hash hash;require(BCryptOpenAlgorithmProvider(&alg.value,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&BCryptCreateHash(alg.value,&hash.value,nullptr,0,nullptr,0,0)>=0,"File SHA256 initialization failed");
    std::array<uint8_t,65536> data{};for(;;){DWORD n=0;require(ReadFile(file.value,data.data(),DWORD(data.size()),&n,nullptr)!=FALSE,"Installer read failed");if(!n)break;require(BCryptHashData(hash.value,data.data(),n,0)>=0,"File SHA256 failed");}
    std::array<uint8_t,32> value{};require(BCryptFinishHash(hash.value,value.data(),ULONG(value.size()),0)>=0,"SHA256 completion failed");std::string hex;for(auto byte:value){hex+="0123456789abcdef"[byte>>4];hex+="0123456789abcdef"[byte&15];}return hex;
}
void verify_package(const Manifest& m,const std::filesystem::path& path){require(std::filesystem::file_size(path)==m.size,"Installer size mismatch");require(hash_file(path)==m.sha256,"Installer SHA256 mismatch");}
std::vector<uint8_t> download(const std::wstring& url,size_t limit){std::vector<uint8_t> data;request(url,limit,[&](const uint8_t* p,DWORD n){data.insert(data.end(),p,p+n);});return data;}
void download_file(const std::wstring& url,const std::filesystem::path& path,uint64_t expected){
    std::ofstream file(path,std::ios::binary|std::ios::trunc);require(bool(file),"Cannot create staged installer");uint64_t total=0;
    request(url,expected,[&](const uint8_t* p,DWORD n){file.write(reinterpret_cast<const char*>(p),n);require(bool(file),"Cannot write staged installer");total+=n;});file.close();require(total==expected,"Truncated installer download");
}
std::wstring release_url(const Manifest& m){const std::string url="https://github.com/jtheller/hivedesk/releases/download/v"+m.version+"/"+m.file;return {url.begin(),url.end()};}
std::filesystem::path module_directory(){wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);return std::filesystem::path(path).parent_path();}
std::filesystem::path installed_directory(){wchar_t path[32768]{};DWORD size=sizeof(path);if(RegGetValueW(HKEY_LOCAL_MACHINE,L"Software\\HiveDesk",L"InstallDir",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,path,&size)!=ERROR_SUCCESS)return {};return path;}
std::string installed_version(const std::filesystem::path& root){
    const auto exe=root/L"HiveDesk.exe";DWORD dummy=0,size=GetFileVersionInfoSizeW(exe.c_str(),&dummy);require(size>0,"Installed version unavailable");std::vector<uint8_t> data(size);
    require(GetFileVersionInfoW(exe.c_str(),0,size,data.data())!=FALSE,"Installed version unreadable");wchar_t* value=nullptr;UINT length=0;
    require(VerQueryValueW(data.data(),L"\\StringFileInfo\\040904B0\\ProductVersion",reinterpret_cast<void**>(&value),&length)&&value&&length>1,"Installed version missing");
    std::string version;for(const wchar_t c:std::wstring(value)){require(c>0&&c<128,"Installed version is not ASCII");version.push_back(static_cast<char>(c));}return version;
}
bool is_installed_agent(){const auto installed=installed_directory();return !installed.empty()&&_wcsicmp(installed.c_str(),module_directory().c_str())==0;}
bool is_system(){Handle token; if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))return false;BYTE info[1024]{};DWORD needed=0;return GetTokenInformation(token.value,TokenUser,info,sizeof(info),&needed)&&IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(info)->User.Sid,WinLocalSystemSid);}
bool system_owned(HANDLE handle){PSID owner=nullptr;PSECURITY_DESCRIPTOR security=nullptr;const auto result=GetSecurityInfo(handle,SE_KERNEL_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&security);const bool ok=result==ERROR_SUCCESS&&owner&&IsWellKnownSid(owner,WinLocalSystemSid);if(security)LocalFree(security);return ok;}
bool pending(){Handle event{OpenEventW(SYNCHRONIZE|READ_CONTROL,FALSE,PendingEvent)};return event.value&&system_owned(event.value)&&WaitForSingleObject(event.value,0)==WAIT_OBJECT_0;}
HANDLE create_system_event(const wchar_t* name){return object(name,false,false);}
HANDLE create_system_mutex(const wchar_t* name,bool users){return object(name,true,users);}
StartupGuard::StartupGuard(){
    if(!is_installed_agent())return;handle_=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE|READ_CONTROL,FALSE,StartupMutex);
    if(!handle_){allowed_=!pending();return;}if(!system_owned(handle_)){allowed_=false;return;}
    const auto wait=WaitForSingleObject(handle_,0);owned_=wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED;allowed_=owned_&&!pending();
}
void StartupGuard::release(){if(owned_){ReleaseMutex(handle_);owned_=false;}if(handle_){CloseHandle(handle_);handle_=nullptr;}}
StartupGuard::~StartupGuard(){release();}
std::filesystem::path private_cache(){
    PWSTR common=nullptr;require(SHGetKnownFolderPath(FOLDERID_ProgramData,0,nullptr,&common)==S_OK,"ProgramData unavailable");const auto path=std::filesystem::path(common)/L"HiveDeskUpdates";CoTaskMemFree(common);
    PSECURITY_DESCRIPTOR security=nullptr;require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&security,nullptr)!=FALSE,"Cache ACL failed");
    SECURITY_ATTRIBUTES attrs{sizeof(attrs),security,FALSE};const bool created=CreateDirectoryW(path.c_str(),&attrs)!=FALSE;const DWORD error=GetLastError();LocalFree(security);require(created||error==ERROR_ALREADY_EXISTS,"Cannot create update cache");
    require((GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)==0,"Update cache must not be a reparse point");
    PSID owner=nullptr;PSECURITY_DESCRIPTOR actual=nullptr;const auto result=GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&actual);
    const bool trusted=result==ERROR_SUCCESS&&owner&&IsWellKnownSid(owner,WinLocalSystemSid);if(actual)LocalFree(actual);require(trusted,"Update cache has wrong owner");
    // Restore protected SYSTEM/Administrators ACL even for an existing directory.
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&security,nullptr)!=FALSE,"Cache ACL failed");
    const bool secured=SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,security)!=FALSE;LocalFree(security);require(secured,"Cannot secure update cache");return path;
}
void log(const std::string& message){try{auto root=private_cache();const auto file=root/L"update.log";if(std::filesystem::exists(file)&&std::filesystem::file_size(file)>1024*1024)std::filesystem::remove(file);SYSTEMTIME now{};GetSystemTime(&now);char stamp[64]{};sprintf_s(stamp,"%04u-%02u-%02u %02u:%02u:%02u UTC ",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);std::ofstream out(file,std::ios::app);out<<stamp<<message<<'\n';}catch(...){}}
void launch_worker(HANDLE& process){
    if(process){if(WaitForSingleObject(process,0)==WAIT_TIMEOUT)return;CloseHandle(process);process=nullptr;}
    try{const auto root=module_directory();if(!is_system()||root!=installed_directory())return;const auto runner=private_cache()/(L"runner-"+std::wstring(L"" HIVEDESK_VERSION)+L".exe");
        if(!CopyFileW((root/L"HiveDeskUpdater.exe").c_str(),runner.c_str(),FALSE))throw std::runtime_error("Cannot stage updater runner");
        std::wstring command=L"\""+runner.wstring()+L"\"";STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION child{};
        require(CreateProcessW(runner.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&startup,&child)!=FALSE,"Cannot launch update worker");CloseHandle(child.hThread);process=child.hProcess;
    }catch(const std::exception& error){log(error.what());}
}
}
