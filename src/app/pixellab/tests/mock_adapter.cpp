#define PIXELLAB_TEST_MOCK
#include "../client.cpp"
#include <cstdlib>
static Reply request(const std::string&,const std::string& url,const std::map<std::string,std::string>&,const Bytes& body){
 auto response=[](json j){auto s=j.dump();return Reply{200,{},Bytes(s.begin(),s.end())};};
 const std::string id="11111111-1111-4111-8111-111111111111",gid="22222222-2222-4222-8222-222222222222";
 if(url.find("/balance")!=std::string::npos)return response({{"subscription",{{"generations",42}}}});
 if(url.find("/characters?")!=std::string::npos)return response({{"characters",json::array({{{"id",id},{"name","Synthetic test"},{"size",{{"width",8},{"height",8}}}}})},{"total",1}});
 if(url.find("/zip?")!=std::string::npos){std::ifstream f(std::getenv("PIXELLAB_FIXTURE"),std::ios::binary);std::string s((std::istreambuf_iterator<char>(f)),{});return {200,{},Bytes(s.begin(),s.end())};}
 if(url.find("/characters/")!=std::string::npos){json urls=json::object();for(auto&d:DIRS)urls[d]="https://images.test/"+d;json dirs=json::array();for(auto d:{"south","south-east"}){json row={{"direction",d},{"frame_count",4}};if(std::getenv("PIXELLAB_MOCK_DIRECT")){row["frames"]=json::array();for(int i=0;i<4;i++)row["frames"].push_back(std::string("https://images.test/anim/")+d+"/"+std::to_string(i));}dirs.push_back(row);}return response({{"id",id},{"name","Synthetic test"},{"size",{{"width",8},{"height",8}}},{"rotation_urls",urls},{"animations",json::array({{{"animation_group_id",gid},{"display_name","custom-Walking"},{"animation_type","walking"},{"directions",dirs}}})}});}
 if(url.find("/estimate-skeleton")!=std::string::npos)return response({{"keypoints",json::array({{{"label","NECK"},{"x",0.5},{"y",0.3}},{{"label","NOSE"},{"x",0.5},{"y",0.2}}})}});
 if(url.find("https://images.test/anim/")==0 && std::getenv("PIXELLAB_MOCK_DIRECT")){std::ifstream f(std::getenv("PIXELLAB_FIXTURE"),std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(f)),{});Bytes b(bytes.begin(),bytes.end());Archive a(b);auto path=url.substr(std::string("https://images.test/").size())+".png";return {200,{},a.get(path)};}
 if(url.find("images.test")!=std::string::npos)throw HttpError(403);
 if(url.find("/mcp")!=std::string::npos){auto req=parsebytes(body);if(text(req,"method")=="initialize")return {200,{{"mcp-session-id","test"}},strbytes("{\"result\":{}}")};if(text(req,"method")=="notifications/initialized")return {202,{}, {}};auto args=member(member(req,"params"),"arguments");std::ofstream o(std::getenv("PIXELLAB_CAPTURE"),std::ios::app);o<<args.dump()<<"\n";return response({{"result",{{"content",json::array()}}}});}
 throw std::runtime_error("Unexpected synthetic request");
}
#ifdef _WIN32
#define PIXELLAB_TEST_EXPORT __declspec(dllexport)
#else
#define PIXELLAB_TEST_EXPORT
#endif
extern "C" PIXELLAB_TEST_EXPORT void* make_client(){return new app::pixellab::Client();}
extern "C" PIXELLAB_TEST_EXPORT void free_client(void* p){delete static_cast<app::pixellab::Client*>(p);}
extern "C" PIXELLAB_TEST_EXPORT const char* run_job(void* p,const char* msg){static thread_local std::string r;r=static_cast<app::pixellab::Client*>(p)->run(msg);return r.c_str();}
