#include "client.h"
#include <mutex>
// PixelLab native client (Windows and Linux). No embedded Python.
#include "vendor/json.hpp"
#include "vendor/miniz/miniz.h"
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb/stb_image_write.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(_WIN32) && !defined(PIXELLAB_TEST_MOCK)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#elif !defined(_WIN32) && !defined(PIXELLAB_TEST_MOCK)
#include <curl/curl.h>
#endif
using json=nlohmann::json; namespace fs=std::filesystem; using namespace std::chrono_literals;
using Bytes=std::vector<uint8_t>;
constexpr const char* VER="0.7.0-native-experimental";
const std::vector<std::string> DIRS={"south","south-east","east","north-east","north","north-west","west","south-west"};
static std::string text(const json& j,const char* k,std::string def=""){ if(!j.is_object()) return def;auto i=j.find(k);return i!=j.end()&&i->is_string()?i->get<std::string>():def; }
static json member(const json& j,const char* k,json fallback=nullptr){if(!j.is_object()||!j.contains(k)||j[k].is_null())return fallback;return j[k];}
static bool flag(const json& j,const char* k){auto x=member(j,k);return x.is_boolean()&&x.get<bool>();}
static int num(const json& j,const char* k,int def=0){auto x=member(j,k);return x.is_number()?x.get<int>():def;}
static std::string lower(std::string x){for(char& c:x)if(c>='A'&&c<='Z')c+=32;return x;}
static std::string direction(std::string x){x=lower(x);std::replace(x.begin(),x.end(),'_','-');return x;}
static bool isdir(const std::string& d){return std::find(DIRS.begin(),DIRS.end(),d)!=DIRS.end();}
static void idcheck(const std::string& s){static const std::regex re("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");if(!std::regex_match(s,re))throw std::runtime_error("Invalid ID");}
static std::string hex(const Bytes& b){static const char* h="0123456789abcdef";std::string s;s.resize(b.size()*2);for(size_t i=0;i<b.size();i++){s[i*2]=h[b[i]>>4];s[i*2+1]=h[b[i]&15];}return s;}
static Bytes unhex(const std::string& s){if(s.size()%2||s.size()>128*1024*1024)throw std::runtime_error("Invalid image data");Bytes b(s.size()/2);auto digit=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};for(size_t i=0;i<b.size();i++){int a=digit(s[i*2]),z=digit(s[i*2+1]);if(a<0||z<0)throw std::runtime_error("Invalid image data");b[i]=(a<<4)|z;}return b;}
static std::string base64(const Bytes& b){static const char*t="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string o;for(size_t i=0;i<b.size();i+=3){unsigned n=b[i]<<16;if(i+1<b.size())n|=b[i+1]<<8;if(i+2<b.size())n|=b[i+2];o+=t[(n>>18)&63];o+=t[(n>>12)&63];o+=i+1<b.size()?t[(n>>6)&63]:'=';o+=i+2<b.size()?t[n&63]:'=';}return o;}
struct HttpError:std::runtime_error{int status;HttpError(int s):std::runtime_error("PixelLab HTTP " +std::to_string(s)),status(s){}};
struct Reply{int status;std::map<std::string,std::string> headers;Bytes data;};
#if defined(PIXELLAB_TEST_MOCK)
static Reply request(const std::string&,const std::string&,const std::map<std::string,std::string>&,const Bytes&); // mock in test source
#elif defined(_WIN32)
static std::wstring wide(const std::string&s){if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);if(n<=0)throw std::runtime_error("Invalid UTF-8");std::wstring w(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),w.data(),n);return w;}
static std::string narrow(const std::wstring&s){if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);if(n<=0)throw std::runtime_error("Invalid HTTP response header");std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),out.data(),n,nullptr,nullptr);return out;}
struct Handle{HINTERNET h;explicit Handle(HINTERNET x):h(x){if(!h)throw std::runtime_error("Windows HTTP initialization failed");}~Handle(){if(h)WinHttpCloseHandle(h);}Handle(const Handle&)=delete;};
static Reply request(const std::string& method,const std::string& url,const std::map<std::string,std::string>& headers={},const Bytes& body={}){
 auto u=wide(url); URL_COMPONENTS c{};c.dwStructSize=sizeof(c);c.dwSchemeLength=(DWORD)-1;c.dwHostNameLength=(DWORD)-1;c.dwUrlPathLength=(DWORD)-1;c.dwExtraInfoLength=(DWORD)-1;
 if(!WinHttpCrackUrl(u.c_str(),0,0,&c)||c.nScheme!=INTERNET_SCHEME_HTTPS || !c.dwHostNameLength)throw std::runtime_error("Refusing invalid or non-HTTPS URL");
 std::wstring host(c.lpszHostName,c.dwHostNameLength),path(c.lpszUrlPath,c.dwUrlPathLength);if(c.dwExtraInfoLength)path.append(c.lpszExtraInfo,c.dwExtraInfoLength);if(path.empty())path=L"/";
 // Credentials are sent only to the hard-coded PixelLab host. Redirects are disabled.
 for(const auto& h:headers)if(lower(h.first)=="authorization" && (lower(narrow(host))!="api.pixellab.ai"||c.nPort!=443))throw std::runtime_error("Refusing to send credentials to this host");
 Handle sess(WinHttpOpen(L"PixelLab-Live-Cpp/0.7",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));WinHttpSetTimeouts(sess.h,15000,15000,60000,60000);
 Handle conn(WinHttpConnect(sess.h,host.c_str(),c.nPort,0));auto met=wide(method);Handle req(WinHttpOpenRequest(conn.h,met.c_str(),path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
 DWORD disable=WINHTTP_DISABLE_REDIRECTS;if(!WinHttpSetOption(req.h,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable)))throw std::runtime_error("Cannot set HTTP security options");
 std::wstring hs;for(const auto& [k,v]:headers){if(k.find_first_of("\r\n:")!=std::string::npos||v.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("Invalid HTTP header");hs+=wide(k)+L": "+wide(v)+L"\r\n";}
 if(body.size()>96*1024*1024)throw std::runtime_error("Request too large");
 if(!WinHttpSendRequest(req.h,hs.empty()?WINHTTP_NO_ADDITIONAL_HEADERS:hs.c_str(),(DWORD)hs.size(),body.empty()?WINHTTP_NO_REQUEST_DATA:(void*)body.data(),(DWORD)body.size(),(DWORD)body.size(),0)||!WinHttpReceiveResponse(req.h,nullptr))throw std::runtime_error("Network error connecting to PixelLab");
 Reply r;DWORD len=sizeof(DWORD),status=0;if(!WinHttpQueryHeaders(req.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&len,WINHTTP_NO_HEADER_INDEX))throw std::runtime_error("Cannot read HTTP status");r.status=(int)status;
 for(const auto& hn:{L"Mcp-Session-Id"}){DWORD l=0;WinHttpQueryHeaders(req.h,WINHTTP_QUERY_CUSTOM,hn,nullptr,&l,WINHTTP_NO_HEADER_INDEX);if(GetLastError()==ERROR_INSUFFICIENT_BUFFER && l<8192){std::wstring v(l/sizeof(wchar_t),L'\0');if(WinHttpQueryHeaders(req.h,WINHTTP_QUERY_CUSTOM,hn,v.data(),&l,WINHTTP_NO_HEADER_INDEX)){while(!v.empty()&&v.back()==L'\0')v.pop_back();r.headers[lower(narrow(hn))]=narrow(v);}}}
 for(;;){DWORD n=0;if(!WinHttpQueryDataAvailable(req.h,&n))throw std::runtime_error("Network read failed");if(!n)break;if(r.data.size()+n>96*1024*1024)throw std::runtime_error("Download too large");size_t old=r.data.size();r.data.resize(old+n);DWORD got=0;if(!WinHttpReadData(req.h,r.data.data()+old,n,&got))throw std::runtime_error("Network read failed");r.data.resize(old+got);}
 if(status<200||status>=300)throw HttpError(status);return r;
}
#else
// Linux in-process transport. Always verify TLS, never forward credentials on
// redirects, and only send Authorization to the PixelLab API's default port.
static Reply request(const std::string& method,const std::string& url,const std::map<std::string,std::string>& headers={},const Bytes& body={}){
 static std::once_flag init;std::call_once(init,[]{if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("HTTP initialization failed");});
 using Url=std::unique_ptr<CURLU,decltype(&curl_url_cleanup)>;Url parsed(curl_url(),curl_url_cleanup);
 if(!parsed||curl_url_set(parsed.get(),CURLUPART_URL,url.c_str(),0)!=CURLUE_OK)throw std::runtime_error("Invalid URL");
 auto part=[&](CURLUPart p,unsigned flags)->std::string {char* v=nullptr;auto rc=curl_url_get(parsed.get(),p,&v,flags);std::string out=rc==CURLUE_OK?v:"";curl_free(v);return out;};
 auto scheme=lower(part(CURLUPART_SCHEME,0));auto host=lower(part(CURLUPART_HOST,0));auto port=part(CURLUPART_PORT,CURLU_DEFAULT_PORT);
 if(scheme!="https"||host.empty())throw std::runtime_error("Refusing invalid or non-HTTPS URL");
 for(auto&[k,v]:headers){if(k.find_first_of("\r\n:")!=std::string::npos||v.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("Invalid HTTP header");if(lower(k)=="authorization"&&(host!="api.pixellab.ai"||port!="443"))throw std::runtime_error("Refusing to send credentials to this host");}
 if(body.size()>96*1024*1024)throw std::runtime_error("Request too large");
 using Curl=std::unique_ptr<CURL,decltype(&curl_easy_cleanup)>;Curl c(curl_easy_init(),curl_easy_cleanup);if(!c)throw std::runtime_error("HTTP initialization failed");
 struct HeaderList{curl_slist* p=nullptr;~HeaderList(){curl_slist_free_all(p);}} list;
 for(auto&[k,v]:headers){auto* p=curl_slist_append(list.p,(k+": "+v).c_str());if(!p)throw std::runtime_error("HTTP header allocation failed");list.p=p;}
 Reply r{};
 auto write=[](char* p,size_t a,size_t b,void* ctx)->size_t{if(a && b>SIZE_MAX/a)return 0;size_t n=a*b;auto* out=static_cast<Bytes*>(ctx);if(n>96*1024*1024-out->size())return 0;out->insert(out->end(),p,p+n);return n;};
 auto readheader=[](char* p,size_t a,size_t b,void* ctx)->size_t{if(a && b>SIZE_MAX/a)return 0;size_t n=a*b;std::string line(p,n);auto sep=line.find(':');if(sep!=std::string::npos&&lower(line.substr(0,sep))=="mcp-session-id"){auto v=line.substr(sep+1);while(!v.empty()&&std::isspace((unsigned char)v.back()))v.pop_back();auto start=v.find_first_not_of(" \t");if(start!=std::string::npos&&v.size()<8192)(*static_cast<std::map<std::string,std::string>*>(ctx))["mcp-session-id"]=v.substr(start);}return n;};
 curl_easy_setopt(c.get(),CURLOPT_URL,url.c_str());curl_easy_setopt(c.get(),CURLOPT_CUSTOMREQUEST,method.c_str());
 curl_easy_setopt(c.get(),CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(c.get(),CURLOPT_PROTOCOLS,(long)CURLPROTO_HTTPS);
 curl_easy_setopt(c.get(),CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(c.get(),CURLOPT_SSL_VERIFYHOST,2L);
 curl_easy_setopt(c.get(),CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(c.get(),CURLOPT_TIMEOUT,75L);curl_easy_setopt(c.get(),CURLOPT_NOSIGNAL,1L);
 curl_easy_setopt(c.get(),CURLOPT_USERAGENT,"PixelLab-Native-WIP/0.7");curl_easy_setopt(c.get(),CURLOPT_HTTPHEADER,list.p);
 curl_easy_setopt(c.get(),CURLOPT_WRITEFUNCTION,+write);curl_easy_setopt(c.get(),CURLOPT_WRITEDATA,&r.data);curl_easy_setopt(c.get(),CURLOPT_HEADERFUNCTION,+readheader);curl_easy_setopt(c.get(),CURLOPT_HEADERDATA,&r.headers);
 if(!body.empty()||method=="POST"){curl_easy_setopt(c.get(),CURLOPT_POSTFIELDS,body.empty()?"":reinterpret_cast<const char*>(body.data()));curl_easy_setopt(c.get(),CURLOPT_POSTFIELDSIZE_LARGE,(curl_off_t)body.size());}
 auto code=curl_easy_perform(c.get());if(code!=CURLE_OK)throw std::runtime_error("Network error connecting to PixelLab");long status=0;curl_easy_getinfo(c.get(),CURLINFO_RESPONSE_CODE,&status);r.status=(int)status;if(status<200||status>=300)throw HttpError((int)status);return r;
}
#endif
static Bytes strbytes(const std::string&s){return Bytes(s.begin(),s.end());}
static json parsebytes(const Bytes& b){return json::parse(b.begin(),b.end());}
struct Img {int w,h; Bytes rgba;};
static void checkdim(int w,int h){if(w<=0||h<=0||w>4096||h>4096||(int64_t)w*h>4096LL*4096)throw std::runtime_error("Invalid or oversized canvas");}
static Img decodepng(const Bytes& b){if(b.size()>8*1024*1024||b.size()<8||memcmp(b.data(),"\x89PNG\r\n\x1a\n",8))throw std::runtime_error("Invalid PNG");int w=0,h=0,n=0;if(!stbi_info_from_memory(b.data(),(int)b.size(),&w,&h,&n))throw std::runtime_error("Invalid PNG header");checkdim(w,h);auto p=stbi_load_from_memory(b.data(),(int)b.size(),&w,&h,&n,4);if(!p)throw std::runtime_error("Cannot decode PNG");Img x{w,h,Bytes(p,p+(size_t)w*h*4)};stbi_image_free(p);for(size_t i=0;i<x.rgba.size();i+=4)if(!x.rgba[i+3])x.rgba[i]=x.rgba[i+1]=x.rgba[i+2]=0;return x;}
// Compare canonical RGBA from the opened document against freshly fetched cloud
// pixels. Transparent RGB is irrelevant and can be rewritten by PNG encoders.
static bool samepixels(Bytes expected,const Bytes& actual){
 if(expected.size()!=actual.size() || expected.size()%4) return false;
 for(size_t i=0;i<expected.size();i+=4)
  if(!expected[i+3]) expected[i]=expected[i+1]=expected[i+2]=0;
 return expected==actual;
}
static void require_original(const json& item,const Bytes& remote){
 auto original=member(item,"original_hex");
 if(!original.is_string()) throw std::runtime_error("Missing original pixels; reopen the asset before saving");
 if(!samepixels(unhex(original.get<std::string>()),remote))
  throw std::runtime_error("PixelLab image changed since opening; no changes saved. Refresh and review.");
}
static void require_original_frames(const json& item,const std::vector<Img>& remote){
 auto original=member(item,"original_frames");
 if(!original.is_array() || original.size()!=remote.size())
  throw std::runtime_error("Original animation frames missing or changed; no changes saved. Refresh and review.");
 for(size_t i=0;i<remote.size();i++)
  if(!original[i].is_string() || !samepixels(unhex(original[i].get<std::string>()),remote[i].rgba))
   throw std::runtime_error("PixelLab animation changed since opening; no changes saved. Refresh and review.");
}
static Bytes encodepng(int w,int h,const Bytes& rgba){checkdim(w,h);if(rgba.size()!=(size_t)w*h*4)throw std::runtime_error("Image data has wrong size");Bytes out;auto fn=[](void*ctx,void*data,int n){auto*o=(Bytes*)ctx;auto p=(uint8_t*)data;o->insert(o->end(),p,p+n);};if(!stbi_write_png_to_func(fn,&out,w,h,4,rgba.data(),w*4))throw std::runtime_error("PNG encoding failed");return out;}
static json payload(const Img& i){return {{"w",i.w},{"h",i.h},{"hex",hex(i.rgba)}};}
static json palette(const std::vector<Img>& images){std::map<std::string,int> counts;for(auto& x:images)for(size_t i=0;i<x.rgba.size();i+=4)if(x.rgba[i+3]){std::string c((char*)&x.rgba[i],4);auto it=counts.find(c);if(it!=counts.end())it->second++;else if(counts.size()<65536)counts[c]=1;}std::vector<std::pair<std::string,int>> v(counts.begin(),counts.end());std::sort(v.begin(),v.end(),[](auto&a,auto&b){return a.second!=b.second?a.second>b.second:a.first<b.first;});json out=json::array();for(size_t i=0;i<v.size()&&i<255;i++)out.push_back(hex(Bytes(v[i].first.begin(),v[i].first.end())));return out;}
struct Archive{mz_zip_archive z{};explicit Archive(const Bytes& b){if(!mz_zip_reader_init_mem(&z,b.data(),b.size(),0))throw std::runtime_error("Invalid PixelLab ZIP export");}~Archive(){mz_zip_reader_end(&z);}Bytes get(const std::string& p,size_t limit=8*1024*1024){int i=mz_zip_reader_locate_file(&z,p.c_str(),nullptr,MZ_ZIP_FLAG_CASE_SENSITIVE);mz_zip_archive_file_stat st{};if(i<0||!mz_zip_reader_file_stat(&z,(mz_uint)i,&st)||st.m_uncomp_size>limit)throw std::runtime_error("Export file missing or too large");size_t n=0;auto ptr=(uint8_t*)mz_zip_reader_extract_to_heap(&z,(mz_uint)i,&n,0);if(!ptr)throw std::runtime_error("Could not read ZIP entry");Bytes b(ptr,ptr+n);mz_free(ptr);return b;}};
static void u16(Bytes&o,int v){o.push_back(v&255);o.push_back((v>>8)&255);}
static Bytes gif(int w,int h,const std::vector<Bytes>& frames,int delay){checkdim(w,h);if(frames.empty()||frames.size()>200||(uint64_t)w*h*4*frames.size()>32ULL*1024*1024)throw std::runtime_error("Animation is too large");std::map<std::string,int> colors;for(auto&f:frames){if(f.size()!=(size_t)w*h*4)throw std::runtime_error("Animation frame data has wrong size");for(size_t i=0;i<f.size();i+=4)if(f[i+3]>=128){std::string c((char*)&f[i],3);if(!colors.count(c)){if(colors.size()>=255)throw std::runtime_error("GIF supports at most 255 opaque colours");colors[c]=(int)colors.size()+1;}}}if(colors.size()>255)throw std::runtime_error("GIF supports at most 255 opaque colours");int bits=2;while((1<<bits)<(int)colors.size()+1)bits++;Bytes o={'G','I','F','8','9','a'};u16(o,w);u16(o,h);o.push_back(0x80|(bits-1));o.push_back(0);o.push_back(0);Bytes pal(3*(1<<bits),0);for(auto&[s,i]:colors)for(int k=0;k<3;k++)pal[i*3+k]=(uint8_t)s[k];o.insert(o.end(),pal.begin(),pal.end());Bytes loop={0x21,0xff,0x0b,'N','E','T','S','C','A','P','E','2','.','0',3,1,0,0,0};o.insert(o.end(),loop.begin(),loop.end());
 for(auto&f:frames){o.insert(o.end(),{0x21,0xf9,4,9});u16(o,std::max(1,std::min(delay,65535)));o.push_back(0);o.push_back(0);o.push_back(0x2c);u16(o,0);u16(o,0);u16(o,w);u16(o,h);o.push_back(0);
  // Simple valid GIF LZW: literal codes with frequent clears; avoids variable-width dictionary ambiguities.
  int clear=1<<bits,eoi=clear+1,width=bits+1,chunk=(1<<width)-clear-3;Bytes comp;uint32_t buf=0;int nbits=0;auto emit=[&](int c){buf|=(uint32_t)c<<nbits;nbits+=width;while(nbits>=8){comp.push_back(buf&255);buf>>=8;nbits-=8;}};
  int count=0;emit(clear);for(size_t i=0;i<f.size();i+=4){if(count>=chunk){emit(clear);count=0;}int idx=f[i+3]>=128?colors.at(std::string((char*)&f[i],3)):0;emit(idx);count++;}emit(eoi);if(nbits)comp.push_back(buf&255);o.push_back(bits);for(size_t i=0;i<comp.size();i+=255){int n=(int)std::min<size_t>(255,comp.size()-i);o.push_back(n);o.insert(o.end(),comp.begin()+i,comp.begin()+i+n);}o.push_back(0);
 }o.push_back(0x3b);return o;}
static std::string label(const json& c){std::string n=text(c,"name","Untitled");if(n.empty())n="Untitled";auto s=text(c,"state_name");return !s.empty()&&s!=n?n+" - "+s:n;}
static json skeleton(const json& raw,int w,int h){
 json list=json::array();if(raw.is_array())list=raw;else if(raw.is_object()){auto p=member(raw,"keypoints");if(p.is_object()||p.is_array())return skeleton(p,w,h);for(auto it=raw.begin();it!=raw.end();++it)if(it.value().is_object()){auto v=it.value();if(text(v,"label").empty()&&text(v,"name").empty())v["label"]=it.key();list.push_back(v);}}else return nullptr;
 struct Point{std::string name;double x,y,z;};std::vector<Point> pts;double mx=0;
 for(auto& p:list){if(!p.is_object()||!member(p,"x").is_number()||!member(p,"y").is_number())continue;auto n=text(p,"label",text(p,"name"));for(char&c:n){if(c=='_')c=' ';else c=(char)std::toupper((unsigned char)c);}double x=p["x"].get<double>(),y=p["y"].get<double>();if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>100000||std::abs(y)>100000)continue;mx=std::max(mx,std::max(x,y));pts.push_back({n,x,y,member(p,"z_index").is_number()?p["z_index"].get<double>():0});}
 if(pts.size()<2)return nullptr;Bytes out((size_t)w*h*4,0);std::map<std::string,std::pair<int,int>> by;json points=json::array();for(auto&p:pts){int x=(int)std::round(mx<=1.0?p.x*(w-1):p.x),y=(int)std::round(mx<=1.0?p.y*(h-1):p.y);x=std::clamp(x,-w,w*2);y=std::clamp(y,-h,h*2);by[p.name]={x,y};points.push_back({{"label",p.name},{"x",x},{"y",y},{"z",p.z}});}
 auto put=[&](int x,int y,int r,int g,int b){if(x>=0&&x<w&&y>=0&&y<h){auto i=((size_t)y*w+x)*4;out[i]=r;out[i+1]=g;out[i+2]=b;out[i+3]=255;}};
 std::vector<std::pair<std::string,std::string>> bones={{"NOSE","NECK"},{"NECK","RIGHT SHOULDER"},{"RIGHT SHOULDER","RIGHT ELBOW"},{"RIGHT ELBOW","RIGHT ARM"},{"NECK","LEFT SHOULDER"},{"LEFT SHOULDER","LEFT ELBOW"},{"LEFT ELBOW","LEFT ARM"},{"NECK","RIGHT HIP"},{"RIGHT HIP","RIGHT KNEE"},{"RIGHT KNEE","RIGHT LEG"},{"NECK","LEFT HIP"},{"LEFT HIP","LEFT KNEE"},{"LEFT KNEE","LEFT LEG"},{"NOSE","RIGHT EYE"},{"RIGHT EYE","RIGHT EAR"},{"NOSE","LEFT EYE"},{"LEFT EYE","LEFT EAR"}};
 for(auto&[a,b]:bones){if(!by.count(a)||!by.count(b))continue;auto [x,y]=by[a];auto [x1,y1]=by[b];int dx=std::abs(x1-x),dy=-std::abs(y1-y),sx=x<x1?1:-1,sy=y<y1?1:-1,e=dx+dy;bool right=(a+b).find("RIGHT")!=std::string::npos,left=(a+b).find("LEFT")!=std::string::npos;for(;;){put(x,y,right?255:left?60:255,right?140:left?170:230,right?40:left?255:60);if(x==x1&&y==y1)break;int e2=2*e;if(e2>=dy){e+=dy;x+=sx;}if(e2<=dx){e+=dx;y+=sy;}}}for(auto&[_,xy]:by)put(xy.first,xy.second,255,255,255);return {{"w",w},{"h",h},{"hex",hex(out)},{"points",points.dump()}};
}
static json rotationskeletons(const json& d){auto s=member(d,"skeletons",json::object());if(!s.is_object())return json::object();auto sub=member(s,"rotations");if(sub.is_object())s=sub;json result=json::object();for(auto i=s.begin();i!=s.end();++i)result[direction(i.key())]=i.value();return result;}
static std::string norm(std::string s){std::string o;for(unsigned char c:s){if(c>=128)o+=(char)c;else if(std::isalnum(c))o+=(char)std::tolower(c);}return o;}
static std::map<std::string,json> grouprows(const json& g){std::map<std::string,json> rows;auto x=member(g,"directions",json::array());if(x.is_array())for(auto&r:x)if(r.is_object()){auto d=direction(text(r,"direction"));if(isdir(d))rows[d]=r;}return rows;}
// Return only one-to-one names where direction sets and frame counts agree.
static std::map<std::string,std::string> matches(const json& exported,const json& groups){
 std::map<std::string,std::vector<std::string>> cand;if(!exported.is_object()||!groups.is_array())return {};
 for(auto it=exported.begin();it!=exported.end();++it){auto name=it.key();auto dirs=it.value();if(!dirs.is_object())continue;std::map<std::string,int> zd;for(auto z=dirs.begin();z!=dirs.end();++z)if(z.value().is_array()&&!z.value().empty())zd[direction(z.key())]=(int)z.value().size();if(zd.empty())continue;
  for(auto&g:groups){auto gid=lower(text(g,"animation_group_id"));try{idcheck(gid);}catch(...){continue;}auto rd=grouprows(g);if(zd.size()!=rd.size())continue;bool eq=true;for(auto&[d,n]:zd){if(!rd.count(d)){eq=false;break;}int count=num(rd[d],"frame_count",0);if(count&&count!=n){eq=false;break;}}if(!eq)continue;
   auto target=norm(name);bool nameok=false;for(auto s:{text(g,"display_name"),text(g,"animation_type")}){auto x=norm(s);std::vector<std::string> variants={x};if(x.rfind("custom",0)==0)variants.push_back(x.substr(6));for(auto&v:variants)if(!target.empty()&&(target==v||(name.size()==50&&target.size()>=20&&v.rfind(target,0)==0)))nameok=true;}if(nameok)cand[name].push_back(gid);
  }
 }
 std::map<std::string,std::string> result;for(auto&[name,ids]:cand){std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());if(ids.size()!=1)continue;int n=0;for(auto&[_,others]:cand)if(std::find(others.begin(),others.end(),ids[0])!=others.end())n++;if(n==1)result[name]=ids[0];}return result;
}
using Progress=std::function<void(const std::string&)>;
struct UndoItem{std::string dir,target,mime;Bytes bytes;};
class Service {
 std::string token,session;json characters=nullptr;std::map<std::string,json> details;std::map<std::string,Bytes> zips;std::map<std::string,std::map<std::string,Bytes>> rotations;std::vector<UndoItem> undo;std::string undochar;
 std::map<std::string,std::string> auth()const{if(token.empty())throw std::runtime_error("Not connected");return {{"Authorization","Bearer "+token},{"Accept","application/json"}};}
 json get(const std::string& p){return parsebytes(request("GET","https://api.pixellab.ai/v2"+p,auth(),{}).data);}
 json detail(const std::string&id,bool force=false){idcheck(id);if(force||!details.count(id)){auto d=get("/characters/"+id);auto inner=member(d,"character");if(inner.is_object())d=inner;details[id]=d;}return details[id];}
 void invalidate(const std::string&id){details.erase(id);zips.erase(id);rotations.erase(id);}
 Bytes zip(const std::string&id,const Progress& progress){if(!zips.count(id)){progress("Downloading PixelLab export...");try{zips[id]=request("GET","https://api.pixellab.ai/v2/characters/"+id+"/zip?states="+id,{{"Accept","application/zip"}},{}).data;}catch(const HttpError&e){if(e.status==423)throw std::runtime_error("PixelLab is still generating; try again shortly");throw;}}return zips[id];}
 json state(Archive& a,const std::string&id){auto meta=parsebytes(a.get("metadata.json",16*1024*1024));auto states=member(meta,"states",json::array());json found=nullptr;int n=0;if(states.is_array())for(auto&s:states)if(text(member(s,"character"),"id")==id){found=s;n++;}if(n!=1)throw std::runtime_error("Export does not contain exactly one matching character");return found;}
 std::map<std::string,Bytes> rotation(const std::string&id,bool force,const Progress& progress){auto d=detail(id,force);if(!force&&rotations.count(id))return rotations[id];auto urls=member(d,"rotation_urls",json::object());if(!urls.is_object()||urls.empty())throw std::runtime_error("No finished rotations");std::map<std::string,Bytes> result;
  try{for(auto&dir:DIRS)if(urls.contains(dir)&&urls[dir].is_string()){auto u=urls[dir].get<std::string>();auto data=request("GET",u,{},{}).data;decodepng(data);result[dir]=std::move(data);}}catch(...){result.clear();progress("Direct images unavailable; using PixelLab export...");auto bytes=zip(id,progress);Archive a(bytes);auto frames=member(state(a,id),"frames",json::object());auto r=member(frames,"rotations",json::object());if(r.is_object())for(auto it=r.begin();it!=r.end();++it){auto dir=direction(it.key());if(isdir(dir)&&it.value().is_string())result[dir]=a.get(it.value().get<std::string>());}}
  if(result.empty())throw std::runtime_error("No rotation images found");rotations[id]=result;return result;
 }
 json group(const json&d,const std::string& gid){idcheck(gid);json found;int n=0;auto groups=member(d,"animations",json::array());if(groups.is_array())for(auto&g:groups)if(lower(text(g,"animation_group_id"))==lower(gid)){found=g;n++;}if(n!=1)throw std::runtime_error("Animation ID is unavailable. Refresh and reopen it.");return found;}
 struct Clip{std::string dir;std::vector<Img> images;std::vector<json> skeletons;};
 std::vector<Clip> clips(const std::string&id,const std::string&gid,const std::vector<std::string>&wanted,const Progress& progress,bool* fallback=nullptr){if(fallback)*fallback=false;auto d=detail(id);auto g=group(d,gid);auto rows=grouprows(g);if(wanted.empty()||wanted.size()>8)throw std::runtime_error("Choose one or more directions");std::set<std::string> unique;for(auto&dir:wanted)if(!isdir(dir)||!rows.count(dir)||!unique.insert(dir).second)throw std::runtime_error("Invalid or repeated animation direction");std::vector<Clip> out;int w=0,h=0;size_t totalbytes=0;
  try{for(auto&dir:wanted){Clip c;c.dir=dir;auto frames=member(rows[dir],"frames",json::array());if(!frames.is_array()||frames.empty()||frames.size()>200)throw std::runtime_error("Frame URLs unavailable");for(auto&v:frames){if(!v.is_string())throw std::runtime_error("Invalid frame URL");auto im=decodepng(request("GET",v.get<std::string>(),{},{}).data);if(!w){w=im.w;h=im.h;}if(im.w!=w||im.h!=h)throw std::runtime_error("Animation frame sizes differ");totalbytes+=im.rgba.size();if(totalbytes>32*1024*1024)throw std::runtime_error("Animation is too large");c.images.push_back(std::move(im));c.skeletons.push_back(nullptr);}out.push_back(std::move(c));}return out;}
  catch(...){if(fallback)*fallback=true;progress("Direct frames unavailable; checking export...");out.clear();w=h=0;totalbytes=0;auto bytes=zip(id,progress);Archive a(bytes);auto st=state(a,id);auto exported=member(member(st,"frames",json::object()),"animations",json::object());auto m=matches(exported,member(d,"animations",json::array()));std::string name;int n=0;for(auto&[k,v]:m)if(v==lower(gid)){name=k;n++;}if(n!=1)throw std::runtime_error("Cannot match the animation export safely. No changes saved.");auto ds=exported[name];for(auto&dir:wanted){json paths=nullptr;if(ds.contains(dir))paths=ds[dir];if(!paths.is_array()||paths.empty()||paths.size()>200)throw std::runtime_error("Direction missing from export");Clip c;c.dir=dir;json kps=nullptr;for(auto container:{member(st,"keypoints"),member(member(st,"frames"),"keypoints")}){if(!container.is_object())continue;auto node=member(container,"animations",container);if(node.is_object()&&node.contains(name)&&node[name].is_object()&&node[name].contains(dir)&&node[name][dir].is_array()){kps=node[name][dir];break;}}size_t frameidx=0;for(auto&p:paths){if(!p.is_string())throw std::runtime_error("Invalid export path");auto im=decodepng(a.get(p.get<std::string>()));if(!w){w=im.w;h=im.h;}if(im.w!=w||im.h!=h)throw std::runtime_error("Animation frame sizes differ");totalbytes+=im.rgba.size();if(totalbytes>32*1024*1024)throw std::runtime_error("Animation is too large");json sk=nullptr;if(kps.is_array()&&frameidx<kps.size())sk=skeleton(kps[frameidx],im.w,im.h);c.images.push_back(std::move(im));c.skeletons.push_back(sk);frameidx++;}out.push_back(std::move(c));}return out;}
 }
 json rpc(const std::string& method,const json& params,bool notification=false){json msg={{"jsonrpc","2.0"},{"method",method},{"params",params}};if(!notification)msg["id"]=1;auto h=auth();h["Content-Type"]="application/json";h["Accept"]="application/json, text/event-stream";h["MCP-Protocol-Version"]="2025-03-26";if(!session.empty())h["Mcp-Session-Id"]=session;auto reply=request("POST","https://api.pixellab.ai/mcp",h,strbytes(msg.dump()));if(reply.headers.count("mcp-session-id"))session=reply.headers["mcp-session-id"];if(notification)return json::object();std::string s(reply.data.begin(),reply.data.end());if(s.find("data:")!=std::string::npos){std::stringstream lines(s);std::string line,last;while(std::getline(lines,line))if(line.rfind("data:",0)==0)last=line.substr(5);s=last;}auto obj=json::parse(s);if(obj.contains("error"))throw std::runtime_error("PixelLab MCP error: "+text(obj["error"],"message","unknown"));return member(obj,"result",json::object());}
 json mcp(const std::string&name,const json& args){for(int attempt=0;attempt<2;attempt++){if(session.empty()){rpc("initialize",{{"protocolVersion","2025-03-26"},{"capabilities",json::object()},{"clientInfo",{{"name","aseprite-pixellab-live-cpp"},{"version",VER}}}});rpc("notifications/initialized",json::object(),true);}try{return rpc("tools/call",{{"name",name},{"arguments",args}});}catch(const HttpError&e){if(attempt==0&&(e.status==400||e.status==401||e.status==404)){session.clear();continue;}throw;}}throw std::runtime_error("MCP session failed");}
 void save(const std::string&target,const Bytes& bytes,const std::string& mime){auto res=mcp("save_to_asset",{{"image","data:"+mime+";base64,"+base64(bytes)},{"target",target}});if(flag(res,"isError")){std::string msg;auto c=member(res,"content",json::array());if(c.is_array())for(auto&x:c){if(!msg.empty())msg+="; ";msg+=text(x,"text");}throw std::runtime_error(msg.empty()?"PixelLab refused the save":msg.substr(0,300));}}
 int estimate(std::vector<json*>& frames,const Progress&progress){int total=0;for(auto*f:frames)if(!f->contains("skeleton"))total++;if(total)progress("Estimating " + std::to_string(total) + " skeletons...");int done=0;for(auto*f:frames){if(f->contains("skeleton"))continue;auto im=member(*f,"image");int w=num(im,"w"),h=num(im,"h");auto rgba=unhex(text(im,"hex"));bool nonzero=false;for(size_t i=3;i<rgba.size();i+=4)if(rgba[i]){nonzero=true;break;}if(!nonzero)continue;auto png=encodepng(w,h,rgba);auto body=json({{"image",{{"type","base64"},{"base64",base64(png)}}}}).dump();auto hds=auth();hds["Content-Type"]="application/json";auto response=parsebytes(request("POST","https://api.pixellab.ai/v2/estimate-skeleton",hds,strbytes(body)).data);auto sk=skeleton(member(response,"keypoints"),w,h);if(!sk.is_null()){(*f)["skeleton"]=sk;done++;}}return done;}
public:
 json job(const json&,const Progress&);
};
json Service::job(const json&m,const Progress&progress){
 auto act=text(m,"action");
 if(act=="auth") {auto key=text(m,"token");while(!key.empty()&&std::isspace((unsigned char)key.back()))key.pop_back();auto it=std::find_if_not(key.begin(),key.end(),[](unsigned char c){return std::isspace(c);});key.erase(key.begin(),it);if(key.empty())throw std::runtime_error("Missing API key");if(key.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("Invalid API key");auto old=token;token=key;json bal;try{bal=get("/balance");}catch(...){token=old;throw;}if(old!=key){characters=nullptr;details.clear();rotations.clear();zips.clear();session.clear();undo.clear();}auto sub=member(bal,"subscription",json::object());auto gen=member(sub,"generations");std::string info;if(!gen.is_null())info=(gen.is_string()?gen.get<std::string>():gen.dump())+" generations left";return {{"info",info}};}
 if(act=="list"){progress("Loading characters...");if(characters.is_null()||flag(m,"force")){json all=json::array();size_t seen=0;for(int off=0;off<2000;off+=100){auto data=get("/characters?limit=100&offset="+std::to_string(off));auto rows=data.is_array()?data:member(data,"characters",json::array());if(!rows.is_array())throw std::runtime_error("Invalid character list");seen+=rows.size();for(auto&c:rows){auto id=text(c,"id",text(c,"character_id"));try{idcheck(id);}catch(...){continue;}auto sz=member(c,"size",json::object());all.push_back({{"id",id},{"label",label(c)},{"w",member(sz,"width")},{"h",member(sz,"height")},{"directions",member(c,"directions")},{"animations",member(c,"animation_count")}});}int total=num(data,"total",(int)seen);if(rows.empty()||(int)seen>=total)break;}characters=all;}return {{"characters",characters}};}
 auto id=act=="undo"?undochar:text(m,"character_id");if(act!="undo"||!id.empty())idcheck(id);
 if(act=="select"||act=="open_rotations"){progress("Loading character...");auto d=detail(id,flag(m,"force"));auto imgs=rotation(id,flag(m,"force"),progress);auto sk=rotationskeletons(d);json previews=json::array(),frames=json::array();std::vector<Img> decoded;int w=0,h=0;bool has=false;for(auto&dir:DIRS)if(imgs.count(dir)){auto im=decodepng(imgs[dir]);if(!w){w=im.w;h=im.h;}if(im.w!=w||im.h!=h)throw std::runtime_error("Rotation sizes do not match");if((uint64_t)w*h*4*imgs.size()>32ULL*1024*1024)throw std::runtime_error("Character is too large");auto p=payload(im);p["direction"]=dir;previews.push_back(p);json f={{"direction",dir},{"image",payload(im)}};if(sk.contains(dir)&&!sk[dir].is_null()&&!sk[dir].empty()){has=true;auto s=skeleton(sk[dir],w,h);if(!s.is_null())f["skeleton"]=s;}frames.push_back(std::move(f));decoded.push_back(std::move(im));}if(act=="select"){json dirs=json::array();for(auto&p:previews)dirs.push_back(p["direction"]);return {{"id",id},{"label",label(d)},{"w",w},{"h",h},{"directions",dirs},{"previews",previews},{"has_skeleton",has},{"updated_at",text(d,"updated_at",text(d,"created_at"))}};}int estimated=0;if(flag(m,"estimate")){std::vector<json*> a;for(auto&f:frames)a.push_back(&f);estimated=estimate(a,progress);}return {{"id",id},{"label",label(d)},{"w",w},{"h",h},{"frames",frames},{"palette",palette(decoded)},{"estimated",estimated}};}
 if(act=="animations"){auto d=detail(id);auto groups=member(d,"animations",json::array());if((!groups.is_array()||groups.empty())&&num(d,"animation_count")>0)throw std::runtime_error("PixelLab did not provide animation IDs");json out=json::array();if(groups.is_array())for(auto&g:groups){auto gid=lower(text(g,"animation_group_id"));try{idcheck(gid);}catch(...){continue;}auto rows=grouprows(g);json dirs=json::array();int maxframes=0;for(auto&dir:DIRS)if(rows.count(dir)){auto f=member(rows[dir],"frames",json::array());int n=num(rows[dir],"frame_count",f.is_array()?(int)f.size():0);if(n>0){dirs.push_back(dir);maxframes=std::max(maxframes,n);}}if(dirs.empty())continue;out.push_back({{"name",text(g,"display_name",text(g,"animation_type","Animation"))},{"directions",dirs},{"frames",maxframes},{"group_id",gid}});}std::sort(out.begin(),out.end(),[](const json&a,const json&b){auto an=lower(text(a,"name")),bn=lower(text(b,"name"));return an!=bn?an<bn:text(a,"group_id")<text(b,"group_id");});return {{"id",id},{"animations",out}};}
 if(act=="anim_preview"||act=="open_animation"){auto gid=lower(text(m,"group_id"));std::vector<std::string>wanted;if(act=="anim_preview")wanted.push_back(direction(text(m,"direction")));else{auto d=member(m,"directions",json::array());if(d.is_array())for(auto&v:d)if(v.is_string())wanted.push_back(direction(v.get<std::string>()));}bool fallback=false;auto cs=clips(id,gid,wanted,progress,&fallback);if(act=="anim_preview"){json frames=json::array();for(auto&im:cs[0].images)frames.push_back(payload(im));return {{"id",id},{"name",text(m,"name")},{"direction",wanted[0]},{"frames",frames}};}json out=json::array();std::vector<Img> all;for(auto&c:cs){json f=json::array();for(size_t i=0;i<c.images.size();i++){json x={{"image",payload(c.images[i])}};if(i<c.skeletons.size()&&!c.skeletons[i].is_null())x["skeleton"]=c.skeletons[i];f.push_back(std::move(x));all.push_back(c.images[i]);}out.push_back({{"direction",c.dir},{"frames",f}});}int estimated=0;if(flag(m,"estimate")){std::vector<json*> a;for(auto&c:out)for(auto&f:c["frames"])a.push_back(&f);estimated=estimate(a,progress);}auto d=detail(id);return {{"id",id},{"label",label(d)},{"name",text(m,"name")},{"group_id",gid},{"fallback_match",fallback},{"w",all[0].w},{"h",all[0].h},{"clips",out},{"palette",palette(all)},{"estimated",estimated}};}
 if(act=="resolve_legacy"){auto d=detail(id);auto bytes=zip(id,progress);Archive a(bytes);auto st=state(a,id);auto exported=member(member(st,"frames",json::object()),"animations",json::object());auto mapping=matches(exported,member(d,"animations",json::array()));auto name=text(m,"name");if(!mapping.count(name))throw std::runtime_error("Reopen this animation from the PixelLab panel to link it safely");return {{"group_id",mapping[name]}};}
 if(act=="upload") {auto items=member(m,"items",json::array());if(!items.is_array()||items.empty()||items.size()>8)throw std::runtime_error("Choose 1-8 directions");invalidate(id);auto d=detail(id);auto old=rotation(id,false,progress);auto sz=member(d,"size",json::object());int w=num(sz,"width"),h=num(sz,"height");struct New{std::string dir;Bytes png;};std::vector<New> prepared;std::set<std::string> seen;for(auto&x:items){auto k=direction(text(x,"direction"));if(!isdir(k)||!old.count(k)||!seen.insert(k).second)throw std::runtime_error("Invalid or repeated direction");if(num(x,"w")!=w||num(x,"h")!=h)throw std::runtime_error("Canvas size must not change");require_original(x,decodepng(old.at(k)).rgba);prepared.push_back({k,encodepng(w,h,unhex(text(x,"hex")))});}json saved=json::array(),failed=json::array();std::vector<UndoItem> backup;for(auto&x:prepared){progress("Saving " + x.dir + "...");auto target="character:"+id+":"+x.dir;try{save(target,x.png,"image/png");saved.push_back(x.dir);backup.push_back({x.dir,target,"image/png",old.at(x.dir)});}catch(const std::exception&e){failed.push_back({{"direction",x.dir},{"error",std::string(e.what()).substr(0,300)}});}}if(!backup.empty()){undo=backup;undochar=id;}invalidate(id);return {{"saved",saved},{"failed",failed}};}
 if(act=="upload_animation"){auto gid=lower(text(m,"group_id"));idcheck(gid);auto items=member(m,"items",json::array());if(!items.is_array()||items.empty()||items.size()>8)throw std::runtime_error("Choose 1-8 directions");std::vector<std::string>wanted;std::set<std::string>seen;for(auto&x:items){auto k=direction(text(x,"direction"));if(!isdir(k)||!seen.insert(k).second)throw std::runtime_error("Invalid or repeated direction");wanted.push_back(k);}invalidate(id);auto old=clips(id,gid,wanted,progress);int w=old[0].images[0].w,h=old[0].images[0].h;struct New{std::string dir;Bytes gifdata;};std::vector<New> prepared;size_t oldidx=0;for(auto&x:items){require_original_frames(x,old.at(oldidx++).images);if(num(x,"w")!=w||num(x,"h")!=h)throw std::runtime_error("Canvas size must not change");auto f=member(x,"frames",json::array());if(!f.is_array())throw std::runtime_error("Invalid animation frames");std::vector<Bytes>b;for(auto&s:f){if(!s.is_string())throw std::runtime_error("Invalid animation frame");b.push_back(unhex(s.get<std::string>()));}prepared.push_back({direction(text(x,"direction")),gif(w,h,b,num(x,"delay_cs",10))});}json saved=json::array(),failed=json::array();for(auto&x:prepared){progress("Saving animation " + x.dir + "...");auto target="character:"+id+":"+x.dir+":"+gid;try{save(target,x.gifdata,"image/gif");saved.push_back(x.dir);}catch(const std::exception&e){failed.push_back({{"direction",x.dir},{"error",std::string(e.what()).substr(0,300)}});}}if(!saved.empty())undo.clear();invalidate(id);return {{"saved",saved},{"failed",failed}};}
 if(act=="undo"){if(undo.empty())throw std::runtime_error("Nothing to undo in this session");json restored=json::array(),failed=json::array();std::vector<UndoItem> remain;for(auto&x:undo){progress("Restoring " + x.dir + "...");try{save(x.target,x.bytes,x.mime);restored.push_back(x.dir);}catch(const std::exception&e){failed.push_back({{"direction",x.dir},{"error",std::string(e.what()).substr(0,300)}});remain.push_back(x);}}undo=remain;invalidate(undochar);return {{"character_id",undochar},{"restored",restored},{"failed",failed}};}
 throw std::runtime_error("Unsupported operation");
}

namespace app::pixellab {
struct Client::Impl { Service service; std::mutex mutex; };
Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() = default;
std::string Client::run(const std::string& requestJson, const ProgressFn& progress) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  try {
    auto request = json::parse(requestJson);
    if (!request.is_object()) throw std::runtime_error("Invalid request");
    auto result = impl_->service.job(request, [&](const std::string& msg) { if (progress) progress(msg); });
    result["ok"] = true;
    return result.dump(-1, ' ', true);
  } catch (const std::exception& e) {
    return json({{"ok",false},{"error",std::string(e.what()).substr(0,700)}}).dump(-1, ' ', true);
  }
}
}
