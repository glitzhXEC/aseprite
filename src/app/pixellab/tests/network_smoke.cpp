// Test transport only; sends no credentials and performs no cloud writes.
#include "../client.cpp"
#include <iostream>
int main(){
 try {
  bool blocked=false; try{request("GET","http://example.com",{},{});}catch(const std::exception&){blocked=true;} if(!blocked)return 1;
  blocked=false;try{request("GET","https://example.com",{{"Authorization","Bearer synthetic"}},{});}catch(const std::exception&){blocked=true;}if(!blocked)return 2;
  auto r=request("GET","https://api.pixellab.ai/",{},{});std::cout<<"HTTPS status "<<r.status<<"; credential host guard OK\n";return 0;
 }catch(const HttpError& e){std::cout<<"HTTPS returned HTTP "<<e.status<<"; credential host guard OK\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 3;}
}
